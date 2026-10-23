// A CLI transport has to end, and has to say when a turn did not get through.
// Both were assumed rather than reported: a `claude` process that never started
// emitted no finished(), so the session that owned it stayed Running behind
// nothing, and a turn written into a closed pipe was reported to the caller as
// sent. Between them, pressing "add" on such a session queued a prompt nobody
// would ever read and restarted into the same silence (adhoc #1618).
#include "../src/ClaudeStreamSession.h"
#include "../src/ClaudeControlProtocol.h"
#include "../src/AgentGuardrails.h"
#include "../src/AgentGuardrailLedger.h"
#include "../src/AgentJail.h"
#include "../src/AgentStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>

namespace {

int failures = 0;

QStringList *capturedMessages = nullptr;

void captureMessages(QtMsgType, const QMessageLogContext &,
                     const QString &message)
{
    if (capturedMessages)
        capturedMessages->append(message);
}

void check(bool condition, const char *what)
{
    if (condition)
        qInfo("PASS: %s", what);
    else {
        qCritical("FAIL: %s", what);
        ++failures;
    }
}

template <typename Predicate> bool pump(Predicate predicate, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

// A working directory that is not there is the everyday version of this: agent
// worktrees live under /tmp and get swept, and the session that used one is
// picked back up days later. QProcess refuses to start at all, which raises
// errorOccurred and never finished — so this is the only ending the owner of the
// session will ever be told about.
void runLaunchFailureTest()
{
    ClaudeStreamSession session;
    int exits = 0;
    int lastCode = 0;
    QString stderrSeen;
    QObject::connect(&session, &ClaudeStreamSession::finished,
                     [&exits, &lastCode](int code) {
                         ++exits;
                         lastCode = code;
                     });
    QObject::connect(&session, &ClaudeStreamSession::stderrText,
                     [&stderrSeen](const QString &text) { stderrSeen += text; });

    const QString missing =
        QDir::tempPath() + QStringLiteral("/forkmesh-no-such-worktree-1618");
    QDir().rmdir(missing); // make sure of it
    session.start(missing, {}, QStringLiteral("Continue where you left off."));

    check(pump([&exits] { return exits > 0; }),
          "a launch that cannot start reports an exit instead of hanging");
    check(lastCode != 0, "the reported exit code marks it as a failure");
    check(stderrSeen.contains(QStringLiteral("could not be started")),
          "the failure names itself in the session's error stream");
    check(!session.running() && !session.acceptsInput(),
          "a session that never started is neither running nor steerable");
    check(!session.sendUserText(QStringLiteral("try again")),
          "a follow-up sent to a dead transport is refused, not swallowed");

    // Give any late signal a chance to double-report the same ending: the owner
    // treats each exit as "the CLI died mid-turn" and would spend a relaunch
    // attempt on a second one.
    pump([] { return false; }, 150);
    check(exits == 1, "the ending is reported exactly once");
}

// MainWindow destruction runs after QApplication::exec() returns.  A transport
// process cannot be detached and handed to deleteLater() at that point: there
// is no next event-loop turn to perform the deletion, and Qt is already
// destroying the rest of the QObject tree.
void runFinalTeardownProcessOwnershipTest()
{
    auto *session = new ClaudeStreamSession;
    int exits = 0;
    QObject::connect(session, &ClaudeStreamSession::finished,
                     [&exits](int) { ++exits; });

    const QString missing =
        QDir::tempPath() + QStringLiteral("/forkmesh-no-such-worktree-teardown");
    QDir().rmdir(missing);
    session->start(missing, {}, QStringLiteral("Continue."));
    check(pump([&exits] { return exits > 0; }),
          "teardown fixture reaches a stopped child process");

    QPointer<QProcess> process = session->findChild<QProcess *>();
    check(!process.isNull(), "the stopped process remains owned by its session");
    delete session;
    check(process.isNull(),
          "final transport teardown destroys its process synchronously");
}

// An idle transport that was never started is the same answer from the other
// direction — nothing to steer, and the caller must be told so it can start one.
void runUnstartedSessionTest()
{
    ClaudeStreamSession session;
    check(!session.running() && !session.acceptsInput(),
          "an unstarted session reports no live transport");
    check(!session.sendUserText(QStringLiteral("hello")),
          "an unstarted session refuses a turn");
    check(!session.sendToolResult(QStringLiteral("stale-question"),
                                  QStringLiteral("Allow once")),
          "an unstarted session refuses a question answer");
    check(session.sendUserText(QStringLiteral("   ")),
          "an empty turn is nothing to deliver, so it is not a delivery failure");
}

// Claude occasionally emits a single stream-json line containing a multi-MiB
// tool result.  Parsing must leave the event loop immediately, raw debug output
// must not duplicate the payload, and process exit must remain behind both the
// parser and the ordered transcript writer.
void runLargeOrderedStreamTest()
{
    QTemporaryDir tmp;
    check(tmp.isValid(), "large-stream transcript temp dir is valid");
    AgentStore store(tmp.path());
    AgentSession saved;
    saved.owner = QStringLiteral("octo");
    saved.name = QStringLiteral("large-stream");
    saved = store.createSession(saved);

    ClaudeStreamSession stream;
    QStringList deliveryOrder;
    QList<QJsonObject> events;
    QStringList rawLines;
    bool persistedBeforeFinish = false;
    QObject::connect(&stream, &ClaudeStreamSession::rawLine,
                     [&](const QString &line) {
                         rawLines.append(line);
                         deliveryOrder.append(QStringLiteral("raw"));
                     });
    QObject::connect(&stream, &ClaudeStreamSession::event,
                     [&](const QJsonObject &event) {
                         events.append(event);
                         deliveryOrder.append(
                             QStringLiteral("event-%1").arg(
                                 event.value(QStringLiteral("seq")).toInt()));
                         store.appendEvent(saved, event);
                     });
    stream.setFinishBarrier([&store](std::function<void()> completion) {
        store.whenTranscriptWritesDrained(QCoreApplication::instance(),
                                          std::move(completion));
    });
    QObject::connect(&stream, &ClaudeStreamSession::finished,
                     [&](int) {
                         deliveryOrder.append(QStringLiteral("finished"));
                         const QString path =
                             tmp.path() + QStringLiteral("/sessions/") +
                             saved.repoKey() + QLatin1Char('/') +
                             QString::number(saved.id) +
                             QStringLiteral("/events.jsonl");
                         QFile file(path);
                         persistedBeforeFinish = file.open(QIODevice::ReadOnly) &&
                             file.readAll().count('\n') == 2;
                     });

    const QString giant(600'000, QLatin1Char('x'));
    const QJsonObject first{{QStringLiteral("type"), QStringLiteral("system")},
                            {QStringLiteral("seq"), 1},
                            {QStringLiteral("payload"), giant}};
    const QJsonObject second{{QStringLiteral("type"), QStringLiteral("assistant")},
                             {QStringLiteral("seq"), 2},
                             {QStringLiteral("message"), QStringLiteral("done")}};
    const QByteArray bytes =
        QJsonDocument(first).toJson(QJsonDocument::Compact) + '\n' +
        QJsonDocument(second).toJson(QJsonDocument::Compact) + '\n';

    QElapsedTimer enqueueTimer;
    enqueueTimer.start();
    stream.ingestStdoutForTest(bytes);
    const qint64 enqueueMs = enqueueTimer.elapsed();
    stream.reportProcessExitForTest(0);

    check(enqueueMs < 250,
          "a giant stdout line is handed to the parser without blocking the UI");
    check(deliveryOrder.isEmpty(),
          "stdout parsing and completion are asynchronous");
    check(pump([&] {
              return !deliveryOrder.isEmpty() &&
                  deliveryOrder.last() == QLatin1String("finished");
          }),
          "queued stdout and persistence drain before process completion");
    check(events.size() == 2 &&
              events.at(0).value(QStringLiteral("seq")).toInt() == 1 &&
              events.at(1).value(QStringLiteral("seq")).toInt() == 2,
          "parsed stream events retain FIFO order");
    check(deliveryOrder ==
              QStringList{QStringLiteral("raw"), QStringLiteral("event-1"),
                          QStringLiteral("raw"), QStringLiteral("event-2"),
                          QStringLiteral("finished")},
          "finished cannot overtake raw lines or final parsed events");
    check(rawLines.size() == 2 && rawLines.first().size() < 200 &&
              rawLines.first().contains(QStringLiteral("omitted from raw view")) &&
              events.first().value(QStringLiteral("payload")).toString().size() ==
                  giant.size(),
          "oversized raw debug text is bounded while the full event is retained");
    check(persistedBeforeFinish,
          "finished is emitted only after the final transcript event is on disk");
}

// MainWindow owns every live stream as a QObject child.  Its destruction begins
// after the event loop has stopped, while parser jobs and their queued delivery
// events can still exist.  Deleting that child tree must drain those jobs without
// leaving a shared QObject behind or freeing an implicitly-shared byte buffer
// twice (the production failure aborted in ClaudeStreamSession::~ at that free).
void runQueuedParserTeardownTest()
{
    QStringList messages;
    capturedMessages = &messages;
    QtMessageHandler priorHandler = qInstallMessageHandler(captureMessages);

    auto *owner = new QObject;
    QByteArray burst;
    for (int i = 0; i < 2'000; ++i) {
        burst += QJsonDocument(
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("assistant")},
                                 {QStringLiteral("seq"), i},
                                 {QStringLiteral("message"),
                                  QString(128, QLatin1Char('x'))}})
                     .toJson(QJsonDocument::Compact);
        burst += '\n';
    }
    for (int i = 0; i < 8; ++i) {
        auto *stream = new ClaudeStreamSession(owner);
        stream->ingestStdoutForTest(burst);
    }

    delete owner;
    QCoreApplication::processEvents();
    qInstallMessageHandler(priorHandler);
    capturedMessages = nullptr;

    const bool malformedSharedObject =
        std::any_of(messages.cbegin(), messages.cend(), [](const QString &line) {
            return line.contains(
                QStringLiteral("shared QObject was deleted directly"));
        });
    check(!malformedSharedObject,
          "teardown drains queued stream parsers without deleting a shared QObject");
}

void runOrderedWriterTest()
{
    QTemporaryDir tmp;
    check(tmp.isValid(), "ordered-writer temp dir is valid");
    AgentStore first(tmp.path());
    AgentStore second(tmp.path());
    AgentSession saved;
    saved.owner = QStringLiteral("octo");
    saved.name = QStringLiteral("writer-order");
    saved = first.createSession(saved);

    first.appendEvent(saved, QJsonObject{{QStringLiteral("seq"), 1}});
    second.appendEvent(saved, QJsonObject{{QStringLiteral("seq"), 2}});
    first.appendEvent(saved, QJsonObject{{QStringLiteral("seq"), 3}});
    AgentStore::flushTranscriptWrites();
    QList<QJsonObject> loaded = second.loadEvents(saved);
    check(loaded.size() == 3 &&
              loaded.at(0).value(QStringLiteral("seq")).toInt() == 1 &&
              loaded.at(1).value(QStringLiteral("seq")).toInt() == 2 &&
              loaded.at(2).value(QStringLiteral("seq")).toInt() == 3,
          "AgentStore copies share one ordered, non-concurrent writer");

    first.clearEvents(saved);
    second.appendEvent(saved, QJsonObject{{QStringLiteral("seq"), 4}});
    loaded = first.loadEvents(saved);
    check(loaded.size() == 1 &&
              loaded.first().value(QStringLiteral("seq")).toInt() == 4,
          "clear and subsequent append keep queue order");
}

void runPendingPromptPersistenceTest()
{
    QTemporaryDir tmp;
    check(tmp.isValid(), "pending-prompt temp dir is valid");
    AgentStore store(tmp.path());
    AgentSession session;
    session.owner = QStringLiteral("octo");
    session.name = QStringLiteral("durable-follow-up");
    session.prompt = QStringLiteral("original task");
    session.pendingSteerMessage = QStringLiteral("first follow-up");
    session = store.createSession(session);

    QList<AgentSession> loaded = store.loadAllSessions();
    check(loaded.size() == 1 &&
              loaded.first().pendingSteerMessage ==
                  QLatin1String("first follow-up"),
          "a queued follow-up is stored with its agent session");

    session.pendingSteerMessage.clear();
    session.inFlightSteerMessage = QStringLiteral("first follow-up");
    check(store.saveSession(session),
          "moving a follow-up into launch is atomically persisted");
    loaded = store.loadAllSessions();
    check(loaded.size() == 1 &&
              loaded.first().pendingSteerMessage.isEmpty() &&
              loaded.first().inFlightSteerMessage ==
                  QLatin1String("first follow-up"),
          "an unacknowledged launch keeps its prompt attached after reload");
}

void runResumeFallbackOrderingTest()
{
    QTemporaryDir tmp;
    check(tmp.isValid(), "resume-fallback temp dir is valid");
    AgentStore store(tmp.path());
    AgentSession saved;
    saved.owner = QStringLiteral("octo");
    saved.name = QStringLiteral("resume-fallback");
    saved = store.createSession(saved);

    ClaudeStreamSession stream;
    QStringList order;
    int relaunches = 0;
    int exits = 0;
    QObject::connect(&stream, &ClaudeStreamSession::event,
                     [&](const QJsonObject &event) {
                         if (event.value(QStringLiteral("type")).toString() ==
                             QLatin1String("_local_notice"))
                             order.append(QStringLiteral("fallback-notice"));
                         else
                             order.append(
                                 QStringLiteral("event-%1").arg(
                                     event.value(QStringLiteral("seq")).toInt()));
                         store.appendEvent(saved, event);
                     });
    stream.setFinishBarrier([&store](std::function<void()> completion) {
        store.whenTranscriptWritesDrained(QCoreApplication::instance(),
                                          std::move(completion));
    });
    QObject::connect(&stream, &ClaudeStreamSession::finished,
                     [&](int) {
                         ++exits;
                         order.append(QStringLiteral("finished"));
                     });
    stream.configureResumeFallbackForTest(
        QStringLiteral("missing-session"), [&] {
            ++relaunches;
            order.append(QStringLiteral("relaunch"));
            const QJsonObject init{
                {QStringLiteral("type"), QStringLiteral("system")},
                {QStringLiteral("subtype"), QStringLiteral("init")},
                {QStringLiteral("session_id"), QStringLiteral("new-session")},
                {QStringLiteral("seq"), 2}};
            stream.ingestStdoutForTest(
                QJsonDocument(init).toJson(QJsonDocument::Compact) + '\n');
            stream.reportProcessExitForTest(0);
        });

    const QJsonObject resumeError{
        {QStringLiteral("type"), QStringLiteral("system")},
        {QStringLiteral("subtype"), QStringLiteral("error")},
        {QStringLiteral("error"), QStringLiteral("session not found")},
        {QStringLiteral("seq"), 1}};
    stream.ingestStdoutForTest(
        QJsonDocument(resumeError).toJson(QJsonDocument::Compact) + '\n');
    stream.reportProcessExitForTest(1);

    check(pump([&] { return exits == 1; }),
          "resume fallback and its persistence barrier reach one final exit");
    check(relaunches == 1,
          "a failed resumptive launch relaunches exactly once");
    check(order ==
              QStringList{QStringLiteral("event-1"),
                          QStringLiteral("fallback-notice"),
                          QStringLiteral("relaunch"), QStringLiteral("event-2"),
                          QStringLiteral("finished")},
          "parsed resume error and fallback notice precede relaunch output");
    const QList<QJsonObject> persisted = store.loadEvents(saved);
    check(persisted.size() == 3 &&
              persisted.at(0).value(QStringLiteral("seq")).toInt() == 1 &&
              persisted.at(1).value(QStringLiteral("type")).toString() ==
                  QLatin1String("_local_notice") &&
              persisted.at(2).value(QStringLiteral("seq")).toInt() == 2,
          "resume error, notice, and relaunched init persist in FIFO order");
}

// "Merge & clean up" deleted the session and it came back. Every writer here
// mkpaths first, so one late write — the org-task completion reply re-saving
// meta.json a second after the delete, a runner still appending — recreates the
// directory, and the next reload reads the session back into the Agents tab
// (adhoc #1718). A deleted session has to refuse writes, from any copy of the
// store, until its id is handed out again.
void runDeletedSessionStaysDeletedTest()
{
    QTemporaryDir tmp;
    check(tmp.isValid(), "deleted-session temp dir is valid");
    AgentStore store(tmp.path());
    AgentStore other(tmp.path()); // a background scan's copy of the same root
    AgentSession session;
    session.owner = QStringLiteral("octo");
    session.name = QStringLiteral("stays-deleted");
    session = store.createSession(session);
    store.appendLog(session, QStringLiteral("ran"));
    AgentStore::flushTranscriptWrites();
    const QString dir = tmp.path() + QStringLiteral("/sessions/octo-stays-deleted/") +
                        QString::number(session.id);
    check(QDir(dir).exists(), "a created session has a directory on disk");

    check(store.deleteSession(session), "deleting a session reports success");
    check(!QDir(dir).exists(), "deleting a session removes its directory");

    // The stale in-memory copies every late callback still holds.
    check(!store.saveSession(session),
          "a late saveSession for a deleted session is refused");
    other.saveSession(session);
    other.appendLog(session, QStringLiteral("late transcript line"));
    other.appendEvent(session, QJsonObject{{QStringLiteral("seq"), 1}});
    other.writePatch(session, QStringLiteral("diff --git a b\n"));
    AgentStore::flushTranscriptWrites();
    check(!QDir(dir).exists(),
          "no late write from any store copy recreates the deleted session");
    check(store.loadAllSessions().isEmpty(),
          "a deleted session does not reappear in the session list");

    // nextId hands the top id straight back out, so the tombstone must not
    // outlive it: the next run to claim that id has to be writable.
    AgentSession fresh;
    fresh.owner = session.owner;
    fresh.name = session.name;
    fresh = store.createSession(fresh);
    check(fresh.id == session.id, "the deleted id is handed back to the next run");
    check(store.saveSession(fresh) && QDir(dir).exists(),
          "a session recycling the deleted id writes normally again");
    check(store.loadAllSessions().size() == 1,
          "the recycled session is the only one listed");
}

void runRepositoryGuardrailTest()
{
    QTemporaryDir tmp;
    check(tmp.isValid(), "guardrail temp repository is valid");
    QDir root(tmp.path());
    root.mkpath(QStringLiteral("allowed"));
    root.mkpath(QStringLiteral("blocked"));
    QFile allowed(root.filePath(QStringLiteral("allowed/read.txt")));
    QFile blocked(root.filePath(QStringLiteral("blocked/secret.txt")));
    check(allowed.open(QIODevice::WriteOnly) && allowed.write("allowed") == 7,
          "guardrail allowed fixture is written");
    allowed.close();
    check(blocked.open(QIODevice::WriteOnly) && blocked.write("secret") == 6,
          "guardrail blocked fixture is written");
    blocked.close();

    QTemporaryDir trustedRepository;
    check(trustedRepository.isValid(),
          "trusted Git metadata fixture is valid");
    const QString gitDirectory =
        QDir(trustedRepository.path())
            .filePath(QStringLiteral(".git/worktrees/guarded"));
    QDir().mkpath(gitDirectory);
    const QString commonGitDirectory =
        QDir(trustedRepository.path()).filePath(QStringLiteral(".git"));
    QDir(commonGitDirectory).mkpath(QStringLiteral("objects"));
    QDir(commonGitDirectory).mkpath(QStringLiteral("refs"));
    QDir(commonGitDirectory).mkpath(QStringLiteral("hooks"));
    QFile hook(QDir(commonGitDirectory)
                   .filePath(QStringLiteral("hooks/post-commit")));
    hook.open(QIODevice::WriteOnly);
    hook.write("safe");
    hook.close();
    QFile common(QDir(gitDirectory).filePath(QStringLiteral("commondir")));
    common.open(QIODevice::WriteOnly);
    common.write("../..\n");
    common.close();
    const QString markerPath = root.filePath(QStringLiteral(".git"));
    QFile backlink(QDir(gitDirectory).filePath(QStringLiteral("gitdir")));
    backlink.open(QIODevice::WriteOnly);
    backlink.write((markerPath + QLatin1Char('\n')).toUtf8());
    backlink.close();
    QFile marker(markerPath);
    marker.open(QIODevice::WriteOnly);
    marker.write((QStringLiteral("gitdir: ") + gitDirectory +
                  QLatin1Char('\n')).toUtf8());
    marker.close();

    const QStringList normalized =
        forkmesh::agentguard::normalizeDirectories(
            {QStringLiteral("src/../allowed/"), QStringLiteral("allowed/sub"),
             QStringLiteral("../escape"), QStringLiteral("/absolute")});
    check(normalized == QStringList{QStringLiteral("allowed")},
          "repository guardrail entries normalize, deduplicate, and reject escapes");
    check(forkmesh::agentguard::pathAllowed(
              tmp.path(), normalized, QStringLiteral("allowed/read.txt")) &&
              !forkmesh::agentguard::pathAllowed(
                  tmp.path(), normalized, QStringLiteral("blocked/secret.txt")) &&
              !forkmesh::agentguard::pathAllowed(
                  tmp.path(), normalized, QStringLiteral("/etc/passwd")),
          "tool-call path checks allow only configured repository directories");
    check(forkmesh::agentguard::toolViolation(
              QStringLiteral("Read"),
              QJsonObject{{QStringLiteral("file_path"),
                           QStringLiteral("blocked/secret.txt")}},
              tmp.path(), normalized) == QStringLiteral("blocked/secret.txt"),
          "a structured forbidden tool call is identified before execution");
    check(forkmesh::agentguard::approvalDirectory(
              tmp.path(), QStringLiteral("blocked/secret.txt")) ==
                  QStringLiteral("blocked") &&
              forkmesh::agentguard::approvalDirectory(
                  tmp.path(), QStringLiteral("blocked/new.txt")) ==
                  QStringLiteral("blocked") &&
              forkmesh::agentguard::approvalDirectory(
                  tmp.path(), QStringLiteral("README.md")) ==
                  QStringLiteral(".") &&
              forkmesh::agentguard::approvalDirectory(
                  tmp.path(), QStringLiteral("/var/tmp/outside.txt")).isEmpty(),
          "guardrail approvals select the narrowest existing repository "
          "directory and refuse host paths");

    {
        QTemporaryDir choicesRoot;
        for (const char *name : {"src", "tests", "node_modules", "misc", ".git"})
            QDir(choicesRoot.path()).mkpath(QString::fromLatin1(name));
        const QList<forkmesh::agentguard::DirectoryChoice> choices =
            forkmesh::agentguard::directoryChoices(
                choicesRoot.path(),
                {QStringLiteral("src"), QStringLiteral("gone/deep")});
        QStringList order;
        for (const auto &choice : choices)
            order.append(choice.path);
        auto choiceFor = [&choices](const QString &path) {
            for (const auto &choice : choices)
                if (choice.path == path)
                    return choice;
            return forkmesh::agentguard::DirectoryChoice();
        };
        check(order == QStringList{QStringLiteral("."), QStringLiteral("src"),
                                   QStringLiteral("tests"),
                                   QStringLiteral("gone/deep"),
                                   QStringLiteral("misc"),
                                   QStringLiteral("node_modules")},
              "the guardrail checklist lists the whole repository, then "
              "recommended folders, then the rest, and never .git");
        check(!choiceFor(QStringLiteral(".")).allowed &&
                  choiceFor(QStringLiteral("src")).allowed &&
                  !choiceFor(QStringLiteral("tests")).allowed &&
                  !choiceFor(QStringLiteral("gone/deep")).exists &&
                  choiceFor(QStringLiteral("gone/deep")).allowed &&
                  choiceFor(QStringLiteral("node_modules"))
                      .description.contains(QStringLiteral("dependencies")),
              "guardrail checklist rows carry allowed, missing, and "
              "description state");
        check(forkmesh::agentguard::recommendedDirectories(choicesRoot.path()) ==
                  (QStringList{QStringLiteral("src"), QStringLiteral("tests")}),
              "recommended guardrail directories are the common source and "
              "test folders present in the checkout");
        check(forkmesh::agentguard::policyNarrowed(
                  {QStringLiteral(".")}, {QStringLiteral("src")}) &&
                  !forkmesh::agentguard::policyNarrowed(
                      {QStringLiteral("src/a")}, {QStringLiteral("src")}) &&
                  !forkmesh::agentguard::policyNarrowed(
                      {QStringLiteral("src")},
                      {QStringLiteral("src"), QStringLiteral("tests")}),
              "only a guardrail change that drops coverage counts as narrowing");
    }

    forkmesh::agentguard::Config config;
    config.enabled = true;
    config.allowedDirectories = normalized;
    config.repositoryRoot = trustedRepository.path();
    config.provider = QStringLiteral("codex");
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    QTemporaryDir providerState;
    check(providerState.isValid(), "provider-state fixture is valid");
    environment.insert(QStringLiteral("CODEX_HOME"), providerState.path());
    const QString scratch = root.filePath(QStringLiteral("scratch"));
    for (const QString &entry : AgentJail::envEntries(scratch)) {
        const int equals = entry.indexOf(QLatin1Char('='));
        environment.insert(entry.left(equals), entry.mid(equals + 1));
    }
    const auto run = [&](const QString &command, QString *output = nullptr,
                         const QStringList &additionalReadOnly = QStringList()) {
        const forkmesh::agentguard::LaunchCommand launch =
            forkmesh::agentguard::restrictedCommand(
                QStringLiteral("/bin/sh"),
                {QStringLiteral("-c"), command}, tmp.path(), config,
                environment, additionalReadOnly);
        if (!launch.error.isEmpty()) {
            if (output)
                *output = launch.error;
            return -999;
        }
        QProcess process;
        process.setWorkingDirectory(tmp.path());
        process.setProcessEnvironment(environment);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(launch.program, launch.arguments);
        if (!process.waitForStarted(5000) || !process.waitForFinished(10000)) {
            process.kill();
            process.waitForFinished(1000);
            if (output)
                *output = process.errorString();
            return -998;
        }
        if (output)
            *output = QString::fromUtf8(process.readAll());
        return process.exitCode();
    };

#ifdef Q_OS_LINUX
    QString allowedOutput;
    const int allowedExit = run(QStringLiteral("cat allowed/read.txt"),
                                &allowedOutput);
    check(allowedExit == 0 && allowedOutput == QLatin1String("allowed"),
          "Landlock lets an agent read an allowed repository directory");
    const QString providerFile =
        QDir(providerState.path()).filePath(QStringLiteral("state.txt"));
    QString providerOutput;
    const int providerExit =
        run(QStringLiteral("printf state > '%1'").arg(providerFile),
            &providerOutput);
    check(providerExit == 0 && QFileInfo::exists(providerFile),
          "provider login state remains available inside the guarded runtime");
    QString blockedOutput;
    const int blockedExit = run(QStringLiteral("cat blocked/secret.txt"),
                                &blockedOutput);
    check(blockedExit != 0 &&
              (blockedOutput.contains(QStringLiteral("Permission denied"),
                                      Qt::CaseInsensitive) ||
               blockedOutput.contains(QStringLiteral("Operation not permitted"),
                                      Qt::CaseInsensitive)),
          "Landlock blocks a subprocess from reading a disallowed repository directory");
    QString hostOutput;
    const int hostExit = run(QStringLiteral("cat /etc/passwd"), &hostOutput);
    // /etc is a read-only runtime exception, so use owner HOME as the unrelated
    // host-data assertion instead of machine configuration needed by TLS/DNS.
    QTemporaryDir unrelated;
    QFile unrelatedFile(unrelated.filePath(QStringLiteral("private.txt")));
    unrelatedFile.open(QIODevice::WriteOnly);
    unrelatedFile.write("private");
    unrelatedFile.close();
    QString privateOutput;
    const int privateExit = run(
        QStringLiteral("cat '%1'").arg(unrelatedFile.fileName()),
        &privateOutput);
    check(privateExit != 0,
          "Landlock blocks unrelated host data from agent subprocesses");
    QString truncateOutput;
    const int truncateExit = run(
        QStringLiteral("python3 -c 'import os; os.truncate(\"%1\", 0)'")
            .arg(unrelatedFile.fileName()),
        &truncateOutput);
    check(truncateExit != 0 && unrelatedFile.size() == 7,
          "Landlock blocks direct truncation outside the allowlist");
    check(hostExit == 0,
          "guardrails retain read-only machine configuration needed by tools");
    QString selfProcessOutput;
    const int selfProcessExit = run(
        QStringLiteral(
            "exec python3 -c 'open(\"/proc/self/status\").read(1)'"),
        &selfProcessOutput);
    QString parentProcessOutput;
    const int parentProcessExit =
        run(QStringLiteral("head -c 0 /proc/$PPID/environ"),
            &parentProcessOutput);
    check(selfProcessExit == 0 && parentProcessExit != 0,
          "an agent can inspect itself without reading ForkMesh process state");
    QTemporaryDir runtime;
    QFile runtimeFile(runtime.filePath(QStringLiteral("agent.py")));
    QFile runtimeNeighbor(runtime.filePath(QStringLiteral("private.txt")));
    runtimeFile.open(QIODevice::WriteOnly);
    runtimeFile.write("runtime");
    runtimeFile.close();
    runtimeNeighbor.open(QIODevice::WriteOnly);
    runtimeNeighbor.write("private");
    runtimeNeighbor.close();
    QString runtimeOutput;
    const int runtimeExit =
        run(QStringLiteral("cat '%1'").arg(runtimeFile.fileName()),
            &runtimeOutput, {runtimeFile.fileName()});
    QString neighborOutput;
    const int neighborExit =
        run(QStringLiteral("cat '%1'").arg(runtimeNeighbor.fileName()),
            &neighborOutput, {runtimeFile.fileName()});
    check(runtimeExit == 0 && runtimeOutput == QLatin1String("runtime") &&
              neighborExit != 0,
          "an exact provider runtime file is readable without exposing its directory");
    const QString objectFile =
        QDir(commonGitDirectory).filePath(QStringLiteral("objects/new-object"));
    QString objectOutput;
    const int objectExit =
        run(QStringLiteral("printf object > '%1'").arg(objectFile),
            &objectOutput);
    QString hookOutput;
    const int hookExit =
        run(QStringLiteral("printf evil > '%1'").arg(hook.fileName()),
            &hookOutput);
    hook.open(QIODevice::ReadOnly);
    const QByteArray hookContents = hook.readAll();
    hook.close();
    check(objectExit == 0 && QFileInfo::exists(objectFile) && hookExit != 0 &&
              hookContents == QByteArrayLiteral("safe"),
          "Git object storage stays writable while shared hooks stay read-only");
    environment.insert(QStringLiteral("CODEX_HOME"), QDir::rootPath());
    QString broadProviderOutput;
    const int broadProviderExit =
        run(QStringLiteral("true"), &broadProviderOutput);
    check(broadProviderExit != 0 &&
              broadProviderOutput.contains(QStringLiteral("too broad")),
          "a broad provider-state override cannot expand the writable boundary");
    environment.insert(QStringLiteral("CODEX_HOME"), providerState.path());
#else
    QString failure;
    check(run(QStringLiteral("true"), &failure) != 0 &&
              failure.contains(QStringLiteral("require Linux Landlock")),
          "guardrails fail closed on hosts without Linux Landlock");
#endif

    marker.open(QIODevice::WriteOnly | QIODevice::Truncate);
    marker.write("gitdir: /tmp\n");
    marker.close();
    check(forkmesh::agentguard::restrictedCommand(
              QStringLiteral("/bin/sh"),
              {QStringLiteral("-c"), QStringLiteral("true")}, tmp.path(),
              config, environment)
              .error.contains(QStringLiteral("trusted repository")),
          "a modified worktree marker cannot expand the Git metadata exception");

    config.allowedDirectories = {QStringLiteral("missing")};
    check(!forkmesh::agentguard::restrictedCommand(
               QStringLiteral("/bin/sh"), {QStringLiteral("-c"),
                                            QStringLiteral("true")},
               tmp.path(), config, environment)
               .error.isEmpty(),
          "a missing allowed directory refuses the agent launch");
}

// The Guardrails tab reads its counts from this ledger, so the numbers it
// shows have to survive a restart and age out of the hourly trend.
void runGuardrailLedgerTest()
{
    QTemporaryDir tmp;
    check(tmp.isValid(), "guardrail ledger temp dir is valid");
    const QString path =
        QDir(tmp.path()).filePath(QStringLiteral("guardrails.json"));
    AgentGuardrailLedger ledger(path);
    int changes = 0;
    ledger.changed = [&changes] { ++changes; };
    const qint64 hour = AgentGuardrailLedger::kActivityBucketMs;
    const qint64 now = 1000000 * hour;
    const QString repo = QStringLiteral("me/r");
    ledger.recordBlocked(repo, QStringLiteral("Read"),
                         QStringLiteral("blocked/secret.txt"), false, now - 2 * hour);
    ledger.recordBlocked(repo, QStringLiteral("Bash"),
                         QStringLiteral("/etc/passwd"), true, now);
    ledger.recordBlocked(repo, QStringLiteral("Read"),
                         QStringLiteral("blocked/secret.txt"), false, now);
    ledger.recordDecision(repo, true, now);
    ledger.recordDecision(QStringLiteral("other/x"), false, now);
    ledger.recordAgentsStopped(repo, 2, now);
    ledger.recordAgentsStopped(repo, 0, now);
    const AgentGuardrailRepoStats stats = ledger.stats(repo);
    check(stats.blocked == 3 && stats.blockedOutside == 1 &&
              stats.approved == 1 && stats.denied == 0 &&
              stats.agentsStopped == 2 &&
              stats.tools.value(QStringLiteral("Read")) == 2 &&
              stats.recentPaths ==
                  (QStringList{QStringLiteral("blocked/secret.txt"),
                               QStringLiteral("/etc/passwd")}) &&
              changes == 6,
          "the guardrail ledger counts blocks, answers, and stopped agents per "
          "repository");
    const int buckets = AgentGuardrailLedger::kActivityBuckets;
    const AgentGuardrailTotals totals = ledger.totals(now);
    check(totals.blocked == 3 && totals.denied == 1 &&
              totals.repositoriesWithEvents == 2 &&
              totals.recentBlocked.size() == buckets &&
              totals.recentBlocked.last() == 2 &&
              totals.recentBlocked.at(buckets - 3) == 1 &&
              ledger.totals(now + 30 * hour).recentBlocked ==
                  QVector<qint64>(buckets, 0),
          "guardrail activity buckets blocked calls by hour and ages out after "
          "a day");

    QString error;
    check(ledger.save(&error), "the guardrail ledger saves to its JSON file");
    AgentGuardrailLedger reloaded(path);
    check(reloaded.load(&error) && reloaded.stats(repo).blocked == 3 &&
              reloaded.stats(repo).recentPaths.size() == 2 &&
              reloaded.totals(now).recentBlocked.last() == 2 &&
              reloaded.repositories() ==
                  (QStringList{QStringLiteral("me/r"), QStringLiteral("other/x")}),
          "the guardrail ledger round-trips through its JSON file");
    reloaded.clear();
    check(!reloaded.hasEvents() && reloaded.totals(now).blocked == 0,
          "clearing the guardrail ledger forgets every event");
}

void runProviderNeutralRuntimePersistenceTest()
{
    AgentSession legacyCodex = AgentSession::fromJson(
        QJsonObject{{QStringLiteral("provider"), QStringLiteral("codex")}});
    check(legacyCodex.effectiveRuntimeId() == AgentRuntimeIds::CodexAppServer,
          "legacy Codex sessions migrate to the neutral app-server runtime id");
    check(legacyCodex.schemaVersion == AgentSessionSchema::CurrentVersion,
          "legacy unversioned sessions migrate to the current schema");
    check(legacyCodex.runtimeProtocolVersion == 1,
          "legacy sessions migrate to the first runtime protocol");

    AgentSession legacyClaude = AgentSession::fromJson(
        QJsonObject{{QStringLiteral("provider"), QStringLiteral("claude-code")}});
    check(legacyClaude.effectiveRuntimeId() == AgentRuntimeIds::ClaudeStreamJson,
          "legacy stream sessions migrate to the neutral stream runtime id");

    AgentSession explicitRuntime;
    explicitRuntime.provider = QStringLiteral("future-provider");
    explicitRuntime.runtimeId = QStringLiteral("runtime.compatible-v2");
    explicitRuntime = AgentSession::fromJson(explicitRuntime.toJson());
    check(explicitRuntime.runtimeId == QStringLiteral("runtime.compatible-v2"),
          "session persistence retains a runtime independently of provider");
    check(explicitRuntime.toJson().value(QStringLiteral("schemaVersion")).toInt() ==
              AgentSessionSchema::CurrentVersion,
          "session persistence records the current schema version");

    const AgentRuntimeDescriptor compatible{
        explicitRuntime.runtimeId, QStringLiteral("Compatible runtime"), 1, 2,
        {AgentRuntimeCapabilities::SessionLifecycle}};
    QString reason;
    check(explicitRuntime.canResumeWithRuntime(compatible, &reason) &&
              reason.isEmpty(),
          "a supported saved protocol can resume through its runtime");

    AgentSession incompatible = explicitRuntime;
    incompatible.runtimeProtocolVersion = 3;
    check(!incompatible.canResumeWithRuntime(compatible, &reason) &&
              reason.contains(QStringLiteral("protocol 3")),
          "an incompatible saved protocol is rejected with an exportable reason");

    AgentSession future = explicitRuntime;
    future.schemaVersion = AgentSessionSchema::CurrentVersion + 1;
    check(!future.canResumeWithRuntime(compatible, &reason) &&
              reason.contains(QStringLiteral("schema"), Qt::CaseInsensitive),
          "a future session schema is retained but not resumed unsafely");
}

// 2026-09-22: the desktop runs as a systemd user service, so the PATH it
// hands a child is the bare system default -- no ~/.local/bin, where the
// native installer puts `claude`. A project-locked launch runs a plain -c
// shell that never reads ~/.profile, and every such session exited 127
// ("Claude Code exited before starting a session"). The launcher itself has
// to put the user install directories on PATH.
void runLockedLaunchFindsUserInstalledCliTest()
{
#ifdef Q_OS_LINUX
    QTemporaryDir home;
    QTemporaryDir worktree;
    check(home.isValid() && worktree.isValid(),
          "user-install PATH fixtures are valid");
    QDir(home.path()).mkpath(QStringLiteral(".local/bin"));
    QFile cli(home.filePath(QStringLiteral(".local/bin/claude")));
    check(cli.open(QIODevice::WriteOnly) &&
              cli.write("#!/bin/sh\n"
                        "printf '{\"type\":\"system\",\"subtype\":\"init\","
                        "\"session_id\":\"path-probe\"}\\n'\n") > 0,
          "a stand-in claude is installed under ~/.local/bin");
    cli.close();
    cli.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                       QFileDevice::ExeOwner);

