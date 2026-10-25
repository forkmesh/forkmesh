#include "ClaudeDesktopSessions.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    if (failures == 0)
        std::cout << "claude desktop sessions: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
