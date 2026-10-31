#include "ClaudeDesktopSessions.h"
#include "ClaudeDesktopSidebar.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrlQuery>

#include <iostream>

using namespace forkmesh::claudedesktop;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void writeFile(const QString &path, const QByteArray &body)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(body);
}

static const QString kId = QStringLiteral("0c43e0b8-a67a-4420-ae94-544cdc20df37");
static const QString kDir = QStringLiteral("-home-f-projects-forkmesh-src--worktrees-2550-arrows");

static void testUrl()
{
    const QUrl url = resumeUrl(kId.toUpper());
    check(url.scheme() == QLatin1String("claude"), "scheme is claude");
    check(url.host() == QLatin1String("resume"), "host is resume");
    check(QUrlQuery(url).queryItemValue(QStringLiteral("session")) == kId,
          "session id is lower-cased in the link");
    check(resumeUrl(QStringLiteral("not-an-id")).isEmpty(), "invalid id gives no link");
    check(resumeUrl(QStringLiteral("x;rm -rf ~")).isEmpty(), "shell text gives no link");
}

static void testAlreadyHome()
{
    QTemporaryDir tmp;
    const QString home = tmp.filePath(QStringLiteral("claude"));
    writeFile(home + QStringLiteral("/projects/") + kDir + QLatin1Char('/') + kId +
                  QStringLiteral(".jsonl"),
              "{}\n");
    const Exposure e = exposeSession(kId, {}, home);
    check(e.ok && !e.linked, "a transcript already under ~/.claude needs no link");
}

static void testLinksAccountDir()
{
    QTemporaryDir tmp;
    const QString home = tmp.filePath(QStringLiteral("claude"));
    const QString account = tmp.filePath(QStringLiteral("accounts/5433f389-0ee0-4df0"));
    const QString source = account + QStringLiteral("/projects/") + kDir;
    writeFile(source + QLatin1Char('/') + kId + QStringLiteral(".jsonl"), "{}\n");
    const Exposure e = exposeSession(kId, {account}, home);
    check(e.ok && e.linked, "an account transcript is linked");
    const QFileInfo link(home + QStringLiteral("/projects/") + kDir);
    check(link.isSymLink(), "the project folder is a symlink");
    check(QFileInfo(link.symLinkTarget()).canonicalFilePath() ==
              QFileInfo(source).canonicalFilePath(),
          "the symlink points at the account folder");
    check(!QFileInfo(e.transcript).isSymLink(), "the transcript itself is a plain file");
    check(QFile::exists(e.transcript), "the transcript is reachable through the link");
    const Exposure again = exposeSession(kId, {account}, home);
    check(again.ok, "exposing twice is fine");
    check(QDir(home + QStringLiteral("/projects")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size() == 1,
          "exposing twice adds no second link");
}

static void testConflictUsesAlternateName()
{
    QTemporaryDir tmp;
    const QString home = tmp.filePath(QStringLiteral("claude"));
    writeFile(home + QStringLiteral("/projects/") + kDir +
                  QStringLiteral("/e56816cf-c9d1-423c-86dd-cde14433eb85.jsonl"),
              "{}\n");
    const QString account = tmp.filePath(QStringLiteral("accounts/5433f389-0ee0-4df0"));
    writeFile(account + QStringLiteral("/projects/") + kDir + QLatin1Char('/') + kId +
                  QStringLiteral(".jsonl"),
              "{}\n");
    const Exposure e = exposeSession(kId, {account}, home);
    check(e.ok && e.linked, "a name clash still links");
    check(QFileInfo(home + QStringLiteral("/projects/") + kDir +
                    QStringLiteral("--forkmesh-5433f389"))
              .isSymLink(),
          "a name clash links under the account-suffixed name");
    check(QFile::exists(home + QStringLiteral("/projects/") + kDir +
                        QStringLiteral("/e56816cf-c9d1-423c-86dd-cde14433eb85.jsonl")),
          "the existing folder is left alone");
}

static void testMissing()
{
    QTemporaryDir tmp;
    const Exposure e = exposeSession(kId, {tmp.filePath(QStringLiteral("a"))},
                                     tmp.filePath(QStringLiteral("claude")));
    check(!e.ok && !e.error.isEmpty(), "a missing transcript reports an error");
    check(!exposeSession(QStringLiteral("../../etc"), {}, tmp.path()).ok,
          "a path-like id is refused");
}

static void testFileSymlinkIgnored()
{
    QTemporaryDir tmp;
    const QString home = tmp.filePath(QStringLiteral("claude"));
    const QString real = tmp.filePath(QStringLiteral("real.jsonl"));
    writeFile(real, "{}\n");
    QDir().mkpath(home + QStringLiteral("/projects/") + kDir);
    QFile::link(real, home + QStringLiteral("/projects/") + kDir + QLatin1Char('/') + kId +
                          QStringLiteral(".jsonl"));
    check(findTranscript(home + QStringLiteral("/projects"), kId).isEmpty(),
          "a symlinked transcript file does not count, the Claude app refuses it");
}

static void testExposeAccount()
{
    QTemporaryDir tmp;
    const QString home = tmp.filePath(QStringLiteral("claude"));
    const QString account = tmp.filePath(QStringLiteral("accounts/6c28b14c"));
    writeFile(account + QStringLiteral("/projects/-a/1.jsonl"), "{}\n");
    writeFile(account + QStringLiteral("/projects/-b/2.jsonl"), "{}\n");
    check(exposeAccount(account, home) == 2, "every account folder is linked");
    check(exposeAccount(account, home) == 2, "linking again finds them already there");
    check(exposeAccount(home, home) == 0, "the default account is never linked into itself");
}

static void testLatestSessionId()
{
    QList<QJsonObject> events;
    events.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                              {QStringLiteral("session_id"), kId}});
    events.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                              {QStringLiteral("session_id"),
                               QStringLiteral("2ED52789-29FD-4E29-91C9-20FCC6ACF132")}});
    events.append(QJsonObject{{QStringLiteral("thread_id"), QStringLiteral("codex-thread")}});
    check(latestSessionId(events) == QLatin1String("2ed52789-29fd-4e29-91c9-20fcc6acf132"),
          "the newest Claude session id wins and Codex threads are skipped");
    check(latestSessionId({}).isEmpty(), "no events, no id");
}

