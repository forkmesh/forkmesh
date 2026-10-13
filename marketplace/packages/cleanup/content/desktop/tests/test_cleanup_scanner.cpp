#include "CleanupPlan.h"
#include "CleanupScanner.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <cstdlib>
#include <functional>

#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace forkmesh::cleanup;

namespace {

int failures = 0;
QString gRoot;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

void touch(const QString &path, const QByteArray &body = QByteArray(4096, 'x'))
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(body);
}

void age(const QString &path, int days)
{
#if defined(Q_OS_UNIX)
    const qint64 seconds = QDateTime::currentSecsSinceEpoch() - qint64(days) * 24 * 3600;
    struct timespec times[2];
    times[0].tv_sec = seconds;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    QStringList paths;
    if (QFileInfo(path).isDir() && !QFileInfo(path).isSymLink()) {
        QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            paths << it.next();
    }
    std::sort(paths.begin(), paths.end(), [](const QString &a, const QString &b) {
        return a.count(QLatin1Char('/')) > b.count(QLatin1Char('/'));
    });
    paths << path;
    for (const QString &entry : std::as_const(paths))
        ::utimensat(AT_FDCWD, QFile::encodeName(entry).constData(), times, AT_SYMLINK_NOFOLLOW);
#else
    Q_UNUSED(path);
    Q_UNUSED(days);
#endif
}

bool run(const QString &dir, const QStringList &args)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.start(QStringLiteral("git"),
                  QStringList{QStringLiteral("-c"), QStringLiteral("user.name=Cleanup Test"),
                              QStringLiteral("-c"), QStringLiteral("user.email=cleanup@test.invalid"),
                              QStringLiteral("-c"), QStringLiteral("commit.gpgsign=false")} +
                      args);
    process.waitForFinished(30000);
    if (process.exitCode() != 0)
        std::printf("git %s: %s\n", qPrintable(args.join(QLatin1Char(' '))),
                    process.readAllStandardError().constData());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

const Item *find(const QList<Item> &items, Category category, const QString &title)
{
    for (const Item &item : items) {
        if (item.category == category && item.title == title)
            return &item;
    }
    return nullptr;
}

bool hasMember(const Item *item, const QString &path)
{
    if (!item)
        return false;
    for (const Member &member : item->members) {
        if (member.path == QDir::cleanPath(path))
            return true;
    }
    return false;
}

Context baseContext()
{
    Context context;
    context.home = gRoot + QStringLiteral("/home");
    context.tmpRoots = {gRoot + QStringLiteral("/tmp")};
#if defined(Q_OS_UNIX)
    context.uid = QString::number(::getuid());
#endif
    context.projectHome = context.home + QStringLiteral("/projects/forkmesh");
    context.dataDir = context.projectHome + QStringLiteral("/data");
    context.buildsDir = context.projectHome + QStringLiteral("/builds");
    context.worktreesDir = context.projectHome + QStringLiteral("/worktrees");
    context.sandboxDir = context.dataDir + QStringLiteral("/actions/sandbox");
    context.runCcache = false;
    context.nowMs = QDateTime::currentMSecsSinceEpoch();
    return context;
}

const QString kLive = QStringLiteral("11111111-2222-3333-4444-555555555555");
const QString kOld = QStringLiteral("aaaaaaaa-0000-0000-0000-000000000001");
const QString kRecent = QStringLiteral("aaaaaaaa-0000-0000-0000-000000000002");
const QString kGone = QStringLiteral("aaaaaaaa-0000-0000-0000-000000000003");

void testHelpers()
{
    check(encodeClaudeProject(QStringLiteral("/home/f/projects/forkmesh/src/.worktrees/x_y")) ==
              QStringLiteral("-home-f-projects-forkmesh-src--worktrees-x-y"),
          "Claude project folders encode every non-alphanumeric character as a dash");
    check(uuidsIn(QStringLiteral("--resume %1 x").arg(kLive)).contains(kLive),
          "session ids are read out of command lines");
    check(pathWithin(QStringLiteral("/a/b/c"), QStringLiteral("/a/b")) &&
              !pathWithin(QStringLiteral("/a/bc"), QStringLiteral("/a/b")),
          "path containment respects folder boundaries");

    const QString dir = gRoot + QStringLiteral("/measure");
    touch(dir + QStringLiteral("/a.bin"), QByteArray(64 * 1024, 'a'));
    touch(dir + QStringLiteral("/sub/b.bin"), QByteArray(64 * 1024, 'b'));
    const QString outside = gRoot + QStringLiteral("/outside");
    touch(outside + QStringLiteral("/big.bin"), QByteArray(512 * 1024, 'c'));
    QFile::link(outside, dir + QStringLiteral("/link"));
    const Usage usage = measure(dir);
    check(usage.files == 3 && usage.bytes >= 128 * 1024 && usage.bytes < 512 * 1024,
          "measure counts files below the folder and never follows a link out of it");
}

