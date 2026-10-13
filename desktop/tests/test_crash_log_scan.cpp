#include "CrashHandler.h"
#include "CrashLogScan.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <iostream>

namespace crashlog = forkmesh::crashlog;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

// Exactly what CrashHandler::crashHandler() writes to the log file descriptor,
// in that order: banner, epoch, signal, build, siginfo, context, the compact
// one-line record, then the backtrace and the footer.
static QString crashBlock(qint64 epoch, const QString &signalName,
                          int frameCount)
{
    QStringList lines;
    lines << QString() << crashlog::kCrashBanner
          << QStringLiteral("when (epoch): %1").arg(epoch)
          << QStringLiteral("signal: %1").arg(signalName)
          << QStringLiteral("build: ForkMesh v9.9.9 (src /src/forkmesh)")
          << QStringLiteral("signal code: 1")
          << QStringLiteral("sender pid: 0")
          << QStringLiteral("sender uid: 0")
          << QStringLiteral("context:")
          << QStringLiteral("repo detail: forkmesh/forkmesh | diff stream")
          << QStringLiteral("ForkMesh signal: %1 at epoch %2; terminating; "
                            "full record logged above")
                 .arg(signalName)
                 .arg(epoch)
          << QString() << QStringLiteral("backtrace:");
    for (int frame = 0; frame < frameCount; ++frame) {
        lines << QStringLiteral("/usr/bin/forkmesh(+0x%1)[0x555500%2]")
                     .arg(frame, 4, 16, QLatin1Char('0'))
                     .arg(frame);
    }
    lines << QStringLiteral("==========================");
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// A stop request: its own heading, its own 36-'=' footer, no backtrace.
static QString shutdownBlock(qint64 epoch)
{
    return QStringLiteral(
               "\n===== ForkMesh shutdown signal =====\n"
               "when (epoch): %1\n"
               "signal: SIGTERM (terminated)\n"
               "build: ForkMesh v9.9.9 (src /src/forkmesh)\n"
               "signal code: 0\n"
               "sender pid: 1\n"
               "sender uid: 0\n"
               "ForkMesh signal: SIGTERM (terminated) at epoch %1; shutting "
               "down cleanly\n"
               "stop requested: closing the window and quitting normally so "
               "settings, chat history and the log are saved; sending it again "
               "exits immediately\n"
               "====================================\n")
        .arg(epoch);
}

static QString appLine(const QString &body)
{
    return QStringLiteral("2026-08-20 10:00:00  %1\n").arg(body);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const qint64 firstEpoch = 1755676800;
    const qint64 secondEpoch = firstEpoch + 90;

    {
        const QString log = appLine(QStringLiteral("system: Session started")) +
                            crashBlock(firstEpoch,
                                       QStringLiteral("SIGSEGV (invalid memory "
                                                      "access)"),
                                       /*frameCount=*/4) +
                            appLine(QStringLiteral("system: Session started"));
        const QList<crashlog::Record> records = crashlog::parseCrashRecords(log);
        check(records.size() == 1, "one crash block yields one record");
        if (records.size() == 1) {
            const crashlog::Record &record = records.first();
            check(record.whenEpochSeconds == firstEpoch,
                  "the crash epoch is read back");
            check(record.signalName ==
                      QStringLiteral("SIGSEGV (invalid memory access)"),
                  "the signal keeps its full description");
            check(record.build.startsWith(QStringLiteral("ForkMesh v9.9.9")),
                  "the crashing build is recorded");
            check(record.context ==
                      QStringLiteral("repo detail: forkmesh/forkmesh | diff "
                                     "stream"),
                  "the crash context survives, without the signal record line");
            check(record.frames.size() == 4,
                  "every backtrace frame in a short trace is kept");
            check(record.frames.first().contains(QStringLiteral("+0x0000")),
                  "frames are stored innermost first");
            check(crashlog::describe(record).contains(
                      QStringLiteral("SIGSEGV")) &&
                      crashlog::describe(record).contains(
                          QStringLiteral("diff stream")),
                  "the evidence block names the signal and the context");
        }
    }

    {
        // A node whose stderr is redirected into the same file stores every
        // block twice. That is one crash, not two runs of the repair agent.
        const QString twice =
            crashBlock(firstEpoch, QStringLiteral("SIGABRT (abort)"), 3) +
            crashBlock(firstEpoch, QStringLiteral("SIGABRT (abort)"), 3);
        check(crashlog::parseCrashRecords(twice).size() == 1,
              "a doubled block is one crash");
    }

    {
        const QString log =
            shutdownBlock(firstEpoch) + appLine(QStringLiteral("bye"));
        check(crashlog::parseCrashRecords(log).isEmpty(),
              "a stop request is never a crash");
    }

    {
        const QString log =
            crashBlock(firstEpoch, QStringLiteral("SIGSEGV (invalid memory "
                                                  "access)"),
                       2) +
            shutdownBlock(firstEpoch + 30) +
            crashBlock(secondEpoch, QStringLiteral("SIGBUS (bus error)"), 2);
        const QList<crashlog::Record> records = crashlog::parseCrashRecords(log);
        check(records.size() == 2, "two crashes around a clean stop");
        if (records.size() == 2) {
            check(records.first().whenEpochSeconds == firstEpoch &&
                      records.last().whenEpochSeconds == secondEpoch,
                  "records come back oldest first");
        }
    }

    {
        // Killed mid-write: no footer, no backtrace, the next launch's crash
        // banner is the only terminator.
        QString truncated =
            crashBlock(firstEpoch, QStringLiteral("SIGILL (illegal "
                                                  "instruction)"),
                       2);
        truncated = truncated.left(
            truncated.indexOf(QStringLiteral("backtrace:")));
        const QList<crashlog::Record> records = crashlog::parseCrashRecords(
            truncated +
            crashBlock(secondEpoch, QStringLiteral("SIGSEGV (invalid memory "
                                                   "access)"),
                       2));
        check(records.size() == 2,
              "a truncated block does not swallow the crash after it");
        if (records.size() == 2)
            check(records.first().frames.isEmpty(),
                  "the truncated block simply has no frames");
    }

    {
        // netlog::tailLines() drops empty lines, so the in-memory app log the
        // scan actually reads has no blank separators at all.
        const QStringList packed =
            crashBlock(firstEpoch,
                       QStringLiteral("SIGSEGV (invalid memory access)"), 20)
                .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QList<crashlog::Record> records =
            crashlog::parseCrashRecords(packed);
        check(records.size() == 1, "the blank-free stored form still parses");
        if (records.size() == 1) {
            check(records.first().frames.size() == crashlog::kMaxFrames,
                  "a long backtrace is capped");
            check(records.first().context ==
                      QStringLiteral("repo detail: forkmesh/forkmesh | diff "
                                     "stream"),
                  "context ends at the signal record without a blank line");
        }
    }

    {
        QString noEpoch =
            crashBlock(firstEpoch, QStringLiteral("SIGSEGV (invalid memory "
                                                  "access)"),
                       2);
        noEpoch.replace(QStringLiteral("when (epoch): %1").arg(firstEpoch),
                        QStringLiteral("when (epoch): 0"));
        check(crashlog::parseCrashRecords(noEpoch).isEmpty(),
              "a block with no usable timestamp is dropped");
    }

    {
        // The handler writes through the descriptor it opened at startup, so a
        // log that rotated mid-session leaves the block at the end of a
        // segment while the live buffer has moved on past it.
        QTemporaryDir archive;
        check(archive.isValid(), "the archive fixture has a directory");
        QStringList segments;
        for (int index = 0; index < 4; ++index) {
            const QString path =
                QDir(archive.path())
                    .filePath(QStringLiteral("network_log-%1.txt").arg(index));
            QFile file(path);
            check(file.open(QIODevice::WriteOnly), "the segment fixture opens");
            QString body = appLine(QStringLiteral("system: rotated segment"));
            if (index == 2) {
                body += crashBlock(secondEpoch,
                                   QStringLiteral("SIGBUS (bus error)"), 3);
            }
            if (index == 0) {
                // Older than the segments the scan is willing to open.
                body += crashBlock(firstEpoch - 5000,
                                   QStringLiteral("SIGILL (illegal instruction)"),
                                   2);
            }
            file.write(body.toUtf8());
            file.close();
            segments << path;
        }
        const QStringList liveTail =
            crashBlock(firstEpoch,
                       QStringLiteral("SIGSEGV (invalid memory access)"), 2)
                .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        const QList<crashlog::Record> records =
            crashlog::scanRecentCrashes(liveTail, segments);
        check(records.size() == 2,
              "a crash left in a rotated segment is found beside the live one");
        if (records.size() == 2) {
            check(records.first().whenEpochSeconds == firstEpoch &&
                      records.last().whenEpochSeconds == secondEpoch,
                  "records from both sources come back oldest first");
        }
        // Same crash in the buffer and in a segment is still one crash.
        QStringList overlapping = liveTail;
        overlapping += crashBlock(secondEpoch,
                                  QStringLiteral("SIGBUS (bus error)"), 3)
                           .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        check(crashlog::scanRecentCrashes(overlapping, segments).size() == 2,
              "a crash seen in both the buffer and a segment is not doubled");
    }

    {
        const QString status = QStringLiteral(
            "2026-09-24 17:38:14  Status: Disconnected - reconnecting...  "
            "[desktop/src/MainWindowSettings.cpp:5656]");
        QString split = crashBlock(firstEpoch,
                                   QStringLiteral("SIGSEGV (invalid memory "
                                                  "access)"),
                                   3);
        split.replace(QStringLiteral("signal: SIGSEGV"),
                      QStringLiteral("signal: ") + status +
                          QStringLiteral("\nSIGSEGV"));
        QString tail = crashBlock(secondEpoch,
                                  QStringLiteral("SIGBUS (bus error)"), 3);
        tail.replace(QStringLiteral("signal: SIGBUS (bus error)"),
                     QStringLiteral("signal: SIGBUS (bus error)") + status +
                         QStringLiteral("\n"));
        tail.replace(QStringLiteral("backtrace:\n"),
                     QStringLiteral("backtrace:\n") + status +
                         QStringLiteral("\n"));
        const QList<crashlog::Record> records =
            crashlog::parseCrashRecords(split + tail);
        check(records.size() == 2,
              "blocks with another writer's lines inside still parse");
        if (records.size() == 2) {
            check(records.first().signalName ==
                      QStringLiteral("SIGSEGV (invalid memory access)"),
                  "a log line written between 'signal: ' and the name is not "
                  "the signal");
            check(records.last().signalName ==
                      QStringLiteral("SIGBUS (bus error)"),
                  "a log line appended to the signal name is cut off");
            check(records.last().frames.size() == 3 &&
                      !records.last().frames.join(QLatin1Char('\n'))
                           .contains(QStringLiteral("Status:")),
                  "a log line inside the backtrace is not a frame");
            check(crashlog::summarize(records.first())
                      .startsWith(QStringLiteral("SIGSEGV")),
                  "the summary header starts with the signal");
        }
    }

    {
        const QString context = QStringLiteral(
            "action run id: 341\nworkflow: Deploy\nrecent process output "
            "tail:\n");
        QString output;
        for (int line = 0; line < 400; ++line)
            output += QStringLiteral("[ %1%] Building CXX object "
                                     "CMakeFiles/forkmesh-window-tests.dir/"
                                     "src/File%2.cpp.o\n")
                          .arg(line % 100)
                          .arg(line);
        const QString fitted = forkmesh::fitCrashContext(context, output);
        check(fitted.toUtf8().size() <= forkmesh::kCrashContextCapacityBytes,
              "the crash context fits the handler's buffer");
        check(fitted.startsWith(context),
              "the action header survives a long process output tail");
        check(fitted.mid(context.size())
                  .startsWith(QStringLiteral("[ ")),
              "the output tail starts on a whole line");
        check(fitted.endsWith(QStringLiteral("File399.cpp.o\n")),
              "the newest output is kept");
    }

    {
        auto described = [](const QString &version, const QString &frame) {
            crashlog::Record record;
            record.whenEpochSeconds = 1790274473;
            record.signalName = QStringLiteral("SIGSEGV (invalid memory access)");
            record.build = QStringLiteral("ForkMesh v%1 (src /src/desktop)")
                               .arg(version);
            record.frames = {
                QStringLiteral("/src/desktop/build/forkmesh(+0x1cb5af3) "
                               "[0x55df4573aaf3]"),
                QStringLiteral("/lib/x86_64-linux-gnu/libc.so.6(+0x3fd70) "
                               "[0x7fb0f304bd70]"),
                QStringLiteral("/src/desktop/build/forkmesh(%1+0x83f) "
                               "[0x55df4591775f]")
                    .arg(frame)};
            return crashlog::describe(record);
        };
        const QString serverNode =
            QStringLiteral("_ZN10ServerNode17onSocketReadyReadEv");
        const QString pullStore =
            QStringLiteral("_ZNK9PullStore8readPullEiR11PullRequest");
        const crashlog::Record parsed =
            crashlog::parseDescription(described(QStringLiteral("1.2.4"),
                                                 serverNode));
        check(parsed.whenEpochSeconds == 1790274473 &&
                  crashlog::buildVersion(parsed.build) ==
                      QStringLiteral("1.2.4") &&
                  parsed.frames.size() == 3,
              "a stored crash description reads back");
        check(crashlog::signature(parsed) ==
                  QStringLiteral("SIGSEGV|") + serverNode,
              "the signature is the signal and the innermost app frame");

        const QStringList queued = {
            described(QStringLiteral("1.2.4"), serverNode),
            described(QStringLiteral("1.2.4"), pullStore),
            described(QStringLiteral("1.2.5"), pullStore),
            described(QStringLiteral("1.2.5"), serverNode),
        };
        check(crashlog::supersededByNewerBuild(queued, QStringLiteral("1.2.5"))
                  .isEmpty(),
              "a crash that recurred in the running version stays queued with "
              "its older records");
        const QList<int> superseded = crashlog::supersededByNewerBuild(
            queued.mid(0, 3), QStringLiteral("1.2.5"));
        check(superseded == QList<int>{0},
              "an older version's crash that has not recurred is set aside");
        check(crashlog::supersededByNewerBuild(queued, QString()).isEmpty(),
              "without a running version nothing is set aside");
    }

    if (failures > 0) {
        std::cerr << failures << " crash log scan test(s) failed\n";
        return 1;
    }
    std::cout << "crash log scan tests passed\n";
    return 0;
}
