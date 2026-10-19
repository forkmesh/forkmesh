#include "../src/CodexRateLimitsProbe.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (condition)
        qInfo("PASS: %s", what);
    else {
        qCritical("FAIL: %s", what);
        ++failures;
    }
}

template <typename Predicate> bool pump(Predicate predicate, int timeoutMs = 5000)
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

void writeMessage(QFile &output, const QJsonObject &message)
{
    output.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
    output.flush();
}

QJsonObject window(int used, qint64 resetsAt, int minutes)
{
    return QJsonObject{{QStringLiteral("usedPercent"), used},
                       {QStringLiteral("resetsAt"), resetsAt},
                       {QStringLiteral("windowDurationMins"), minutes}};
}

QJsonObject generalLimit()
{
    return QJsonObject{{QStringLiteral("limitId"), QStringLiteral("codex")},
                       {QStringLiteral("primary"), window(31, 1'800'000'000, 300)},
                       {QStringLiteral("secondary"),
                        window(64, 1'800'500'000, 10'080)}};
}

QJsonObject sparkLimit()
{
    return QJsonObject{{QStringLiteral("limitId"), QStringLiteral("spark")},
                       {QStringLiteral("limitName"), QStringLiteral("Spark")},
                       {QStringLiteral("primary"), window(12, 1'800'000'100, 300)},
                       {QStringLiteral("secondary"), QJsonValue(QJsonValue::Null)}};
}

int runStub(const QStringList &arguments)
{
    if (arguments.size() < 4)
        return 2;
    const QString scenario = arguments.at(2);
    QFile input;
    QFile output;
    if (!input.open(stdin, QIODevice::ReadOnly) ||
        !output.open(stdout, QIODevice::WriteOnly))
        return 3;
    QFile log(arguments.at(3));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return 4;
    const QJsonObject env{
        {QStringLiteral("env"),
         QJsonObject{{QStringLiteral("CODEX_HOME"),
                      qEnvironmentVariable("CODEX_HOME")},
                     {QStringLiteral("OPENAI_API_KEY"),
                      qEnvironmentVariableIsSet("OPENAI_API_KEY")},
                     {QStringLiteral("CODEX_API_KEY"),
                      qEnvironmentVariableIsSet("CODEX_API_KEY")},
                     {QStringLiteral("cwd"), QDir::currentPath()}}}};
    log.write(QJsonDocument(env).toJson(QJsonDocument::Compact) + '\n');
    log.flush();
    if (scenario == QStringLiteral("exit"))
        return 7;

    for (;;) {
        const QByteArray line = input.readLine();
        if (line.isEmpty())
            return 0;
        log.write(line);
        log.flush();
        if (scenario == QStringLiteral("silent"))
            continue;
        const QJsonObject message = QJsonDocument::fromJson(line).object();
        const QString method = message.value(QStringLiteral("method")).toString();
        const QJsonValue id = message.value(QStringLiteral("id"));
        if (method == QStringLiteral("initialize")) {
            output.write("not json\n");
            writeMessage(output,
                         QJsonObject{{QStringLiteral("method"),
                                      QStringLiteral("account/updated")},
                                     {QStringLiteral("params"), QJsonObject()}});
            writeMessage(output,
                         QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("result"),
                                      QJsonObject{{QStringLiteral("userAgent"),
                                                   QStringLiteral("stub")}}}});
        } else if (method == QStringLiteral("account/rateLimits/read")) {
            if (scenario == QStringLiteral("rpc-error")) {
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("id"), id},
                                {QStringLiteral("error"),
                                 QJsonObject{{QStringLiteral("code"), -32600},
                                             {QStringLiteral("message"),
                                              QStringLiteral("not signed in")}}}});
                continue;
            }
            writeMessage(
                output,
                QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("rateLimits"), generalLimit()},
                                 {QStringLiteral("rateLimitsByLimitId"),
                                  QJsonObject{{QStringLiteral("spark"),
                                               sparkLimit()}}}}}});
        }
    }
}

QList<QJsonObject> readLog(const QString &path)
{
    QList<QJsonObject> messages;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return messages;
    while (!file.atEnd()) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readLine());
        if (document.isObject())
            messages.append(document.object());
    }
    return messages;
}

struct Outcome {
    int calls = 0;
    bool ok = false;
    QJsonObject rateLimits;
    QJsonObject byId;
    QString error;
};

void connectOutcome(CodexRateLimitsProbe &probe, Outcome &outcome)
{
    QObject::connect(&probe, &CodexRateLimitsProbe::finished,
                     [&outcome](bool ok, const QJsonObject &rateLimits,
                                const QJsonObject &byId, const QString &error) {
                         ++outcome.calls;
                         outcome.ok = ok;
                         outcome.rateLimits = rateLimits;
                         outcome.byId = byId;
                         outcome.error = error;
                     });
}