void testProcesses()
{
    const QString proc = gRoot + QStringLiteral("/proc");
    const QString target = gRoot + QStringLiteral("/home/.claude/projects/p");
    QDir().mkpath(target);
    QDir().mkpath(proc + QStringLiteral("/4242/fd"));
    QFile::link(target, proc + QStringLiteral("/4242/cwd"));
    QFile::link(gRoot + QStringLiteral("/home/.local/share/claude/versions/2.0.0"),
                proc + QStringLiteral("/4242/exe"));
    QFile::link(gRoot + QStringLiteral("/tmp/open.log"), proc + QStringLiteral("/4242/fd/3"));
    touch(proc + QStringLiteral("/4242/comm"), "claude\n");
    touch(proc + QStringLiteral("/4242/cmdline"),
          QByteArray("claude\0--resume\0") + kLive.toUtf8() + QByteArray("\0", 1));
    QDir().mkpath(proc + QStringLiteral("/self"));
    const ProcessSnapshot snapshot = readProcesses(proc);
    check(snapshot.processes.size() == 1 && snapshot.processes.first().pid == 4242,
          "only numeric process folders are read");
    check(!snapshot.usersOf(target).isEmpty(), "a process whose cwd is inside a path uses it");
    check(!snapshot.usersOf(gRoot + QStringLiteral("/tmp")).isEmpty(),
          "an open file descriptor marks its folder in use");
    check(snapshot.usersOf(gRoot + QStringLiteral("/home/.codex")).isEmpty(),
          "unrelated folders are not in use");
    check(isClaudeProcess(snapshot.processes.first()), "claude processes are recognised");
}

ProcessSnapshot claudeSnapshot(const QString &cwd)
{
    ProcessSnapshot snapshot;
    Process process;
    process.pid = 77;
    process.comm = QStringLiteral("claude");
    process.cwd = cwd;
    process.cmdline = {QStringLiteral("claude"), QStringLiteral("--resume"), kLive};
    snapshot.processes << process;
    snapshot.available = true;
    return snapshot;
}