    const QByteArray savedHome = qgetenv("HOME");
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("HOME", home.path().toUtf8());
    qputenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin");

    forkmesh::agentguard::Config config;
    config.projectLocked = true;
    config.provider = QStringLiteral("claude-code");
    ClaudeStreamSession session;
    session.setGuardrails(config);
    int exits = 0;
    int exitCode = 0;
    QString sessionId;
    QString stderrSeen;
    QObject::connect(&session, &ClaudeStreamSession::finished,
                     [&](int code) {
                         ++exits;
                         exitCode = code;
                     });
    QObject::connect(&session, &ClaudeStreamSession::event,
                     [&](const QJsonObject &event) {
                         if (sessionId.isEmpty())
                             sessionId = event.value(QStringLiteral("session_id"))
                                             .toString();
                     });
    QObject::connect(&session, &ClaudeStreamSession::stderrText,
                     [&](const QString &text) { stderrSeen += text; });
    session.start(worktree.path(), {}, QString());
    const bool ended = pump([&exits] { return exits > 0; }, 10000);

    qputenv("HOME", savedHome);
    qputenv("PATH", savedPath);

    check(ended, "the locked launch with a system-default PATH ends");
    check(exitCode == 0 && sessionId == QLatin1String("path-probe"),
          qPrintable(QStringLiteral(
                         "a locked launch finds claude in ~/.local/bin when "
                         "the desktop's PATH lacks it (exit %1): %2")
                         .arg(exitCode)
                         .arg(stderrSeen.trimmed())));
#endif
}