void runSuccessTest(const QString &executable)
{
    QTemporaryDir temp;
    check(temp.isValid(), "success temp dir created");
    const QString home = temp.filePath(QStringLiteral("codex-home"));
    QDir().mkpath(home);
    const QString logPath = temp.filePath(QStringLiteral("client.jsonl"));
    qputenv("OPENAI_API_KEY", "sk-test-not-real");
    qputenv("CODEX_API_KEY", "test-not-real");
    CodexRateLimitsProbe probe;
    probe.setAppServerCommand(executable, {QStringLiteral("--probe-stub"),
                                           QStringLiteral("ok"), logPath});
    Outcome outcome;
    connectOutcome(probe, outcome);
    probe.start(home, 5000);
    check(pump([&] { return outcome.calls > 0; }), "probe reports a result");
    qunsetenv("OPENAI_API_KEY");
    qunsetenv("CODEX_API_KEY");
    check(outcome.ok, "probe succeeds against a rate-limit answer");
    check(outcome.error.isEmpty(), "a successful probe has no error");
    check(outcome.rateLimits.value(QStringLiteral("limitId")).toString() ==
              QStringLiteral("codex"),
          "probe returns the general rateLimits snapshot");
    check(outcome.byId.contains(QStringLiteral("spark")) &&
              outcome.byId.contains(QStringLiteral("codex")),
          "probe keeps per-limit snapshots and adds the general one by its id");
    check(outcome.byId.value(QStringLiteral("codex")).toObject() ==
              generalLimit(),
          "the general snapshot is stored unchanged under its limit id");
    check(!probe.running(), "probe stops the app-server after the answer");

    const QList<QJsonObject> sent = readLog(logPath);
    check(sent.size() == 4, "stub saw its environment and three messages");
    if (sent.size() == 4) {
        const QJsonObject env = sent.at(0).value(QStringLiteral("env")).toObject();
        check(env.value(QStringLiteral("CODEX_HOME")).toString() ==
                  QDir::cleanPath(home),
              "CODEX_HOME points at the account's config folder");
        check(!env.value(QStringLiteral("OPENAI_API_KEY")).toBool() &&
                  !env.value(QStringLiteral("CODEX_API_KEY")).toBool(),
              "API keys are stripped so the read is the signed-in account's");
        check(QDir(env.value(QStringLiteral("cwd")).toString()).canonicalPath() ==
                  QDir(home).canonicalPath(),
              "app-server runs in the account's config folder");
        check(sent.at(1).value(QStringLiteral("method")).toString() ==
                      QStringLiteral("initialize") &&
                  sent.at(1)
                      .value(QStringLiteral("params"))
                      .toObject()
                      .value(QStringLiteral("capabilities"))
                      .toObject()
                      .value(QStringLiteral("experimentalApi"))
                      .toBool(),
              "initialize comes first with experimentalApi");
        check(sent.at(2).value(QStringLiteral("method")).toString() ==
                      QStringLiteral("initialized") &&
                  !sent.at(2).contains(QStringLiteral("id")),
              "initialized is a notification after initialize");
        check(sent.at(3).value(QStringLiteral("method")).toString() ==
                      QStringLiteral("account/rateLimits/read") &&
                  sent.at(3).value(QStringLiteral("params")).isNull(),
              "account/rateLimits/read follows with null params");
    }
    QCoreApplication::processEvents();
    check(outcome.calls == 1, "probe reports exactly once");
}

void runFailureTest(const QString &executable, const QString &scenario,
                    int timeoutMs, const QString &expectedError,
                    const char *what)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("client.jsonl"));
    CodexRateLimitsProbe probe;
    probe.setAppServerCommand(executable,
                              {QStringLiteral("--probe-stub"), scenario, logPath});
    Outcome outcome;
    connectOutcome(probe, outcome);
    probe.start(temp.path(), timeoutMs);
    pump([&] { return outcome.calls > 0; });
    check(outcome.calls == 1 && !outcome.ok &&
              outcome.error.contains(expectedError),
          what);
    check(outcome.rateLimits.isEmpty() && outcome.byId.isEmpty(),
          "a failed probe reports no limits");
    check(!probe.running(), "a failed probe leaves no app-server running");
}

void runEmptyHomeTest()
{
    CodexRateLimitsProbe probe;
    Outcome outcome;
    connectOutcome(probe, outcome);
    probe.start(QString());
    check(pump([&] { return outcome.calls > 0; }) && !outcome.ok,
          "probe refuses an empty config folder");
}

void runMissingProgramTest()
{
    QTemporaryDir temp;
    CodexRateLimitsProbe probe;
    probe.setAppServerCommand(temp.filePath(QStringLiteral("no-such-codex")));
    Outcome outcome;
    connectOutcome(probe, outcome);
    probe.start(temp.path(), 5000);
    check(pump([&] { return outcome.calls > 0; }) && !outcome.ok &&
              outcome.error.contains(QStringLiteral("Unable to start")),
          "probe reports a program that cannot start");
}

void runDestroyWhileRunningTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("client.jsonl"));
    auto *probe = new CodexRateLimitsProbe;
    probe->setAppServerCommand(executable, {QStringLiteral("--probe-stub"),
                                            QStringLiteral("silent"), logPath});
    probe->start(temp.path(), 5000);
    check(pump([probe] { return probe->running(); }),
          "silent stub starts");
    QPointer<QProcess> process = probe->findChild<QProcess *>();
    delete probe;
    check(pump([&] { return process.isNull(); }),
          "deleting a running probe stops and frees its app-server");
}

}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() > 1 && arguments.at(1) == QStringLiteral("--probe-stub"))
        return runStub(arguments);

    const QString executable = QCoreApplication::applicationFilePath();
    runSuccessTest(executable);
    runFailureTest(executable, QStringLiteral("rpc-error"), 5000,
                   QStringLiteral("not signed in"),
                   "an account/rateLimits/read error fails the probe");
    runFailureTest(executable, QStringLiteral("silent"), 300,
                   QStringLiteral("in time"),
                   "a silent app-server times out");
    runFailureTest(executable, QStringLiteral("exit"), 5000,
                   QStringLiteral("exited (7)"),
                   "an app-server that exits early fails the probe");
    runEmptyHomeTest();
    runMissingProgramTest();
    runDestroyWhileRunningTest(executable);

    if (failures == 0)
        qInfo("All Codex rate-limit probe tests passed.");
    else
        qCritical("%d Codex rate-limit probe test(s) failed.", failures);
    return failures == 0 ? 0 : 1;
}