void testClaude()
{
    Context context = baseContext();
    const QString folder = context.home + QStringLiteral("/code/app");
    QDir().mkpath(folder);
    const QString projects = context.home + QStringLiteral("/.claude/projects");
    const QString kept = projects + QLatin1Char('/') + encodeClaudeProject(folder);
    const QByteArray line = QStringLiteral("{\"cwd\":\"%1\",\"type\":\"user\"}\n").arg(folder).toUtf8();
    touch(kept + QLatin1Char('/') + kOld + QStringLiteral(".jsonl"), line);
    touch(kept + QLatin1Char('/') + kOld + QStringLiteral("/subagents/a.jsonl"), line);
    touch(kept + QLatin1Char('/') + kRecent + QStringLiteral(".jsonl"), line);
    touch(kept + QLatin1Char('/') + kLive + QStringLiteral(".jsonl"), line);
    touch(kept + QStringLiteral("/memory/MEMORY.md"), "notes");
    const QString goneFolder = context.home + QStringLiteral("/code/removed");
    const QString gone = projects + QLatin1Char('/') + encodeClaudeProject(goneFolder);
    touch(gone + QLatin1Char('/') + kGone + QStringLiteral(".jsonl"),
          QStringLiteral("{\"cwd\":\"%1\"}\n").arg(goneFolder).toUtf8());
    age(kept + QLatin1Char('/') + kOld + QStringLiteral(".jsonl"), 60);
    age(kept + QLatin1Char('/') + kOld, 60);
    age(kept + QLatin1Char('/') + kRecent + QStringLiteral(".jsonl"), 2);
    age(kept + QLatin1Char('/') + kLive + QStringLiteral(".jsonl"), 3);
    age(kept + QStringLiteral("/memory"), 90);
    age(gone + QLatin1Char('/') + kGone + QStringLiteral(".jsonl"), 5);

    const QString scratch = context.tmpRoots.first() + QStringLiteral("/claude-") + context.uid +
                            QLatin1Char('/') + encodeClaudeProject(folder);
    touch(scratch + QLatin1Char('/') + kLive + QStringLiteral("/scratchpad/notes.txt"));
    touch(scratch + QLatin1Char('/') + kOld + QStringLiteral("/tasks/out.txt"));
    age(scratch + QLatin1Char('/') + kLive, 1);
    age(scratch + QLatin1Char('/') + kOld, 1);

    const QString versions = context.home + QStringLiteral("/.local/share/claude/versions");
    touch(versions + QStringLiteral("/2.0.0"));
    touch(versions + QStringLiteral("/1.9.0"));
    age(versions + QStringLiteral("/1.9.0"), 10);
    QDir().mkpath(context.home + QStringLiteral("/.local/bin"));
    QFile::link(versions + QStringLiteral("/2.0.0"), context.home + QStringLiteral("/.local/bin/claude"));
    context.claudeBinary = context.home + QStringLiteral("/.local/bin/claude");

    const QString account = context.dataDir + QStringLiteral("/agent-accounts/claude/abcdef12-0000/projects/p");
    touch(account + QStringLiteral("/s.jsonl"));
    age(account, 40);

    context.processes = claudeSnapshot(context.home);
    const ScanResult result = scan(context);
    const QString label = folder;
    const Item *old = find(result.items, Category::Claude, QStringLiteral("Old transcripts · ") + label);
    const Item *current = find(result.items, Category::Claude, QStringLiteral("Transcripts · ") + label);
    check(old && old->status == Status::Orphaned &&
              hasMember(old, kept + QLatin1Char('/') + kOld + QStringLiteral(".jsonl")) &&
              hasMember(old, kept + QLatin1Char('/') + kOld),
          "sessions older than the cut-off are orphaned even while their folder exists");
    check(current && current->status == Status::Attached &&
              hasMember(current, kept + QLatin1Char('/') + kRecent + QStringLiteral(".jsonl")) &&
              current->reasonText().contains(QStringLiteral("still exists")),
          "recent sessions of an existing folder are attached to it");
    check(current && !hasMember(current, kept + QLatin1Char('/') + kLive + QStringLiteral(".jsonl")) &&
              current->reasonText().contains(QStringLiteral("live session")),
          "the transcript of a running session is never a member");
    bool memoryListed = false;
    for (const Item &item : result.items) {
        for (const Member &member : item.members)
            memoryListed = memoryListed || member.path.contains(QStringLiteral("/memory"));
    }
    check(!memoryListed, "a project's memory folder is never offered for deletion");
    const Item *goneItem = find(result.items, Category::Claude, QStringLiteral("Transcripts · ") + goneFolder);
    check(goneItem && goneItem->status == Status::Orphaned &&
              goneItem->reasonText().contains(QStringLiteral("is gone")),
          "transcripts of a folder that is gone are orphaned");
    const Item *scratchItem = find(result.items, Category::Claude,
                                   QStringLiteral("Scratchpads · ") + encodeClaudeProject(folder));
    check(scratchItem && hasMember(scratchItem, scratch + QLatin1Char('/') + kOld) &&
              !hasMember(scratchItem, scratch + QLatin1Char('/') + kLive),
          "the scratchpad of a live session is kept, finished ones can go");
    const Item *current2 = find(result.items, Category::Claude, QStringLiteral("Claude Code 2.0.0"));
    const Item *older = find(result.items, Category::Claude, QStringLiteral("Claude Code 1.9.0"));
    check(current2 && current2->status == Status::Protected && !current2->deletable(),
          "the Claude Code version `claude` runs is protected");
    check(older && older->status == Status::Orphaned && older->deletable(),
          "older Claude Code versions are orphaned");
    bool accountProtected = false;
    for (const Item &item : result.items) {
        if (item.title.startsWith(QStringLiteral("Desktop agent transcripts")))
            accountProtected = item.status == Status::Protected && !item.deletable() &&
                               item.members.isEmpty() && item.keptBytes > 0;
    }
    check(accountProtected, "desktop agent account transcripts are shown but protected");
}