static const QString kScope = QStringLiteral("acct/org");

static void writeRecord(const QString &root, const QString &id, const QString &cwd,
                        const QString &title, qint64 activity, bool archived = false)
{
    QJsonObject o;
    o.insert(QStringLiteral("sessionId"), QStringLiteral("local_") + id);
    o.insert(QStringLiteral("cliSessionId"), id);
    o.insert(QStringLiteral("cwd"), cwd);
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("createdAt"), double(activity - 10));
    o.insert(QStringLiteral("lastActivityAt"), double(activity));
    o.insert(QStringLiteral("isArchived"), archived);
    writeFile(root + QStringLiteral("/claude-code-sessions/") + kScope + QStringLiteral("/local_") +
                  id + QStringLiteral(".json"),
              QJsonDocument(o).toJson());
}

static QString uuid(int n)
{
    return QStringLiteral("00000000-0000-4000-8000-%1").arg(n, 12, 10, QLatin1Char('0'));
}

static void testDesktopIndex()
{
    QTemporaryDir dir;
    const QString desktop = dir.filePath(QStringLiteral("Claude"));
    const QString home = dir.filePath(QStringLiteral("home"));
    writeRecord(desktop, uuid(1), QStringLiteral("/r/src/.worktrees/15-qr"), QStringLiteral("QR codes"), 500);
    writeRecord(desktop, uuid(2), QStringLiteral("/r/src/.worktrees/2625-sweep"), QStringLiteral("Sweep"), 900);
    writeRecord(desktop, uuid(3), QStringLiteral("/r/src/.worktrees/2346-ssh"), QStringLiteral("SSH"), 100);
    writeRecord(desktop, uuid(4), QStringLiteral("/r/src"), QStringLiteral("Old"), 50, true);
    writeFile(desktop + QStringLiteral("/claude-code-sessions/") + kScope +
                  QStringLiteral("/waiting-input/local_") + uuid(2),
              "1");
    writeFile(home + QStringLiteral("/projects/-r-src--worktrees-15-qr/") + uuid(1) +
                  QStringLiteral(".jsonl"),
              "{}\n");
    const QByteArray config = QStringLiteral(R"({"preferences":{"epitaxyPrefs":{
        "dframe-code-sections":{"acct/org":{"sections":[
          {"id":"sessions","kind":"sessions","order":5,"shown":true,"groupBy":"project"},
          {"id":"pinned","kind":"pinned","order":0,"shown":true},
          {"id":"routines","kind":"routines","order":1,"shown":false},
          {"id":"cg-empty","kind":"manual","name":"Desktop app","order":2,"shown":true,"members":[]},
          {"id":"cg-infra","kind":"manual","name":"Infrastructure & fleet","order":3,"shown":true,"members":[]}]}},
        "dframe-group-scopes":{"acct/org":{"groups":[{"id":"cg-infra","name":"Infrastructure & fleet"}],
          "assignments":{"code:local_%1":"cg-infra"},"order":{"cg-infra":["code:local_%1"]}}},
        "starred-local-code-sessions":[]}}})")
                                .arg(uuid(3))
                                .toUtf8();
    writeFile(desktop + QStringLiteral("/claude_desktop_config.json"), config);

    const DesktopIndex index = readDesktopIndex(desktop, home);
    check(index.records.size() == 3, "archived record is left out");
    QHash<QString, DesktopRecord> byId;
    for (const DesktopRecord &record : index.records)
        byId.insert(record.sessionId, record);
    check(byId.value(uuid(1)).title == QLatin1String("QR codes"), "record title read");
    check(byId.value(uuid(1)).transcript.endsWith(uuid(1) + QStringLiteral(".jsonl")),
          "transcript found under the cwd's project folder");
    check(byId.value(uuid(2)).transcript.isEmpty(), "missing transcript stays empty");
    check(byId.value(uuid(2)).waiting, "waiting-input marker read");
    check(!byId.value(uuid(1)).waiting, "no marker means not waiting");
    check(index.sections.size() == 4, "routines and hidden sections dropped");
    check(index.sections.first().kind == QLatin1String("pinned"), "sections follow their order");
    check(index.sections.at(2).members == QStringList{QStringLiteral("local_") + uuid(3)},
          "manual group members read from the group scopes");

    QList<SidebarItem> items;
    for (const DesktopRecord &record : index.records)
        items.append({record.key, folderLabel(record.cwd), record.activityMs});
    items.append({QString(), QStringLiteral("30-stall"), 1000});
    items.append({QString(), QStringLiteral("15-qr"), 2000});
    const QList<SidebarGroup> groups = arrangeSidebar(index, items);
    QStringList labels;
    for (const SidebarGroup &group : groups)
        labels << group.label;
    check(labels == QStringList({QStringLiteral("Infrastructure & fleet"), QStringLiteral("15-qr"),
                                 QStringLiteral("30-stall"), QStringLiteral("2625-sweep")}),
          "manual group first, then folders in natural order, empty groups hidden");
    check(groups.first().key == QLatin1String("@claude:cg-infra"), "manual group key");
    check(groups.at(1).items.size() == 2 && items.at(groups.at(1).items.first()).at == 2000,
          "a folder lists its newest session first");

    DesktopIndex flat = index;
    for (DesktopSection &section : flat.sections)
        if (section.kind == QLatin1String("sessions"))
            section.groupBy = QStringLiteral("none");
    const QList<SidebarGroup> recents = arrangeSidebar(flat, items);
    check(recents.size() == 2 && recents.at(1).label == QLatin1String("Recents") &&
              recents.at(1).key == QLatin1String("@claude:recents"),
          "group by none lists the ungrouped sessions under Recents");
    QList<qint64> order;
    for (int item : recents.at(1).items)
        order << items.at(item).at;
    check(order == QList<qint64>({2000, 1000, 900, 500}),
          "Recents lists every ungrouped session newest first");
    flat.sections.clear();
    const QList<SidebarGroup> bare = arrangeSidebar(flat, items);
    check(bare.size() == 1 && bare.first().items.size() == items.size(),
          "without the Claude app's layout every session goes under Recents");
}

static void testNaturalOrder()
{
    check(naturalLess(QStringLiteral("30-a"), QStringLiteral("2625-b")), "30 before 2625");
    check(naturalLess(QStringLiteral("forkmesh"), QStringLiteral("Src")), "case-insensitive");
    check(!naturalLess(QStringLiteral("b"), QStringLiteral("a")), "b after a");
    check(folderLabel(QStringLiteral("/r/src/.worktrees/15-qr/")) == QLatin1String("15-qr"),
          "folder label is the last folder");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testUrl();
    testAlreadyHome();
    testLinksAccountDir();
    testConflictUsesAlternateName();
    testMissing();
    testFileSymlinkIgnored();
    testExposeAccount();
    testLatestSessionId();
    testDesktopIndex();
    testNaturalOrder();
    if (failures == 0)
        std::cout << "claude desktop sessions: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