void runHostControlProtocolTest()
{
#ifdef Q_OS_LINUX
    QTemporaryDir home;
    QTemporaryDir worktree;
    check(home.isValid() && worktree.isValid(), "host-control fixtures are valid");
    QDir(home.path()).mkpath(QStringLiteral(".local/bin"));
    QFile profile(home.filePath(QStringLiteral(".profile")));
    check(profile.open(QIODevice::WriteOnly) &&
              profile.write("PATH=\"$HOME/.local/bin:$PATH\"\n") > 0,
          "the fixture's login profile puts ~/.local/bin on PATH");
    profile.close();
    const QString argsFile = home.filePath(QStringLiteral("args.txt"));
    const QString stdinFile = home.filePath(QStringLiteral("stdin.jsonl"));
    QFile cli(home.filePath(QStringLiteral(".local/bin/claude")));
    const QByteArray script =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$@\" > \"$FM_ARGS\"\n"
        "printf '{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"cli-7\"}\\n'\n"
        "printf '{\"type\":\"control_request\",\"request_id\":\"r1\",\"request\":{\"subtype\":"
        "\"can_use_tool\",\"tool_name\":\"Write\",\"input\":{\"file_path\":\"/w/a.txt\","
        "\"content\":\"hi\"},\"tool_use_id\":\"tu1\",\"permission_suggestions\":[{\"type\":"
        "\"setMode\",\"mode\":\"acceptEdits\",\"destination\":\"session\"}]}}\\n'\n"
        "printf '{\"type\":\"control_request\",\"request_id\":\"r2\",\"request\":{\"subtype\":"
        "\"hook_callback\",\"callback_id\":\"x\"}}\\n'\n"
        "printf '{\"type\":\"control_request\",\"request_id\":\"r3\",\"request\":{\"subtype\":"
        "\"request_user_dialog\",\"dialog_kind\":\"lsp_recommendation\",\"payload\":{}}}\\n'\n"
        "printf '{\"type\":\"control_request\",\"request_id\":\"r4\",\"request\":{\"subtype\":"
        "\"elicitation\",\"mcp_server_name\":\"t\",\"message\":\"m\"}}\\n'\n"
        "printf '{\"type\":\"control_cancel_request\",\"request_id\":\"r4\"}\\n'\n"
        "while IFS= read -r line; do\n"
        "  printf '%s\\n' \"$line\" >> \"$FM_STDIN\"\n"
        "  case \"$line\" in *generate_session_title*)\n"
        "    id=$(printf '%s' \"$line\" | sed 's/.*\"request_id\":\"\\([^\"]*\\)\".*/\\1/')\n"
        "    printf '{\"type\":\"control_response\",\"response\":{\"subtype\":\"success\","
        "\"request_id\":\"%s\",\"response\":{\"title\":\"Fix the login bug\"}}}\\n' \"$id\";;\n"
        "  esac\n"
        "done\n";
    check(cli.open(QIODevice::WriteOnly) && cli.write(script) > 0,
          "a scripted claude is installed under ~/.local/bin");
    cli.close();
    cli.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                       QFileDevice::ExeOwner);

    const QByteArray savedHome = qgetenv("HOME");
    const QByteArray savedPath = qgetenv("PATH");
    qputenv("HOME", home.path().toUtf8());
    qputenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/bin");

    ClaudeStreamSession session;
    QList<QJsonObject> events;
    QString stderrSeen;
    QObject::connect(&session, &ClaudeStreamSession::event,
                     [&events](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &ClaudeStreamSession::stderrText,
                     [&stderrSeen](const QString &text) { stderrSeen += text; });
    AgentRuntimeLaunch launch;
    launch.workingDirectory = worktree.path();
    launch.environment = {QStringLiteral("FM_ARGS=") + argsFile,
                          QStringLiteral("FM_STDIN=") + stdinFile};
    launch.initialPrompt = QStringLiteral("Fix the login bug");
    launch.permissionMode = QStringLiteral("default");
    launch.options.insert(QStringLiteral("hostControl"), true);
    session.startRuntime(launch);

    const auto keyed = [&events](const QString &key) {
        int seen = 0;
        for (const QJsonObject &event : events)
            if (event.value(forkmesh::claudecontrol::kPromptKeyField).toString() == key)
                ++seen;
        return seen;
    };
    const bool prompted = pump([&] { return keyed(QStringLiteral("claude-input-r4")) >= 2; }, 10000);
    check(prompted, qPrintable(QStringLiteral("the permission prompt, elicitation and its "
                                              "cancel reach the owner (%1 events) %2")
                                   .arg(events.size())
                                   .arg(stderrSeen.trimmed().left(400))));
    check(keyed(QStringLiteral("claude-approval-r1")) == 1 &&
              session.hasPendingPrompt(QStringLiteral("claude-approval-r1")),
          "a permission request waits under its card id");
    check(!session.hasPendingPrompt(QStringLiteral("claude-input-r4")),
          "a cancelled request is no longer pending");
    bool requestLeaked = false;
    for (const QJsonObject &event : events) {
        const QString id = event.value(QStringLiteral("request_id")).toString();
        if (id == QLatin1String("r2") || id == QLatin1String("r3"))
            requestLeaked = true;
    }
    check(!requestLeaked, "requests nobody renders never mark the session as waiting");
    check(session.cliSessionId() == QLatin1String("cli-7"), "the CLI's session id is learned");

    AgentRuntimeCommand answer{1, AgentRuntimeCommandTypes::RequestRespond,
                               QStringLiteral("claude-approval-r1"), 0,
                               {{QStringLiteral("answer"),
                                 forkmesh::claudecontrol::kAllowAndRemember}}};
    check(session.sendCommand(answer), "the card's answer is delivered");
    check(session.permissionMode() == QLatin1String("acceptEdits"),
          "remembering a mode switch updates the session's mode");
    check(!session.hasPendingPrompt(QStringLiteral("claude-approval-r1")),
          "an answered request is no longer pending");
    check(!session.sendCommand(answer),
          "a second click on an answered approval card is refused, not sent as a tool result");

    QString title;
    check(session.generateSessionTitle(QStringLiteral("Fix the login bug"),
                                       [&title](const QString &named) { title = named; }),
          "a title request is sent");
    check(pump([&title] { return !title.isEmpty(); }, 5000) &&
              title == QLatin1String("Fix the login bug"),
          "Claude Code's session name comes back");
    check(session.renameSession(QStringLiteral("Login fix")), "a rename is sent");

    QList<QJsonObject> written;
    pump([&] {
        written.clear();
        QFile in(stdinFile);
        if (!in.open(QIODevice::ReadOnly))
            return false;
        for (const QByteArray &line : in.readAll().split('\n'))
            if (!line.trimmed().isEmpty())
                written.append(QJsonDocument::fromJson(line).object());
        for (const QJsonObject &line : written)
            if (line.value(QStringLiteral("request")).toObject().value(QStringLiteral("subtype")) ==
                QLatin1String("rename_session"))
                return true;
        return false;
    }, 5000);
    session.stop();
    qputenv("HOME", savedHome);
    qputenv("PATH", savedPath);

    check(!written.isEmpty() &&
              written.first().value(QStringLiteral("request")).toObject()
                      .value(QStringLiteral("subtype")).toString() == QLatin1String("initialize"),
          "the handshake is the first line Claude Code reads");
    check(written.size() > 1 && written.at(1).value(QStringLiteral("type")).toString() ==
                                    QLatin1String("user"),
          "the task follows the handshake");
    bool refusedHook = false;
    bool answeredDialog = false;
    bool allowed = false;
    bool renamed = false;
    for (const QJsonObject &line : written) {
        const QJsonObject response = line.value(QStringLiteral("response")).toObject();
        const QString id = response.value(QStringLiteral("request_id")).toString();
        if (id == QLatin1String("r2"))
            refusedHook = response.value(QStringLiteral("subtype")).toString() == QLatin1String("error");
        if (id == QLatin1String("r3"))
            answeredDialog = true;
        if (id == QLatin1String("r1"))
            allowed = response.value(QStringLiteral("response")).toObject()
                          .value(QStringLiteral("updatedPermissions")).toArray().size() == 1;
        const QJsonObject request = line.value(QStringLiteral("request")).toObject();
        if (request.value(QStringLiteral("subtype")) == QLatin1String("rename_session"))
            renamed = request.value(QStringLiteral("session_id")).toString() == QLatin1String("cli-7") &&
                      request.value(QStringLiteral("source")).toString() == QLatin1String("host");
    }
    check(refusedHook, "an unsupported request is refused instead of left hanging");
    check(!answeredDialog, "an undeclared dialog kind is never answered");
    check(allowed, "the approval carries Claude's suggested permission update");
    check(renamed, "a rename names the CLI session and says the host chose it");
    QFile args(argsFile);
    const QString argv = args.open(QIODevice::ReadOnly) ? QString::fromUtf8(args.readAll()) : QString();
    check(argv.contains(QStringLiteral("--permission-prompt-tool\nstdio")),
          "host control routes permission prompts over stdio");
#endif
}

} // namespace

int main(int argc, char *argv[])
{
    QTemporaryDir appData;
    qputenv("XDG_DATA_HOME", appData.path().toUtf8());
    QCoreApplication app(argc, argv);
    runLaunchFailureTest();
    runFinalTeardownProcessOwnershipTest();
    runUnstartedSessionTest();
    runLargeOrderedStreamTest();
    runQueuedParserTeardownTest();
    runOrderedWriterTest();
    runPendingPromptPersistenceTest();
    runResumeFallbackOrderingTest();
    runDeletedSessionStaysDeletedTest();
    runRepositoryGuardrailTest();
    runLockedLaunchFindsUserInstalledCliTest();
    runHostControlProtocolTest();
    runProviderNeutralRuntimePersistenceTest();
    runGuardrailLedgerTest();
    if (failures == 0)
        qInfo("All agent transport tests passed.");
    else
        qCritical("%d agent transport test(s) failed.", failures);
    return failures == 0 ? 0 : 1;
}