void testCodex()
{
    Context context = baseContext();
    const QString codex = context.home + QStringLiteral("/.codex");
    touch(codex + QStringLiteral("/sessions/2026/09/01/rollout-a.jsonl"));
    touch(codex + QStringLiteral("/logs_2.sqlite"));
    touch(codex + QStringLiteral("/auth.json"), "{}");
    touch(codex + QStringLiteral("/packages/standalone/releases/0.1/codex"));
    touch(codex + QStringLiteral("/packages/standalone/releases/0.2/codex"));
    QFile::link(codex + QStringLiteral("/packages/standalone/releases/0.2"),
                codex + QStringLiteral("/packages/standalone/current"));
    age(codex, 20);
    ScanResult result = scan(context);
    const Item *sessions = find(result.items, Category::Codex, QStringLiteral("Codex sessions"));
    check(sessions && hasMember(sessions, codex + QStringLiteral("/sessions/2026/09/01")),
          "Codex session days are offered");
    const Item *logs = find(result.items, Category::Codex, QStringLiteral("Codex logs"));
    check(logs && logs->status == Status::Orphaned && hasMember(logs, codex + QStringLiteral("/logs_2.sqlite")),
          "Codex logs can go while Codex is not running");
    bool authListed = false;
    for (const Item &item : result.items)
        authListed = authListed || hasMember(&item, codex + QStringLiteral("/auth.json"));
    check(!authListed, "Codex sign-in and settings are never listed");
    const Item *current = find(result.items, Category::Codex, QStringLiteral("Codex 0.2"));
    const Item *old = find(result.items, Category::Codex, QStringLiteral("Codex 0.1"));
    check(current && current->status == Status::Protected && old && old->deletable(),
          "the current Codex release is protected and older ones are not");

    Process codexProcess;
    codexProcess.pid = 9;
    codexProcess.comm = QStringLiteral("codex");
    context.processes.processes << codexProcess;
    result = scan(context);
    logs = find(result.items, Category::Codex, QStringLiteral("Codex logs"));
    check(logs && logs->status == Status::InUse && !logs->deletable(),
          "Codex logs are in use while Codex runs");
}

struct GitFixture {
    QString repo;
    QString worktrees;
};

GitFixture makeRepo(const Context &context)
{
    GitFixture fixture;
    fixture.repo = context.home + QStringLiteral("/projects/app");
    fixture.worktrees = fixture.repo + QStringLiteral("/.worktrees");
    QDir().mkpath(fixture.repo);
    run(fixture.repo, {QStringLiteral("init"), QStringLiteral("-q"), QStringLiteral("-b"), QStringLiteral("main")});
    touch(fixture.repo + QStringLiteral("/README"), "hello\n");
    touch(fixture.repo + QStringLiteral("/.gitignore"), ".worktrees\nnode_modules\n");
    run(fixture.repo, {QStringLiteral("add"), QStringLiteral(".")});
    run(fixture.repo, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("init")});
    for (const QString &name : {QStringLiteral("clean"), QStringLiteral("dirty"), QStringLiteral("ahead"),
                                QStringLiteral("agent"), QStringLiteral("live"), QStringLiteral("task"),
                                QStringLiteral("busy")})
        run(fixture.repo, {QStringLiteral("worktree"), QStringLiteral("add"), QStringLiteral("-q"),
                           QStringLiteral("-b"), name, fixture.worktrees + QLatin1Char('/') + name});
    touch(fixture.worktrees + QStringLiteral("/clean/node_modules/pkg/index.js"), QByteArray(100000, 'n'));
    touch(fixture.worktrees + QStringLiteral("/dirty/README"), "changed\n");
    touch(fixture.worktrees + QStringLiteral("/ahead/feature.txt"), "feature\n");
    run(fixture.worktrees + QStringLiteral("/ahead"), {QStringLiteral("add"), QStringLiteral(".")});
    run(fixture.worktrees + QStringLiteral("/ahead"),
        {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("feature")});
    touch(fixture.worktrees + QStringLiteral("/leftover/junk.txt"));
    age(fixture.worktrees, 3);
    return fixture;
}

