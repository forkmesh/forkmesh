#include "DeployLogs.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh;
using namespace forkmesh::deploylogs;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void checkEqual(const QString &actual, const QString &expected,
                       const char *what)
{
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n", what,
                     qPrintable(expected), qPrintable(actual));
        ++failures;
    }
}

static void append(const QString &path, const QByteArray &data)
{
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Append), "append to a log");
    file.write(data);
}

static void testNamesAndCategories()
{
    checkEqual(fileKind(QStringLiteral("ops"),
                        QStringLiteral("20260926-213000-next_fleet-create-core.log")),
               QStringLiteral("next_fleet create-core"), "an ops log names tool and command");
    checkEqual(fileKind(QStringLiteral("ops"),
                        QStringLiteral("20260926-213000-next_dns.log")),
               QStringLiteral("next_dns"), "an ops log without a command");
    checkEqual(fileKind(QStringLiteral("autodeploy"),
                        QStringLiteral("20260926-212845-5c7485cf6b33.log")),
               QStringLiteral("autodeploy 5c7485cf6b33"), "an agent run");
    checkEqual(fileKind(QStringLiteral("autodeploy"),
                        QStringLiteral("20260926-212845-manual-5c7485cf6b33.log")),
               QStringLiteral("autodeploy manual-5c7485cf6b33"), "a manual run");
    checkEqual(withoutStamp(QStringLiteral("21:30:49 result: ok: deployed")),
               QStringLiteral("result: ok: deployed"), "the log's own stamp is dropped");
    checkEqual(lineCategory(QStringLiteral("result: ok: deployed")),
               QStringLiteral("DEPLOY"), "an ok line is a deploy line");
    checkEqual(lineCategory(QStringLiteral("result: FAILED: gate 2")),
               QStringLiteral("ERROR"), "a failed result is an error");
    checkEqual(lineCategory(QStringLiteral("FAILED app/tests/x.py::test_y - boom")),
               QStringLiteral("DEPLOY"),
               "a single failed test is left to the gate's verdict");
    checkEqual(lineCategory(QStringLiteral("next_deploy restart FAILED on n2")),
               QStringLiteral("ERROR"), "a failed step is an error");
    checkEqual(lineCategory(QStringLiteral("Traceback (most recent call last):")),
               QStringLiteral("ERROR"), "a traceback is an error");
    checkEqual(lineCategory(QStringLiteral("up to date"), 3), QStringLiteral("ERROR"),
               "an err-priority journal line is an error");
    const DeployLine line = fileLine(
        {QStringLiteral("ops"), QStringLiteral("next_deploy restart"),
         QStringLiteral("21:30:47 [execute] n1: restart")});
    checkEqual(line.text, QStringLiteral("Deploy next_deploy restart: [execute] n1: restart"),
               "a file line reads as Deploy <kind>: <line>");
}

static void testJournal()
{
    const QStringList fresh = journalArguments(QString());
    check(fresh.contains(QStringLiteral("--user")) &&
              fresh.contains(QStringLiteral("--lines=0")) &&
              fresh.contains(QStringLiteral("--unit=forkmesh-source-publish.service")) &&
              fresh.contains(QStringLiteral("--unit=forkmesh-mirror-fanout.service")) &&
              fresh.contains(QStringLiteral("--unit=forkmesh-node-deploy.service")),
          "a first follow starts at the end of the three user units");
    const QString cursor = QStringLiteral("s=abc;i=1;b=def;m=1;t=2;x=3");
    check(journalArguments(cursor).contains(QStringLiteral("--after-cursor=") + cursor) &&
              !journalArguments(cursor).contains(QStringLiteral("--lines=0")),
          "a saved cursor resumes the follow");
    const corelogs::JournalEntry publish = corelogs::parseJournalLine(
        R"({"MESSAGE":"up to date: 630a33cc4280","PRIORITY":"6","_SYSTEMD_UNIT":"user@1001.service","_SYSTEMD_USER_UNIT":"forkmesh-source-publish.service","__CURSOR":"s=a;i=2"})");
    checkEqual(journalRole(publish), QStringLiteral("publisher"), "the publisher unit");
    checkEqual(journalLine(publish).text,
               QStringLiteral("Deploy publisher: up to date: 630a33cc4280"),
               "a publisher line");
    const corelogs::JournalEntry agent = corelogs::parseJournalLine(
        R"({"MESSAGE":"21:30:49 result: FAILED: verify","PRIORITY":"6","_SYSTEMD_USER_UNIT":"forkmesh-node-deploy.service","__CURSOR":"s=a;i=3"})");
    checkEqual(journalLine(agent).text, QStringLiteral("Deploy agent: result: FAILED: verify"),
               "an agent line drops its stamp");
    checkEqual(journalLine(agent).category, QStringLiteral("ERROR"), "a failed agent run");
    const corelogs::JournalEntry other = corelogs::parseJournalLine(
        R"({"MESSAGE":"x","_SYSTEMD_USER_UNIT":"pipewire.service","__CURSOR":"s=a;i=4"})");
    check(journalLine(other).text.isEmpty(), "another user unit is not a deploy line");
}

