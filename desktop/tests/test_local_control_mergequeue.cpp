#include "LocalControlHosts.h"
#include "LocalControlMergeQueue.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::localcontrol::mergequeue;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static RepoState openRepo()
{
    RepoState repo;
    repo.owner = QStringLiteral("mona");
    repo.name = QStringLiteral("app");
    repo.enabled = true;
    repo.open = true;
    repo.writable = true;
    repo.queue.enqueue(4, 100);
    repo.queue.enqueue(9, 200);
    return repo;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    check(familyPrefix() == QStringLiteral("mergequeue."), "family prefix");
    check(actions().size() == 9 && isKnownAction(QStringLiteral("process")) &&
              !isKnownAction(QStringLiteral("merge")),
          "the phone may name only the queue's own operations");
    check(needsNumber(QStringLiteral("move")) && !needsNumber(QStringLiteral("clear")),
          "number-bound actions");

    check(validRequest(QStringLiteral("mona"), QStringLiteral("app"), QStringLiteral("enqueue"), 3, 0),
          "a plain enqueue is valid");
    check(!validRequest(QStringLiteral("../x"), QStringLiteral("app"), QStringLiteral("clear"), 0, 0),
          "path segments are refused");
    check(!validRequest(QStringLiteral("mona"), QStringLiteral("a..b"), QStringLiteral("clear"), 0, 0),
          "dot-dot inside a name is refused");
    check(!validRequest(QStringLiteral("mona"), QStringLiteral("app"), QStringLiteral("remove"), 0, 0),
          "remove needs a pull number");
    check(!validRequest(QStringLiteral("mona"), QStringLiteral("app"), QStringLiteral("move"), 4, 0),
          "a move needs a direction");
    check(!validRequest(QStringLiteral("mona"), QStringLiteral("app"), QStringLiteral("move"), 4, 500),
          "a move is bounded");

    check(confirmationTarget(QStringLiteral("Mona"), QStringLiteral("App"), QStringLiteral("move"), 4, -1) ==
              QStringLiteral("mona/app#4:-1"),
          "a token binds the repository, the pull and the move");
    check(confirmationTarget(QStringLiteral("mona"), QStringLiteral("app"), QStringLiteral("pause"), 7, 3) ==
              QStringLiteral("mona/app"),
          "repository-wide actions bind only the repository");
    check(actionSummary(QStringLiteral("mona"), QStringLiteral("app"), QStringLiteral("move"), 9, -2)
              .contains(QStringLiteral("up 2 places")),
          "move summary");
    check(actionSummary(QStringLiteral("mona"), QStringLiteral("app"), QStringLiteral("clear"), 0, 0)
              .contains(QStringLiteral("stay open")),
          "clear summary says nothing closes");

    forkmesh::localcontrol::hosts::ConfirmationStore store;
    const QString target = confirmationTarget(QStringLiteral("mona"), QStringLiteral("app"),
                                              QStringLiteral("remove"), 4, 0);
    const QString token = store.issue(QStringLiteral("dev"), QStringLiteral("remove"), target, 1000);
    const QString other = confirmationTarget(QStringLiteral("mona"), QStringLiteral("app"),
                                             QStringLiteral("remove"), 9, 0);
    check(store.consume(token, QStringLiteral("dev"), QStringLiteral("remove"), other, 2000) ==
              forkmesh::localcontrol::hosts::ConfirmationStore::Result::Mismatch,
          "a token for #4 cannot remove #9");
    check(store.consume(token, QStringLiteral("dev"), QStringLiteral("remove"), target, 2000) ==
              forkmesh::localcontrol::hosts::ConfirmationStore::Result::Missing,
          "a token is spent by any attempt");

    RepoState repo = openRepo();
    check(precheck(repo, QStringLiteral("enqueue"), 5, QStringLiteral("open")).isEmpty(), "open pull joins");
    check(precheck(repo, QStringLiteral("enqueue"), 5, QStringLiteral("merged")) ==
              QStringLiteral("not_open_pull"),
          "a merged pull cannot join");
    check(precheck(repo, QStringLiteral("enqueue"), 4, QString()) == QStringLiteral("already_queued"),
          "no duplicates");
    check(precheck(repo, QStringLiteral("remove"), 5, QString()) == QStringLiteral("not_queued"),
          "remove needs a queued pull");
    repo.queue.setState(4, MergeQueueState::Merging, QStringLiteral("Merging"), 300);
    check(precheck(repo, QStringLiteral("move"), 4, QString()) == QStringLiteral("busy"),
          "a merging entry does not move");
    repo.open = false;
    check(precheck(repo, QStringLiteral("process"), 0, QString()) == QStringLiteral("repo_not_open"),
          "only the open repository processes now");
    repo.paused = true;
    check(precheck(repo, QStringLiteral("process"), 0, QString()) == QStringLiteral("paused"),
          "a paused queue does not process");
    repo.enabled = false;
    check(precheck(repo, QStringLiteral("clear"), 0, QString()) == QStringLiteral("queue_off"),
          "an off queue refuses edits");
    check(precheck(repo, QStringLiteral("enable"), 0, QString()).isEmpty(), "but can be turned on");
    repo.writable = false;
    check(precheck(repo, QStringLiteral("enable"), 0, QString()) == QStringLiteral("read_only"),
          "a read-only mirror is never changed from the phone");

    MergeQueue queue = openRepo().queue;
    check(applyToQueue(queue, QStringLiteral("enqueue"), 12, 0, 500).isEmpty() && queue.size() == 3,
          "enqueue appends");
    check(applyToQueue(queue, QStringLiteral("move"), 12, -5, 0).isEmpty() && queue.numbers().first() == 12,
          "move clamps to the top");
    check(resultMessage(QStringLiteral("move"), 12, queue) == QStringLiteral("Pull request #12 is now at place 1."),
          "move message");
    check(applyToQueue(queue, QStringLiteral("remove"), 4, 0, 0).isEmpty() && !queue.contains(4), "remove");
    check(applyToQueue(queue, QStringLiteral("remove"), 4, 0, 0) == QStringLiteral("not_queued"), "remove twice");
    check(applyToQueue(queue, QStringLiteral("pause"), 0, 0, 0) == QStringLiteral("unsupported_action"),
          "flags are not queue edits");
    check(applyToQueue(queue, QStringLiteral("clear"), 0, 0, 0).isEmpty() && queue.isEmpty(), "clear");

    RepoState listed = openRepo();
    listed.queue.setState(9, MergeQueueState::Blocked, QString(300, QLatin1Char('x')), 1);
    QHash<int, QString> titles{{4, QString(500, QLatin1Char('t'))}};
    const QJsonObject row = repoRow(listed, titles);
    const QJsonArray entries = row.value(QStringLiteral("entries")).toArray();
    check(row.value(QStringLiteral("owner")).toString() == QStringLiteral("mona") &&
              row.value(QStringLiteral("open")).toBool() && entries.size() == 2,
          "row carries the queue");
    check(entries.at(0).toObject().value(QStringLiteral("title")).toString().size() == kMaxTitleChars,
          "titles are bounded");
    check(entries.at(1).toObject().value(QStringLiteral("detail")).toString().size() <= 200,
          "details are bounded");
    check(!row.contains(QStringLiteral("localPath")) && !row.contains(QStringLiteral("path")),
          "no local paths reach the phone");
    RepoState many = openRepo();
    for (int n = 100; n < 300; ++n)
        many.queue.enqueue(n, n);
    check(repoRow(many, {}).value(QStringLiteral("entries")).toArray().size() == kMaxEntries,
          "entries are bounded");
    RepoState off;
    off.owner = QStringLiteral("a");
    off.name = QStringLiteral("b");
    check(repoRow(off, {}).value(QStringLiteral("summary")).toString() == QStringLiteral("Off"),
          "off summary");

    if (failures == 0)
        std::cout << "local control mergequeue tests passed\n";
    return failures == 0 ? 0 : 1;
}