void testWorktrees()
{
    if (QStandardPaths::findExecutable(QStringLiteral("git")).isEmpty()) {
        check(true, "git is missing; worktree checks skipped");
        return;
    }
    Context context = baseContext();
    const GitFixture fixture = makeRepo(context);
    context.repositories = {{QStringLiteral("me/app"), fixture.repo}};
    forkmesh::cleanupfeed::Agent finished;
    finished.id = 512;
    finished.status = QStringLiteral("success");
    finished.branch = QStringLiteral("agent");
    finished.repoPath = fixture.repo;
    forkmesh::cleanupfeed::Agent running;
    running.id = 513;
    running.status = QStringLiteral("running");
    running.worktree = fixture.worktrees + QStringLiteral("/live");
    running.live = true;
    context.agents = {finished, running};
    context.tasks = {{QStringLiteral("t-1"), QStringLiteral("Ship it"), QStringLiteral("me/app"),
                      QStringLiteral("task"), QString()}};

    QProcess sleeper;
    sleeper.setWorkingDirectory(fixture.worktrees + QStringLiteral("/busy"));
    sleeper.start(QStringLiteral("sleep"), {QStringLiteral("30")});
    sleeper.waitForStarted(5000);
    context.processes = readProcesses();
    const ScanResult result = scan(context);
    sleeper.kill();
    sleeper.waitForFinished(5000);

    const auto worktree = [&](const QString &name) {
        return find(result.items, Category::Worktrees, name);
    };
    const Item *clean = worktree(QStringLiteral("clean"));
    check(clean && clean->kind == Kind::Worktree && clean->status == Status::Orphaned &&
              clean->ahead == 0 && clean->dirtyFiles == 0 && clean->bytes >= 100000,
          "a clean merged worktree nothing uses is orphaned and sized with its node_modules");
    const Item *dirty = worktree(QStringLiteral("dirty"));
    check(dirty && dirty->status == Status::Attached && dirty->dirtyFiles == 1 &&
              dirty->reasonText().contains(QStringLiteral("1 uncommitted file")),
          "uncommitted changes attach a worktree");
    const Item *ahead = worktree(QStringLiteral("ahead"));
    check(ahead && ahead->status == Status::Attached && ahead->ahead == 1 &&
              ahead->reasonText().contains(QStringLiteral("1 commit not in main")),
          "unmerged commits attach a worktree");
    const Item *agent = worktree(QStringLiteral("agent"));
    check(agent && agent->status == Status::Attached &&
              agent->reasonText().contains(QStringLiteral("agent #512")),
          "a finished agent session's branch attaches its worktree");
    const Item *live = worktree(QStringLiteral("live"));
    check(live && live->status == Status::InUse && !live->deletable(),
          "a running agent's worktree is in use");
    const Item *task = worktree(QStringLiteral("task"));
    check(task && task->status == Status::Attached &&
              task->reasonText().contains(QStringLiteral("Ship it")),
          "an organization task work item attaches its worktree");
    const Item *busy = worktree(QStringLiteral("busy"));
    check(busy && busy->status == Status::InUse && busy->reasonText().contains(QStringLiteral("sleep")),
          "a process working inside a worktree marks it in use");
    const Item *leftover = worktree(QStringLiteral("leftover"));
    check(leftover && leftover->kind == Kind::Files && leftover->status == Status::Orphaned &&
              hasMember(leftover, fixture.worktrees + QStringLiteral("/leftover")),
          "a leftover folder without .git is plain files");

    QList<Item> chosen;
    QHash<QString, Choice> choices;
    for (const Item *item : {clean, dirty, ahead, live}) {
        if (item)
            chosen << *item;
    }
    if (ahead)
        choices.insert(ahead->id, {true, false, false});
    Plan plan = buildPlan(chosen, choices);
    check(plan.worktrees.size() == 2 && plan.skipped.size() == 2,
          "the plan removes the orphan and the confirmed attached worktree only");
    if (dirty)
        choices.insert(dirty->id, {true, false, false});
    plan = buildPlan(chosen, choices);
    bool dirtyKept = false;
    for (const Skip &skip : plan.skipped)
        dirtyKept = dirtyKept || (skip.item.title == QStringLiteral("dirty") &&
                                  skip.why.contains(QStringLiteral("uncommitted")));
    check(dirtyKept, "a dirty worktree needs Discard ticked even when confirmed");
    if (dirty)
        choices.insert(dirty->id, {true, true, false});
    plan = buildPlan(chosen, choices);
    check(plan.worktrees.size() == 3, "ticking Discard lets the dirty worktree into the plan");

    Recheck recheck;
    Process busyProcess;
    busyProcess.pid = 1;
    busyProcess.cwd = fixture.worktrees + QStringLiteral("/clean/node_modules");
    recheck.processes.processes << busyProcess;
    check(clean && !worktreeRefusal(*clean, recheck).isEmpty(),
          "a worktree that became busy after the scan is refused at delete time");
    check(clean && worktreeRefusal(*clean, Recheck()).isEmpty(),
          "an idle worktree passes the delete-time check");
}