static void testTailer()
{
    QTemporaryDir dir;
    check(dir.isValid(), "temp dir");
    const QString ops = dir.path() + QStringLiteral("/ops");
    const QString agent = dir.path() + QStringLiteral("/autodeploy");
    QDir().mkpath(agent);
    const QString old = agent + QStringLiteral("/20260926-210000-aaaaaaaaaaaa.log");
    append(old, "21:00:00 already there\n");
    FileTailer tailer({{QStringLiteral("autodeploy"), agent},
                       {QStringLiteral("ops"), ops}});
    check(tailer.poll().isEmpty(), "start-up tails from the end");
    append(old, "21:00:01 grown\n21:00:02 partial");
    QList<TailedLine> lines = tailer.poll();
    check(lines.size() == 1 && lines.value(0).raw == QStringLiteral("21:00:01 grown") &&
              lines.value(0).fileKind == QStringLiteral("autodeploy aaaaaaaaaaaa"),
          "growth is followed and a partial line waits");
    append(old, " line\n");
    lines = tailer.poll();
    check(lines.size() == 1 &&
              lines.value(0).raw == QStringLiteral("21:00:02 partial line"),
          "the partial line completes");
    QDir().mkpath(ops);
    append(ops + QStringLiteral("/20260926-213000-next_deploy-restart.log"),
           "21:30:00 [execute] n1\n\n21:30:01 n1 ready\n");
    lines = tailer.poll();
    check(lines.size() == 2 && lines.value(0).directoryKind == QStringLiteral("ops") &&
              lines.value(1).raw == QStringLiteral("21:30:01 n1 ready"),
          "a new file is read from its start, blank lines skipped");
    check(tailer.poll().isEmpty(), "nothing new, nothing read");
    QFile truncate(old);
    check(truncate.open(QIODevice::WriteOnly | QIODevice::Truncate), "truncate");
    truncate.write("21:40:00 again\n");
    truncate.close();
    lines = tailer.poll();
    check(lines.size() == 1 && lines.value(0).raw == QStringLiteral("21:40:00 again"),
          "a truncated file is read again from the start");
}

static void testDedupe()
{
    CrossSourceDedupe dedupe;
    const QString line = QStringLiteral("21:30:49 result: ok");
    check(dedupe.admit(QStringLiteral("journal"), line, 1000), "first copy shows");
    check(!dedupe.admit(QStringLiteral("file"), line, 3000),
          "the file's copy of a journal line is dropped");
    check(dedupe.admit(QStringLiteral("file"), line, 4000),
          "a second identical file line still shows");
    check(dedupe.admit(QStringLiteral("file"), QStringLiteral("21:30:50 x"), 5000),
          "a file-only line shows");
    check(dedupe.admit(QStringLiteral("journal"), QStringLiteral("21:30:51 y"), 6000) &&
              dedupe.admit(QStringLiteral("journal"), QStringLiteral("21:30:51 y"), 6000),
          "identical lines from one source both show");
    check(dedupe.admit(QStringLiteral("journal"), QStringLiteral("idle"), 7000) &&
              dedupe.admit(QStringLiteral("file"), QStringLiteral("idle"),
                           7000 + kDedupeWindowMs + 31000),
          "a copy outside the window shows");
}

int main()
{
    testNamesAndCategories();
    testJournal();
    testTailer();
    testDedupe();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("deploy log tests passed\n");
    return 0;
}