void testBuildsAndCaches()
{
    Context context = baseContext();
    QDir().mkpath(context.worktreesDir + QStringLiteral("/5-feature"));
    touch(context.buildsDir + QStringLiteral("/agent-5/build.ninja"));
    touch(context.buildsDir + QStringLiteral("/agent-9/build.ninja"));
    touch(context.buildsDir + QStringLiteral("/agent-9-tmp/cc.o"));
    touch(context.buildsDir + QStringLiteral("/agent-9.build0.log"));
    touch(context.sandboxDir + QStringLiteral("/forkmesh-run-1-100/work/a"));
    touch(context.sandboxDir + QStringLiteral("/forkmesh-run-2-200/work/a"));
    touch(context.dataDir + QStringLiteral("/actions/cache/repo/x"));
    touch(context.home + QStringLiteral("/.cache/ms-playwright/chromium-1000/chrome"));
    touch(context.home + QStringLiteral("/.cache/ms-playwright/chromium-1100/chrome"));
    touch(context.home + QStringLiteral("/.cache/fm-rewrite/state.json"));
    touch(context.home + QStringLiteral("/.cache/forkmesh-release-host/build.log"));
    touch(context.home + QStringLiteral("/.ccache/a/b"));
    touch(context.home + QStringLiteral("/.cache/ccache/a/b"));
    touch(context.tmpRoots.first() + QStringLiteral("/fm123-build/x.o"));
    touch(context.tmpRoots.first() + QStringLiteral("/unrelated-file"));
    touch(context.home + QStringLiteral("/.qttest/share/ForkMeshTests/WindowResize-qt_temp-1/x"));
    age(context.home, 4);
    age(context.tmpRoots.first(), 4);
    context.ccacheDir = context.home + QStringLiteral("/.ccache");
    context.claimedSandboxes = {QStringLiteral("forkmesh-run-1-100")};
    const ScanResult result = scan(context);

    const Item *attached = find(result.items, Category::BuildTemp, QStringLiteral("Build · agent-5"));
    check(attached && attached->status == Status::Attached &&
              attached->reasonText().contains(QStringLiteral("5-feature")),
          "an agent build dir whose worktree still exists is attached");
    const Item *orphan = find(result.items, Category::BuildTemp, QStringLiteral("Build · agent-9"));
    check(orphan && orphan->status == Status::Orphaned && orphan->members.size() == 3,
          "an agent build dir, its tmp dir and its logs are one orphaned item");
    const Item *claimed = find(result.items, Category::BuildTemp,
                               QStringLiteral("Actions sandbox · forkmesh-run-1-100"));
    const Item *finished = find(result.items, Category::BuildTemp,
                                QStringLiteral("Actions sandbox · forkmesh-run-2-200"));
    check(claimed && claimed->status == Status::InUse && finished &&
              finished->status == Status::Orphaned && finished->kind == Kind::ActionSandbox,
          "only finished Actions sandboxes can go");
    const Item *actionsCache = find(result.items, Category::BuildTemp, QStringLiteral("Actions build cache"));
    check(actionsCache && actionsCache->kind == Kind::Report && !actionsCache->deletable(),
          "the Actions build cache is report only");
    const Item *newest = find(result.items, Category::BuildTemp, QStringLiteral("Playwright chromium-1100"));
    const Item *older = find(result.items, Category::BuildTemp, QStringLiteral("Playwright chromium-1000"));
    check(newest && newest->status == Status::Attached && older && older->status == Status::Orphaned,
          "only older Playwright browser builds are orphaned");
    const Item *operatorCache = find(result.items, Category::OtherCaches, QStringLiteral("fm-rewrite"));
    check(operatorCache && operatorCache->confirmEach && operatorCache->needsConfirm(),
          "operator caches always need an explicit confirm");
    const Item *release = find(result.items, Category::OtherCaches, QStringLiteral("forkmesh-release-host"));
    check(release && release->status == Status::Protected, "release build folders are protected");
    const QString active = QStringLiteral("ccache · ") + QDir::cleanPath(context.home + QStringLiteral("/.ccache"));
    const QString idle = QStringLiteral("ccache · ") + QDir::cleanPath(context.home + QStringLiteral("/.cache/ccache"));
    const Item *activeCache = find(result.items, Category::BuildTemp, active);
    const Item *idleCache = find(result.items, Category::BuildTemp, idle);
    check(activeCache && activeCache->kind == Kind::Report && !activeCache->deletable(),
          "the active ccache is report only");
    check(idleCache && idleCache->deletable() && idleCache->needsConfirm(),
          "an idle ccache folder can go after a confirm");
    const Item *temp = find(result.items, Category::BuildTemp, QStringLiteral("Agent temp files"));
    check(temp && hasMember(temp, context.tmpRoots.first() + QStringLiteral("/fm123-build")) &&
              !hasMember(temp, context.tmpRoots.first() + QStringLiteral("/unrelated-file")),
          "only agent temp files are listed from the temp folder");
    const Item *qttest = find(result.items, Category::BuildTemp, QStringLiteral("Qt test temp folders"));
    check(qttest && qttest->members.size() == 1, "Qt test temp folders are listed");
}

void testDeletion()
{
    const QString parent = gRoot + QStringLiteral("/del/project");
    const QString finished = parent + QLatin1Char('/') + kOld + QStringLiteral(".jsonl");
    const QString finishedDir = parent + QLatin1Char('/') + kOld;
    const QString live = parent + QLatin1Char('/') + kLive + QStringLiteral(".jsonl");
    const QString fresh = parent + QStringLiteral("/fresh.txt");
    const QString outsideTarget = gRoot + QStringLiteral("/del/keep");
    const QString link = parent + QStringLiteral("/link");
    touch(finished);
    touch(finishedDir + QStringLiteral("/sub/x"));
    touch(live);
    touch(fresh);
    touch(outsideTarget + QStringLiteral("/precious.txt"));
    QFile::link(outsideTarget, link);
    age(parent, 30);
    touch(fresh);
    touch(gRoot + QStringLiteral("/del/sibling.txt"));

    Item item;
    item.id = QStringLiteral("x");
    item.category = Category::Claude;
    item.kind = Kind::Files;
    item.title = QStringLiteral("Transcripts");
    item.path = parent;
    item.parent = parent;
    item.status = Status::Orphaned;
    for (const QString &path : {finished, finishedDir, live, fresh, link,
                                gRoot + QStringLiteral("/del/sibling.txt")})
        item.members.append({path, 10});
    item.bytes = 60;

    Recheck recheck;
    recheck.live = {kLive};
    recheck.nowMs = QDateTime::currentMSecsSinceEpoch();
    const Plan plan = buildPlan({item}, {});
    check(plan.files.size() == 1 && plan.bytes == 60, "an orphaned files item needs no confirm");
    const QList<Outcome> outcomes = deleteFiles(plan.files, recheck);
    const auto outcomeFor = [&](const QString &path) -> const Outcome * {
        for (const Outcome &outcome : outcomes) {
            if (outcome.path == QDir::cleanPath(path))
                return &outcome;
        }
        return nullptr;
    };
    check(!QFileInfo::exists(finished) && !QFileInfo::exists(finishedDir),
          "finished transcripts and their folders are removed");
    check(QFileInfo::exists(live) && outcomeFor(live) && !outcomeFor(live)->removed,
          "a transcript whose session went live after the scan is kept");
    check(QFileInfo::exists(fresh) && outcomeFor(fresh) &&
              outcomeFor(fresh)->error.contains(QStringLiteral("changed in the last")),
          "a file that changed after the scan is kept");
    check(!QFileInfo(link).isSymLink() &&
              QFileInfo::exists(outsideTarget + QStringLiteral("/precious.txt")),
          "a link is removed without touching what it points at");
    check(QFileInfo::exists(gRoot + QStringLiteral("/del/sibling.txt")) &&
              outcomeFor(gRoot + QStringLiteral("/del/sibling.txt"))->error.contains(QStringLiteral("not inside")),
          "a member outside the item's folder is refused");
    check(freedBytes(outcomes) == 30, "freed bytes count only what was removed");

    const QString log = gRoot + QStringLiteral("/logs/cleanup.log");
    check(appendLog(log, outcomes, recheck.nowMs), "the cleanup log is written");
    QFile file(log);
    file.open(QIODevice::ReadOnly);
    const QString text = QString::fromUtf8(file.readAll());
    check(text.contains(QStringLiteral("\tremoved\t10\t") + QDir::cleanPath(finished)) &&
              text.contains(QStringLiteral("\tkept\t")),
          "every deletion and refusal is logged with its path and size");

    Item sandbox;
    sandbox.id = QStringLiteral("s");
    sandbox.kind = Kind::ActionSandbox;
    sandbox.path = gRoot + QStringLiteral("/sandbox/forkmesh-run-2-200");
    sandbox.parent = gRoot + QStringLiteral("/sandbox");
    sandbox.status = Status::Orphaned;
    sandbox.members.append({sandbox.path, 5});
    sandbox.bytes = 5;
    QString seenBase;
    QString seenPath;
    const QList<Outcome> sandboxOutcomes = deleteSandboxes(
        {{sandbox, Choice()}}, Recheck(), [&](const QString &base, const QString &path) {
            seenBase = base;
            seenPath = path;
            return QString();
        });
    check(sandboxOutcomes.size() == 1 && sandboxOutcomes.first().removed &&
              seenBase == QDir::cleanPath(sandbox.parent) && seenPath == QDir::cleanPath(sandbox.path),
          "Actions sandboxes go through the desktop's own guarded removal");

    Item protectedItem = item;
    protectedItem.status = Status::Protected;
    Item inUse = item;
    inUse.status = Status::InUse;
    Item attached = item;
    attached.status = Status::Attached;
    const Plan refused = buildPlan({protectedItem, inUse, attached}, {});
    check(refused.count() == 0 && refused.skipped.size() == 3,
          "protected, in-use and unconfirmed attached items never reach the plan");
    check(describe(buildPlan({item}, {})).contains(QStringLiteral("reclaimed")),
          "the plan summary says how much space comes back");
}

bool sandboxEnvironment(const QString &root)
{
    const QByteArray home = (root + QStringLiteral("/home")).toUtf8();
    qputenv("HOME", home);
    qputenv("XDG_DATA_HOME", home + "/.local/share");
    qputenv("XDG_CONFIG_HOME", home + "/.config");
    qputenv("XDG_CACHE_HOME", home + "/.cache");
    for (const char *name : {"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_PROJECT",
                             "FORKMESH_PROJECT_DIR", "FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR",
                             "CCACHE_DIR"})
        qunsetenv(name);
#if defined(Q_OS_UNIX)
    if (const passwd *entry = ::getpwuid(::getuid())) {
        const QString realHome = QDir::cleanPath(QString::fromLocal8Bit(entry->pw_dir));
        if (pathWithin(QDir::cleanPath(root), realHome))
            return false;
    }
#endif
    return QDir::homePath() == root + QStringLiteral("/home");
}

} // namespace

int main(int argc, char **argv)
{
    const QString base = QFileInfo(QStringLiteral("/tmp")).isDir() ? QStringLiteral("/tmp")
                                                                    : QDir::tempPath();
    QTemporaryDir root(base + QStringLiteral("/forkmesh-cleanup-test-XXXXXX"));
    if (!root.isValid()) {
        std::printf("FAIL: temporary root\n");
        return 1;
    }
    gRoot = QFileInfo(root.path()).canonicalFilePath();
    QDir().mkpath(gRoot + QStringLiteral("/home"));
    QDir().mkpath(gRoot + QStringLiteral("/tmp"));
    if (!sandboxEnvironment(gRoot)) {
        std::printf("FAIL: the test root %s is not a sandbox outside the real home\n", qPrintable(gRoot));
        return 1;
    }
    QCoreApplication app(argc, argv);
    testHelpers();
    testProcesses();
    testClaude();
    testCodex();
    testWorktrees();
    testBuildsAndCaches();
    testDeletion();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
