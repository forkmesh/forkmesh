#include "../src/AgentResumeIdentity.h"
#include "../src/CodexAppServerSession.h"
#include "../src/CodexTranscriptStyle.h"
#include "../src/TranscriptCoverage.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <utility>

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

void writeBytes(QFile &output, const QByteArray &bytes)
{
    output.write(bytes);
    output.flush();
}

void writeMessage(QFile &output, const QJsonObject &message,
                  bool fragmented = false)
{
    const QByteArray line =
        QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (!fragmented) {
        writeBytes(output, line);
        return;
    }
    const int split = line.size() / 2;
    writeBytes(output, line.left(split));
    QThread::msleep(25);
    writeBytes(output, line.mid(split));
}

QJsonObject turn(const QString &id, const QString &status,
                 const QJsonArray &items = QJsonArray())
{
    return QJsonObject{{QStringLiteral("id"), id},
                       {QStringLiteral("items"), items},
                       {QStringLiteral("itemsView"), QStringLiteral("full")},
                       {QStringLiteral("status"), status},
                       {QStringLiteral("error"), QJsonValue(QJsonValue::Null)},
                       {QStringLiteral("startedAt"), 1},
                       {QStringLiteral("completedAt"),
                        status == QStringLiteral("inProgress")
                            ? QJsonValue(QJsonValue::Null)
                            : QJsonValue(2)},
                       {QStringLiteral("durationMs"),
                        status == QStringLiteral("inProgress")
                            ? QJsonValue(QJsonValue::Null)
                            : QJsonValue(123)}};
}

void itemNotification(QFile &output, const QString &method,
                      const QJsonObject &item, const QString &turnId)
{
    QJsonObject params{{QStringLiteral("threadId"), QStringLiteral("thread-fresh")},
                       {QStringLiteral("turnId"), turnId},
                       {QStringLiteral("item"), item}};
    params.insert(method == QStringLiteral("item/started")
                      ? QStringLiteral("startedAtMs")
                      : QStringLiteral("completedAtMs"),
                  1000);
    writeMessage(output, QJsonObject{{QStringLiteral("method"), method},
                                     {QStringLiteral("params"), params}});
}

QJsonObject commandItem(const QString &status)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("commandExecution")},
        {QStringLiteral("id"), QStringLiteral("cmd-1")},
        {QStringLiteral("command"), QStringLiteral("printf hello")},
        {QStringLiteral("cwd"), QStringLiteral("/tmp")},
        {QStringLiteral("status"), status},
        {QStringLiteral("commandActions"), QJsonArray()},
        {QStringLiteral("aggregatedOutput"),
         status == QStringLiteral("completed") ? QJsonValue(QStringLiteral("hello"))
                                                : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("exitCode"),
         status == QStringLiteral("completed") ? QJsonValue(0)
                                                : QJsonValue(QJsonValue::Null)}};
}

void sendToolLifecycle(QFile &output, const QJsonObject &started,
                       QJsonObject completed, const QString &turnId)
{
    itemNotification(output, QStringLiteral("item/started"), started, turnId);
    itemNotification(output, QStringLiteral("item/completed"), completed, turnId);
}

void emitFreshTurn(QFile &output)
{
    const QString turnId = QStringLiteral("turn-fresh");
    writeMessage(output,
                 QJsonObject{{QStringLiteral("method"), QStringLiteral("turn/started")},
                             {QStringLiteral("params"),
                              QJsonObject{{QStringLiteral("threadId"),
                                           QStringLiteral("thread-fresh")},
                                          {QStringLiteral("turn"),
                                           turn(turnId, QStringLiteral("inProgress"))}}}});
    writeMessage(output,
                 QJsonObject{{QStringLiteral("method"),
                              QStringLiteral("item/agentMessage/delta")},
                             {QStringLiteral("params"),
                              QJsonObject{{QStringLiteral("threadId"),
                                           QStringLiteral("thread-fresh")},
                                          {QStringLiteral("turnId"), turnId},
                                          {QStringLiteral("itemId"),
                                           QStringLiteral("agent-1")},
                                          {QStringLiteral("delta"),
                                           QStringLiteral("hello ")}}}});
    writeMessage(output,
                 QJsonObject{{QStringLiteral("method"),
                              QStringLiteral("item/reasoning/textDelta")},
                             {QStringLiteral("params"),
                              QJsonObject{{QStringLiteral("threadId"),
                                           QStringLiteral("thread-fresh")},
                                          {QStringLiteral("turnId"), turnId},
                                          {QStringLiteral("itemId"),
                                           QStringLiteral("reason-1")},
                                          {QStringLiteral("contentIndex"), 0},
                                          {QStringLiteral("delta"),
                                           QStringLiteral("thinking")}}}});

    itemNotification(output, QStringLiteral("item/started"),
                     commandItem(QStringLiteral("inProgress")), turnId);
    writeMessage(output,
                 QJsonObject{{QStringLiteral("method"),
                              QStringLiteral("item/commandExecution/outputDelta")},
                             {QStringLiteral("params"),
                              QJsonObject{{QStringLiteral("threadId"),
                                           QStringLiteral("thread-fresh")},
                                          {QStringLiteral("turnId"), turnId},
                                          {QStringLiteral("itemId"),
                                           QStringLiteral("cmd-1")},
                                          {QStringLiteral("delta"),
                                           QStringLiteral("hello")}}}});
    itemNotification(output, QStringLiteral("item/completed"),
                     commandItem(QStringLiteral("completed")), turnId);

    const QJsonArray changes{QJsonObject{{QStringLiteral("path"),
                                         QStringLiteral("src/a.cpp")},
                                        {QStringLiteral("kind"),
                                         QJsonObject{{QStringLiteral("type"),
                                                      QStringLiteral("update")}}}}};
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("fileChange")},
                    {QStringLiteral("id"), QStringLiteral("file-1")},
                    {QStringLiteral("changes"), changes},
                    {QStringLiteral("status"), QStringLiteral("inProgress")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("fileChange")},
                    {QStringLiteral("id"), QStringLiteral("file-1")},
                    {QStringLiteral("changes"), changes},
                    {QStringLiteral("status"), QStringLiteral("completed")}},
        turnId);
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("mcpToolCall")},
                    {QStringLiteral("id"), QStringLiteral("mcp-1")},
                    {QStringLiteral("server"), QStringLiteral("docs")},
                    {QStringLiteral("tool"), QStringLiteral("search")},
                    {QStringLiteral("arguments"), QJsonObject{{QStringLiteral("q"),
                                                               QStringLiteral("Qt")}}},
                    {QStringLiteral("status"), QStringLiteral("inProgress")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("mcpToolCall")},
                    {QStringLiteral("id"), QStringLiteral("mcp-1")},
                    {QStringLiteral("server"), QStringLiteral("docs")},
                    {QStringLiteral("tool"), QStringLiteral("search")},
                    {QStringLiteral("arguments"), QJsonObject()},
                    {QStringLiteral("status"), QStringLiteral("completed")},
                    {QStringLiteral("result"), QJsonObject{{QStringLiteral("ok"), true}}},
                    {QStringLiteral("error"), QJsonValue(QJsonValue::Null)}},
        turnId);
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("dynamicToolCall")},
                    {QStringLiteral("id"), QStringLiteral("dynamic-1")},
                    {QStringLiteral("tool"), QStringLiteral("custom_tool")},
                    {QStringLiteral("arguments"), QJsonObject()},
                    {QStringLiteral("status"), QStringLiteral("inProgress")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("dynamicToolCall")},
                    {QStringLiteral("id"), QStringLiteral("dynamic-1")},
                    {QStringLiteral("tool"), QStringLiteral("custom_tool")},
                    {QStringLiteral("arguments"), QJsonObject()},
                    {QStringLiteral("status"), QStringLiteral("completed")},
                    {QStringLiteral("contentItems"), QJsonArray{QStringLiteral("done")}},
                    {QStringLiteral("success"), true}},
        turnId);
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("collabAgentToolCall")},
                    {QStringLiteral("id"), QStringLiteral("collab-1")},
                    {QStringLiteral("tool"), QStringLiteral("spawn_agent")},
                    {QStringLiteral("prompt"), QStringLiteral("inspect tests")},
                    {QStringLiteral("receiverThreadIds"),
                     QJsonArray{QStringLiteral("child-1")}},
                    {QStringLiteral("status"), QStringLiteral("inProgress")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("collabAgentToolCall")},
                    {QStringLiteral("id"), QStringLiteral("collab-1")},
                    {QStringLiteral("tool"), QStringLiteral("spawn_agent")},
                    {QStringLiteral("receiverThreadIds"),
                     QJsonArray{QStringLiteral("child-1")}},
                    {QStringLiteral("agentsStates"),
                     QJsonObject{{QStringLiteral("child-1"),
                                  QStringLiteral("completed")}}},
                    {QStringLiteral("status"), QStringLiteral("completed")}},
        turnId);
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("webSearch")},
                    {QStringLiteral("id"), QStringLiteral("web-1")},
                    {QStringLiteral("query"), QStringLiteral("Qt JSONL")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("webSearch")},
                    {QStringLiteral("id"), QStringLiteral("web-1")},
                    {QStringLiteral("query"), QStringLiteral("Qt JSONL")},
                    {QStringLiteral("action"),
                     QJsonObject{{QStringLiteral("type"),
                                  QStringLiteral("search")}}}},
        turnId);
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("imageView")},
                    {QStringLiteral("id"), QStringLiteral("image-view-1")},
                    {QStringLiteral("path"), QStringLiteral("/tmp/input.png")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("imageView")},
                    {QStringLiteral("id"), QStringLiteral("image-view-1")},
                    {QStringLiteral("path"), QStringLiteral("/tmp/input.png")}},
        turnId);
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("imageGeneration")},
                    {QStringLiteral("id"), QStringLiteral("image-gen-1")},
                    {QStringLiteral("status"), QStringLiteral("inProgress")},
                    {QStringLiteral("revisedPrompt"), QStringLiteral("a diagram")}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("imageGeneration")},
                    {QStringLiteral("id"), QStringLiteral("image-gen-1")},
                    {QStringLiteral("status"), QStringLiteral("completed")},
                    {QStringLiteral("result"), QStringLiteral("image bytes")},
                    {QStringLiteral("savedPath"), QStringLiteral("/tmp/output.png")}},
        turnId);
    writeMessage(
        output,
        QJsonObject{
            {QStringLiteral("method"), QStringLiteral("turn/plan/updated")},
            {QStringLiteral("params"),
             QJsonObject{
                 {QStringLiteral("threadId"), QStringLiteral("thread-fresh")},
                 {QStringLiteral("turnId"), turnId},
                 {QStringLiteral("explanation"), QStringLiteral("Test the transport")},
                 {QStringLiteral("plan"),
                  QJsonArray{
                      QJsonObject{{QStringLiteral("step"),
                                   QStringLiteral("Inspect")},
                                  {QStringLiteral("status"),
                                   QStringLiteral("completed")}},
                      QJsonObject{{QStringLiteral("step"), QStringLiteral("Fix")},
                                  {QStringLiteral("status"),
                                   QStringLiteral("inProgress")}}}}}}});
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"),
                     QStringLiteral("subAgentActivity")},
                    {QStringLiteral("id"), QStringLiteral("subactivity-1")},
                    {QStringLiteral("kind"), QStringLiteral("spawned")},
                    {QStringLiteral("agentThreadId"), QStringLiteral("child-2")},
                    {QStringLiteral("agentPath"), QStringLiteral("root/child")}},
        QJsonObject{{QStringLiteral("type"),
                     QStringLiteral("subAgentActivity")},
                    {QStringLiteral("id"), QStringLiteral("subactivity-1")},
                    {QStringLiteral("kind"), QStringLiteral("completed")},
                    {QStringLiteral("agentThreadId"), QStringLiteral("child-2")},
                    {QStringLiteral("agentPath"), QStringLiteral("root/child")}},
        turnId);
    sendToolLifecycle(
        output,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("sleep")},
                    {QStringLiteral("id"), QStringLiteral("sleep-1")},
                    {QStringLiteral("durationMs"), 250}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("sleep")},
                    {QStringLiteral("id"), QStringLiteral("sleep-1")},
                    {QStringLiteral("durationMs"), 250}},
        turnId);
    for (const QJsonObject &special : {
             QJsonObject{{QStringLiteral("type"),
                          QStringLiteral("enteredReviewMode")},
                         {QStringLiteral("id"), QStringLiteral("review-in")},
                         {QStringLiteral("review"), QStringLiteral("Review changes")}},
             QJsonObject{{QStringLiteral("type"),
                          QStringLiteral("exitedReviewMode")},
                         {QStringLiteral("id"), QStringLiteral("review-out")},
                         {QStringLiteral("review"), QStringLiteral("Review complete")}},
             QJsonObject{{QStringLiteral("type"),
                          QStringLiteral("contextCompaction")},
                         {QStringLiteral("id"), QStringLiteral("compact-1")}}}) {
        itemNotification(output, QStringLiteral("item/started"), special, turnId);
        itemNotification(output, QStringLiteral("item/completed"), special, turnId);
    }

    itemNotification(
        output, QStringLiteral("item/completed"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("reasoning")},
                    {QStringLiteral("id"), QStringLiteral("reason-1")},
                    {QStringLiteral("summary"),
                     QJsonArray{QStringLiteral("Checked protocol")}},
                    {QStringLiteral("content"), QJsonArray()}},
        turnId);
    itemNotification(
        output, QStringLiteral("item/completed"),
        QJsonObject{{QStringLiteral("type"), QStringLiteral("agentMessage")},
                    {QStringLiteral("id"), QStringLiteral("agent-1")},
                    {QStringLiteral("text"), QStringLiteral("hello world")}},
        turnId);
    writeMessage(
        output,
        QJsonObject{{QStringLiteral("method"),
                     QStringLiteral("thread/tokenUsage/updated")},
                    {QStringLiteral("params"),
                     QJsonObject{
                         {QStringLiteral("threadId"), QStringLiteral("thread-fresh")},
                         {QStringLiteral("turnId"), turnId},
                         {QStringLiteral("tokenUsage"),
                          QJsonObject{{QStringLiteral("total"),
                                       QJsonObject{{QStringLiteral("totalTokens"), 42},
                                                   {QStringLiteral("inputTokens"), 30},
                                                   {QStringLiteral("outputTokens"), 12}}}}}}}});
    writeMessage(
        output,
        QJsonObject{{QStringLiteral("method"),
                     QStringLiteral("account/rateLimits/updated")},
                    {QStringLiteral("params"),
                     QJsonObject{{QStringLiteral("rateLimits"),
                                  QJsonObject{{QStringLiteral("limitId"),
                                               QStringLiteral("codex")},
                                              {QStringLiteral("primary"),
                                               QJsonObject{{QStringLiteral("usedPercent"),
                                                            25}}}}}}}});
    writeMessage(output,
                 QJsonObject{{QStringLiteral("method"), QStringLiteral("warning")},
                             {QStringLiteral("params"),
                              QJsonObject{{QStringLiteral("threadId"),
                                           QStringLiteral("thread-fresh")},
                                          {QStringLiteral("message"),
                                           QStringLiteral("stub warning")}}}});

    writeMessage(
        output,
        QJsonObject{
            {QStringLiteral("method"),
             QStringLiteral("item/commandExecution/requestApproval")},
            {QStringLiteral("id"), QStringLiteral("approval-command")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("threadId"), QStringLiteral("thread-fresh")},
                         {QStringLiteral("turnId"), turnId},
                         {QStringLiteral("itemId"), QStringLiteral("cmd-approval")},
                         {QStringLiteral("reason"), QStringLiteral("Needs network")},
                         {QStringLiteral("command"), QStringLiteral("curl example.com")},
                         {QStringLiteral("availableDecisions"),
                          QJsonArray{QStringLiteral("accept"),
                                     QStringLiteral("acceptForSession"),
                                     QStringLiteral("decline")}}}}});
    writeMessage(
        output,
        QJsonObject{
            {QStringLiteral("method"),
             QStringLiteral("item/fileChange/requestApproval")},
            {QStringLiteral("id"), QStringLiteral("approval-file")},
            {QStringLiteral("params"),
             QJsonObject{{QStringLiteral("threadId"), QStringLiteral("thread-fresh")},
                         {QStringLiteral("turnId"), turnId},
                         {QStringLiteral("itemId"), QStringLiteral("file-approval")},
                         {QStringLiteral("reason"),
                          QStringLiteral("Write outside workspace")}}}});
    const QJsonArray colorOptions{
        QJsonObject{{QStringLiteral("label"), QStringLiteral("Blue")},
                    {QStringLiteral("description"), QStringLiteral("Use blue")}},
        QJsonObject{{QStringLiteral("label"), QStringLiteral("Green")},
                    {QStringLiteral("description"), QStringLiteral("Use green")}}};
    const QJsonObject colorQuestion{
        {QStringLiteral("id"), QStringLiteral("color")},
        {QStringLiteral("header"), QStringLiteral("Color")},
        {QStringLiteral("question"), QStringLiteral("Pick a color")},
        {QStringLiteral("isOther"), false},
        {QStringLiteral("isSecret"), false},
        {QStringLiteral("options"), colorOptions}};
    const QJsonObject sizeQuestion{
        {QStringLiteral("id"), QStringLiteral("size")},
        {QStringLiteral("header"), QStringLiteral("Size")},
        {QStringLiteral("question"), QStringLiteral("Pick a size")},
        {QStringLiteral("isOther"), false},
        {QStringLiteral("isSecret"), false},
        {QStringLiteral("options"),
         QJsonArray{
             QJsonObject{{QStringLiteral("label"), QStringLiteral("Large")},
                         {QStringLiteral("description"),
                          QStringLiteral("Use large")}},
             QJsonObject{{QStringLiteral("label"), QStringLiteral("Small")},
                         {QStringLiteral("description"),
                          QStringLiteral("Use small")}}}}};
    const QJsonObject inputParams{
        {QStringLiteral("threadId"), QStringLiteral("thread-fresh")},
        {QStringLiteral("turnId"), turnId},
        {QStringLiteral("itemId"), QStringLiteral("question-1")},
        {QStringLiteral("questions"), QJsonArray{colorQuestion, sizeQuestion}}};
    writeMessage(output,
                 QJsonObject{{QStringLiteral("method"),
                              QStringLiteral("item/tool/requestUserInput")},
                             {QStringLiteral("id"),
                              QStringLiteral("request-input")},
                             {QStringLiteral("params"), inputParams}});
}

int runStub(const QStringList &arguments)
{
    if (arguments.size() < 4)
        return 2;
    const QString scenario = arguments.at(2);
    QFile input;
    QFile output;
    QFile errors;
    if (!input.open(stdin, QIODevice::ReadOnly) ||
        !output.open(stdout, QIODevice::WriteOnly) ||
        !errors.open(stderr, QIODevice::WriteOnly))
        return 3;
    QFile log(arguments.at(3));
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return 4;

    int serverResponses = 0;
    int turnStarts = 0;
    for (;;) {
        const QByteArray line = input.readLine();
        if (line.isEmpty())
            return 0;
        log.write(line);
        log.flush();
        const QJsonObject message = QJsonDocument::fromJson(line).object();
        const QString method = message.value(QStringLiteral("method")).toString();
        const QJsonValue id = message.value(QStringLiteral("id"));
        if (method == QStringLiteral("initialize")) {
            writeBytes(output, "this is not json\n");
            writeMessage(output,
                         QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("result"),
                                      QJsonObject{{QStringLiteral("userAgent"),
                                                   QStringLiteral("stub")}}}},
                         true);
            writeBytes(errors, "stub diagnostic\n");
        } else if (method == QStringLiteral("account/rateLimits/read")) {
            const QJsonObject weekly{
                {QStringLiteral("usedPercent"), 78},
                {QStringLiteral("resetsAt"), 1'800'000'000},
                {QStringLiteral("windowDurationMins"), 10'080}};
            const QJsonObject general{
                {QStringLiteral("limitId"), QStringLiteral("codex")},
                {QStringLiteral("primary"), weekly},
                {QStringLiteral("secondary"), QJsonValue(QJsonValue::Null)}};
            const QJsonObject spark{
                {QStringLiteral("limitId"),
                 QStringLiteral("gpt-5.3-codex-spark")},
                {QStringLiteral("limitName"),
                 QStringLiteral("GPT-5.3-Codex-Spark")},
                {QStringLiteral("primary"),
                 QJsonObject{{QStringLiteral("usedPercent"), 43},
                             {QStringLiteral("resetsAt"), 1'800'000'100},
                             {QStringLiteral("windowDurationMins"), 300}}},
                {QStringLiteral("secondary"),
                 QJsonObject{{QStringLiteral("usedPercent"), 19},
                             {QStringLiteral("resetsAt"), 1'800'000'200},
                             {QStringLiteral("windowDurationMins"), 10'080}}}};
            writeMessage(
                output,
                QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("result"),
                     QJsonObject{
                         {QStringLiteral("rateLimits"), general},
                         {QStringLiteral("rateLimitsByLimitId"),
                          QJsonObject{
                              {QStringLiteral("codex"), general},
                              {QStringLiteral("gpt-5.3-codex-spark"),
                               spark}}}}}});
        } else if (method == QStringLiteral("model/list")) {
            writeMessage(
                output,
                QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("result"),
                     QJsonObject{
                         {QStringLiteral("data"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("id"), QStringLiteral("gpt-test")},
                              {QStringLiteral("model"), QStringLiteral("gpt-test")},
                              {QStringLiteral("displayName"), QStringLiteral("GPT Test")},
                              {QStringLiteral("hidden"), false}}}},
                         {QStringLiteral("nextCursor"),
                          QJsonValue(QJsonValue::Null)}}}});
        } else if (method == QStringLiteral("thread/start") ||
                   method == QStringLiteral("thread/resume")) {
            if (scenario == QStringLiteral("resume-error") &&
                method == QStringLiteral("thread/resume")) {
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("id"), id},
                                {QStringLiteral("error"),
                                 QJsonObject{{QStringLiteral("code"), -32001},
                                             {QStringLiteral("message"),
                                              QStringLiteral("thread missing")}}}});
                continue;
            }
            const QString threadId = method == QStringLiteral("thread/resume")
                                         ? QStringLiteral("thread-resumed")
                                         : QStringLiteral("thread-fresh");
            writeMessage(
                output,
                QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("result"),
                     QJsonObject{{QStringLiteral("thread"),
                                  QJsonObject{{QStringLiteral("id"), threadId}}},
                                 {QStringLiteral("model"),
                                  (scenario == QStringLiteral("resume") ||
                                   scenario == QStringLiteral("steer-reject"))
                                      ? QStringLiteral("gpt-plan")
                                      : QStringLiteral("gpt-test")}}}});
        } else if (method == QStringLiteral("turn/start")) {
            ++turnStarts;
            const QString turnId =
                scenario == QStringLiteral("steer-reject") && turnStarts > 1
                    ? QStringLiteral("turn-replayed")
                    : scenario == QStringLiteral("options")
                          ? QStringLiteral("turn-options-%1").arg(turnStarts)
                    : (scenario == QStringLiteral("resume") ||
                       scenario == QStringLiteral("steer-reject"))
                                       ? QStringLiteral("turn-resumed")
                                       : QStringLiteral("turn-fresh");
            writeMessage(output,
                         QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("result"),
                                      QJsonObject{{QStringLiteral("turn"),
                                                   turn(turnId,
                                                        QStringLiteral("inProgress"))}}}});
            if (scenario == QStringLiteral("options")) {
                const QJsonObject agent{
                    {QStringLiteral("type"), QStringLiteral("agentMessage")},
                    {QStringLiteral("id"),
                     QStringLiteral("agent-options-%1").arg(turnStarts)},
                    {QStringLiteral("text"), QStringLiteral("options complete")}};
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/started")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("inProgress"))}}}});
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("completed"),
                                                   QJsonArray{agent})}}}});
                if (turnStarts > 2)
                    return 0;
            } else if (scenario == QStringLiteral("mode-switch")) {
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/started")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("inProgress"))}}}});
                if (turnStarts == 1) {
                    // Leave enough time for the client to change modes while
                    // this turn is active. The next request must be a fresh
                    // turn/start, not a turn/steer with the old Plan policy.
                    QThread::msleep(150);
                    writeMessage(
                        output,
                        QJsonObject{{QStringLiteral("method"),
                                     QStringLiteral("turn/completed")},
                                    {QStringLiteral("params"),
                                     QJsonObject{{QStringLiteral("threadId"),
                                                  QStringLiteral("thread-fresh")},
                                                 {QStringLiteral("turn"),
                                                  turn(turnId,
                                                       QStringLiteral("completed"))}}}});
                    continue;
                }
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("completed"))}}}});
                return 0;
            } else if (scenario == QStringLiteral("unshown")) {
                const QJsonObject thread{{QStringLiteral("threadId"),
                                          QStringLiteral("thread-fresh")}};
                const char *statusMessages[] = {
                    R"({"method":"remoteControl/status/changed","params":{"serverName":"a","status":"disabled","environmentId":null}})",
                    R"({"method":"mcpServer/startupStatus/updated","params":{"name":"codex_apps","status":"starting","error":null,"failureReason":null}})",
                    R"({"method":"mcpServer/startupStatus/updated","params":{"name":"codex_apps","status":"failed","error":{"message":"connection refused"},"failureReason":"transport"}})",
                    R"({"method":"thread/settings/updated","params":{"threadId":"thread-fresh","threadSettings":{"model":"gpt-test","effort":"high","approvalPolicy":"never","cwd":"/workspace","sandboxPolicy":{"type":"dangerFullAccess"},"collaborationMode":{"mode":"default","settings":{"developer_instructions":"do not repeat this"}}}}})",
                    R"({"method":"remoteControl/status/changed","params":{}})",
                    R"({"method":"mcpServer/startupStatus/updated","params":{}})",
                    R"({"method":"thread/settings/updated","params":{}})",
                    R"({"method":"turn/moderationMetadata","params":{"metadata":{"generation":{},"prompt":{"omnimod":{"outputs":[{"is_blocked":false}]}},"tool_call":{},"tool_response":{}}}})",
                };
                for (const char *json : statusMessages)
                    writeMessage(output, QJsonDocument::fromJson(QByteArray(json)).object());
                writeMessage(output,
                             QJsonObject{{QStringLiteral("method"),
                                          QStringLiteral("model/rerouted")},
                                         {QStringLiteral("params"), thread}});
                writeMessage(output,
                             QJsonObject{{QStringLiteral("method"),
                                          QStringLiteral("account/updated")},
                                         {QStringLiteral("params"), thread}});
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("item/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("item"),
                                              QJsonObject{{QStringLiteral("type"),
                                                           QStringLiteral("hookPrompt")},
                                                          {QStringLiteral("id"),
                                                           QStringLiteral("hook-1")}}}}}});
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("completed"))}}}});
                return 0;
            } else if (scenario == QStringLiteral("fatal-error")) {
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/started")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("inProgress"))}}}});
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"), QStringLiteral("error")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turnId"), turnId},
                                             {QStringLiteral("willRetry"), false},
                                             {QStringLiteral("error"),
                                              QJsonObject{{QStringLiteral("message"),
                                                           QStringLiteral("fatal turn")}}}}}});
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("failed"))}}}});
                return 0;
            } else if (scenario == QStringLiteral("steer-reject") &&
                       turnStarts > 1) {
                const QJsonObject agent{
                    {QStringLiteral("type"), QStringLiteral("agentMessage")},
                    {QStringLiteral("id"), QStringLiteral("agent-replayed")},
                    {QStringLiteral("text"), QStringLiteral("replayed")}};
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-resumed")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("completed"),
                                                   QJsonArray{agent})}}}});
                return 0;
            } else if (scenario == QStringLiteral("fresh")) {
                emitFreshTurn(output);
            } else if (scenario == QStringLiteral("resume") ||
                       scenario == QStringLiteral("steer-reject") ||
                       scenario == QStringLiteral("interrupt")) {
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/started")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              scenario == QStringLiteral("interrupt")
                                                  ? QStringLiteral("thread-fresh")
                                                  : QStringLiteral("thread-resumed")},
                                             {QStringLiteral("turn"),
                                              turn(turnId,
                                                   QStringLiteral("inProgress"))}}}});
            } else {
                const QString status = scenario == QStringLiteral("interrupted")
                                           ? QStringLiteral("interrupted")
                                           : QStringLiteral("completed");
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                             QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(turnId, status)}}}});
                return 0;
            }
        } else if (method == QStringLiteral("turn/interrupt")) {
            writeMessage(output,
                         QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("result"), QJsonObject()}});
            writeMessage(
                output,
                QJsonObject{{QStringLiteral("method"),
                             QStringLiteral("turn/completed")},
                            {QStringLiteral("params"),
                             QJsonObject{{QStringLiteral("threadId"),
                                          QStringLiteral("thread-fresh")},
                                         {QStringLiteral("turn"),
                                          turn(QStringLiteral("turn-fresh"),
                                               QStringLiteral("interrupted"))}}}});
            return 0;
        } else if (method == QStringLiteral("turn/steer")) {
            if (scenario == QStringLiteral("steer-reject")) {
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("id"), id},
                                {QStringLiteral("error"),
                                 QJsonObject{{QStringLiteral("code"), -32002},
                                             {QStringLiteral("message"),
                                              QStringLiteral("turn not steerable")}}}});
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-resumed")},
                                             {QStringLiteral("turn"),
                                              turn(QStringLiteral("turn-resumed"),
                                                   QStringLiteral("interrupted"))}}}});
                continue;
            }
            writeMessage(output,
                         QJsonObject{{QStringLiteral("id"), id},
                                     {QStringLiteral("result"),
                                      QJsonObject{{QStringLiteral("turnId"),
                                                   QStringLiteral("turn-resumed")}}}});
            const QJsonObject agent{
                {QStringLiteral("type"), QStringLiteral("agentMessage")},
                {QStringLiteral("id"), QStringLiteral("agent-resumed")},
                {QStringLiteral("text"), QStringLiteral("steered")}};
            writeMessage(
                output,
                QJsonObject{{QStringLiteral("method"),
                             QStringLiteral("item/completed")},
                            {QStringLiteral("params"),
                             QJsonObject{{QStringLiteral("threadId"),
                                          QStringLiteral("thread-resumed")},
                                         {QStringLiteral("turnId"),
                                          QStringLiteral("turn-resumed")},
                                         {QStringLiteral("item"), agent},
                                         {QStringLiteral("completedAtMs"), 2}}}});
            writeMessage(
                output,
                QJsonObject{{QStringLiteral("method"),
                             QStringLiteral("turn/completed")},
                            {QStringLiteral("params"),
                             QJsonObject{{QStringLiteral("threadId"),
                                          QStringLiteral("thread-resumed")},
                                         {QStringLiteral("turn"),
                                          turn(QStringLiteral("turn-resumed"),
                                               QStringLiteral("completed"),
                                               QJsonArray{agent})}}}});
            return 0;
        } else if (method.isEmpty() && message.contains(QStringLiteral("id"))) {
            const QString responseId = id.toString();
            if (responseId == QStringLiteral("approval-command") ||
                responseId == QStringLiteral("approval-file") ||
                responseId == QStringLiteral("request-input"))
                ++serverResponses;
            if (serverResponses == 3) {
                const QJsonObject agent{
                    {QStringLiteral("type"), QStringLiteral("agentMessage")},
                    {QStringLiteral("id"), QStringLiteral("agent-1")},
                    {QStringLiteral("text"), QStringLiteral("hello world")}};
                writeMessage(
                    output,
                    QJsonObject{{QStringLiteral("method"),
                                 QStringLiteral("turn/completed")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("threadId"),
                                              QStringLiteral("thread-fresh")},
                                             {QStringLiteral("turn"),
                                              turn(QStringLiteral("turn-fresh"),
                                                   QStringLiteral("completed"),
                                                   QJsonArray{agent})}}}});
                return 0;
            }
        }
    }
}

QList<QJsonObject> readMessages(const QString &path)
{
    QList<QJsonObject> messages;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return messages;
    while (!file.atEnd()) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readLine());
        if (document.isObject())
            messages.append(document.object());
    }
    return messages;
}

int methodIndex(const QList<QJsonObject> &messages, const QString &method)
{
    for (int i = 0; i < messages.size(); ++i) {
        if (messages.at(i).value(QStringLiteral("method")).toString() == method)
            return i;
    }
    return -1;
}

QJsonObject firstMethod(const QList<QJsonObject> &messages,
                        const QString &method)
{
    const int index = methodIndex(messages, method);
    return index >= 0 ? messages.at(index) : QJsonObject();
}

QJsonObject responseWithId(const QList<QJsonObject> &messages, const QString &id)
{
    for (const QJsonObject &message : messages) {
        if (message.value(QStringLiteral("id")).toString() == id &&
            message.value(QStringLiteral("method")).toString().isEmpty())
            return message;
    }
    return QJsonObject();
}

int eventCount(const QList<QJsonObject> &events, const QString &type)
{
    int count = 0;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() == type)
            ++count;
    }
    return count;
}

QHash<QString, QString> questionTokens(const QList<QJsonObject> &events)
{
    QHash<QString, QString> tokens;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() !=
            QStringLiteral("assistant"))
            continue;
        const QJsonArray blocks = event.value(QStringLiteral("message"))
                                      .toObject()
                                      .value(QStringLiteral("content"))
                                      .toArray();
        for (const QJsonValue &value : blocks) {
            const QJsonObject block = value.toObject();
            if (block.value(QStringLiteral("name")).toString() !=
                QStringLiteral("AskUserQuestion"))
                continue;
            const QJsonArray questions = block.value(QStringLiteral("input"))
                                             .toObject()
                                             .value(QStringLiteral("questions"))
                                             .toArray();
            if (!questions.isEmpty())
                tokens.insert(questions.first()
                                  .toObject()
                                  .value(QStringLiteral("header"))
                                  .toString(),
                              block.value(QStringLiteral("id")).toString());
        }
    }
    return tokens;
}

bool rawContainsMethod(const QStringList &lines, const QString &method)
{
    for (const QString &line : lines) {
        if (QJsonDocument::fromJson(line.toUtf8())
                .object()
                .value(QStringLiteral("method"))
                .toString() == method)
            return true;
    }
    return false;
}

void runDefaultLaunchEnvironmentTest(const QString &executable)
{
#ifndef Q_OS_WIN
    QTemporaryDir temp;
    check(temp.isValid(), "default launch temp dir created");
    const QString logPath = temp.filePath(QStringLiteral("client.jsonl"));
    QString quotedExecutable = executable;
    quotedExecutable.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    QString quotedLog = logPath;
    quotedLog.replace(QLatin1Char('\''), QStringLiteral("'\"'\"'"));
    QFile launcher(temp.filePath(QStringLiteral("codex")));
    check(launcher.open(QIODevice::WriteOnly), "default launch CLI fixture opens");
    launcher.write(QStringLiteral("#!/bin/sh\nexec '%1' --codex-stub fresh '%2'\n")
                       .arg(quotedExecutable, quotedLog).toUtf8());
    launcher.close();
    launcher.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile profile(temp.filePath(QStringLiteral(".bash_profile")));
    check(profile.open(QIODevice::WriteOnly), "default launch profile fixture opens");
    profile.write("PATH=/missing-cli\nexport PATH\n");
    profile.close();
    CodexAppServerSession session;
    session.start(temp.path(),
                  {QStringLiteral("HOME=") + temp.path(),
                   QStringLiteral("PATH=") + temp.path() + QStringLiteral(":/usr/bin:/bin"),
                   QStringLiteral("BASH_ENV"),
                   QStringLiteral("CODEX_STUB_ENV=present")},
                  QStringLiteral("Continue the existing work."));
    check(pump([&] { return !session.threadId().isEmpty(); }),
          "default Codex launch preserves its prepared PATH despite login profile overrides");
    QProcess *process = session.findChild<QProcess *>();
    check(process && process->arguments().contains(QStringLiteral("-c")) &&
              !process->arguments().contains(QStringLiteral("-lc")),
          "Codex app-server uses a non-login shell");
    const QString resumeId = session.threadId();
    session.stop();
    session.start(temp.path(),
                  {QStringLiteral("HOME=") + temp.path(),
                   QStringLiteral("PATH=") + temp.path() + QStringLiteral(":/usr/bin:/bin"),
                   QStringLiteral("BASH_ENV"),
                   QStringLiteral("CODEX_STUB_ENV=present")},
                  QStringLiteral("Continue the existing work."), resumeId);
    check(pump([&] { return session.threadId() == QStringLiteral("thread-resumed"); }),
          "default Codex launch can continue an existing thread");
    check(firstMethod(readMessages(logPath), QStringLiteral("thread/resume"))
              .value(QStringLiteral("params")).toObject()
              .value(QStringLiteral("threadId")).toString() == resumeId,
          "continuation retains the existing Codex resume identifier");
    session.stop();
#endif
}

void runFinalTeardownProcessOwnershipTest(const QString &executable)
{
    QTemporaryDir temp;
    check(temp.isValid(), "final teardown temp dir created");
    const QString logPath = temp.filePath(QStringLiteral("client.jsonl"));
    auto *session = new CodexAppServerSession;
    session->setAppServerCommand(executable,
                                 {QStringLiteral("--codex-stub"),
                                  QStringLiteral("teardown"), logPath});
    session->start(temp.path(), {}, QStringLiteral("Keep running."));
    check(pump([session] { return session->running(); }),
          "final teardown fixture starts a live app-server process");

    QPointer<QProcess> process = session->findChild<QProcess *>();
    check(!process.isNull(), "live app-server process is owned by its session");
    delete session;
    check(process.isNull(),
          "final Codex transport teardown reaps its process synchronously");
}

void runFreshProtocolTest(const QString &executable)
{
    QTemporaryDir temp;
    check(temp.isValid(), "fresh protocol temp dir created");
    const QString logPath = temp.filePath(QStringLiteral("client.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("fresh"), logPath});
    QList<QJsonObject> events;
    QStringList rawLines;
    QString stderrOutput;
    QJsonArray models;
    bool started = false;
    bool finished = false;
    int exitCode = -99;
    QObject::connect(&session, &CodexAppServerSession::started,
                     [&] { started = true; });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int code) {
                         exitCode = code;
                         finished = true;
                     });
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::rawLine,
                     [&](const QString &line) { rawLines.append(line); });
    QObject::connect(&session, &CodexAppServerSession::stderrText,
                     [&](const QString &text) { stderrOutput += text; });
    QObject::connect(&session, &CodexAppServerSession::modelsListed,
                     [&](const QJsonArray &listed) { models = listed; });

    const QString initialTask =
        QStringLiteral("Agent launch identity: provider display name: Codex; "
                       "resolved model: gpt-test; permission mode: Auto mode; "
                       "reasoning speed: high.\n\n"
                       "describe this\nAttached image: /tmp/example.png");
    AgentRuntimeLaunch launch;
    launch.sessionId = QStringLiteral("host-session-fresh");
    launch.workingDirectory = temp.path();
    launch.environment = {QStringLiteral("CODEX_STUB_ENV=present")};
    launch.initialPrompt = initialTask;
    launch.model = QStringLiteral("gpt-test");
    launch.permissionMode = QStringLiteral("Edit automatically");
    launch.effort = QStringLiteral("high");
    launch.options.insert(QStringLiteral("externalCallGate"), true);
    session.startRuntime(launch);
    check(pump([&] { return started; }), "app-server process starts");
    check(pump([&] {
              return questionTokens(events).size() == 1 &&
                     eventCount(events,
                                AgentRuntimeEventTypes::PermissionRequested) == 0;
          }),
          "full-permission Codex answers approval requests without prompting");

    const QHash<QString, QString> tokens = questionTokens(events);
    auto answer = [&session](const QString &requestId, const QString &text) {
        return session.sendCommand(
            {1,
             AgentRuntimeCommandTypes::RequestRespond,
             requestId,
             0,
             {{QStringLiteral("answer"), text}}});
    };
    const bool inputAnswered =
        answer(tokens.value(QStringLiteral("Color")),
               QStringLiteral("Pick a color %1 Blue\nPick a size %1 Large")
                   .arg(QChar(0x2192)));
    check(inputAnswered &&
              !answer(QStringLiteral("stale-request"),
                      QStringLiteral("Allow once")),
          "request answers report whether they reached a live pending request");
    check(pump([&] { return finished; }), "fresh stub completes after responses");
    check(exitCode == 0, "fresh stub exits successfully");
    check(!session.running(), "session reports stopped after child exit");
    check(session.threadId() == QStringLiteral("thread-fresh"),
          "thread id is retained");

    const QList<QJsonObject> sent = readMessages(logPath);
    const int initIndex = methodIndex(sent, QStringLiteral("initialize"));
    const int initializedIndex = methodIndex(sent, QStringLiteral("initialized"));
    const int rateLimitsIndex =
        methodIndex(sent, QStringLiteral("account/rateLimits/read"));
    const int modelsIndex = methodIndex(sent, QStringLiteral("model/list"));
    const int threadIndex = methodIndex(sent, QStringLiteral("thread/start"));
    const int turnIndex = methodIndex(sent, QStringLiteral("turn/start"));
    check(initIndex == 0, "initialize is the first protocol message");
    check(initializedIndex > initIndex && rateLimitsIndex > initializedIndex &&
              modelsIndex > initializedIndex &&
              threadIndex > modelsIndex && turnIndex > threadIndex,
          "initialized, usage read, model list, thread start, and turn start are ordered");
    const QJsonObject initialize = sent.at(initIndex);
    check(initialize.value(QStringLiteral("params"))
              .toObject()
              .value(QStringLiteral("capabilities"))
              .toObject()
              .value(QStringLiteral("experimentalApi"))
              .toBool(),
          "initialize enables experimentalApi");
    check(!initialize.contains(QStringLiteral("jsonrpc")),
          "Codex messages omit the jsonrpc wrapper");
    check(rateLimitsIndex >= 0 &&
              sent.at(rateLimitsIndex)
                  .value(QStringLiteral("params"))
                  .isNull(),
          "account/rateLimits/read uses the protocol's null parameters");

    const QJsonObject threadParams = sent.at(threadIndex)
                                         .value(QStringLiteral("params"))
                                         .toObject();
    check(threadParams.value(QStringLiteral("approvalPolicy")).toString() ==
                  QStringLiteral("never") &&
              threadParams.value(QStringLiteral("sandbox")).toString() ==
                  QStringLiteral("danger-full-access"),
          "Auto mode maps to never/danger-full-access");
    const QJsonObject turnParams = sent.at(turnIndex)
                                       .value(QStringLiteral("params"))
                                       .toObject();
    const QJsonArray input = turnParams.value(QStringLiteral("input")).toArray();
    const QString expectedText =
        initialTask.left(initialTask.indexOf(QStringLiteral("\nAttached image:")));
    check(input.size() == 2 &&
              input.first().toObject().value(QStringLiteral("text")).toString() ==
                  expectedText &&
              input.last().toObject().value(QStringLiteral("type")).toString() ==
                  QStringLiteral("localImage") &&
              input.last().toObject().value(QStringLiteral("path")).toString() ==
                  QStringLiteral("/tmp/example.png"),
          "identity instruction and attached image reach turn/start");
    check(turnParams.value(QStringLiteral("model")).toString() ==
                  QStringLiteral("gpt-test") &&
              turnParams.value(QStringLiteral("effort")).toString() ==
                  QStringLiteral("high"),
          "model and reasoning effort reach turn/start");

    check(models.size() == 1 &&
              models.first().toObject().value(QStringLiteral("id")).toString() ==
                  QStringLiteral("gpt-test"),
          "model/list result is emitted");
    check(rawLines.contains(QStringLiteral("this is not json")),
          "malformed lines are preserved as raw output");
    check(rawLines.size() > 10, "fragmented JSON is reassembled into protocol lines");
    check(stderrOutput.contains(QStringLiteral("stub diagnostic")),
          "stderr is forwarded");

    check(eventCount(events, QStringLiteral("system")) == 1,
          "thread readiness emits one system/init event");
    check(eventCount(events, QStringLiteral("_codex_agent_delta")) == 1 &&
              eventCount(events, QStringLiteral("_codex_agent_complete")) == 1,
          "agent delta and completion events are normalized");
    check(eventCount(events, QStringLiteral("stream_event")) == 1 &&
              eventCount(events, QStringLiteral("_codex_reasoning_complete")) == 1,
          "reasoning stream and completion events are normalized");
    check(eventCount(events, QStringLiteral("_codex_tool_delta")) >= 1,
          "tool output deltas are normalized");
    QSet<QString> toolIds;
    QSet<QString> toolResultIds;
    for (const QJsonObject &event : events) {
        for (const QJsonValue &value : event.value(QStringLiteral("message"))
                                           .toObject()
                                           .value(QStringLiteral("content"))
                                           .toArray()) {
            const QJsonObject block = value.toObject();
            if (block.value(QStringLiteral("type")).toString() ==
                QStringLiteral("tool_use"))
                toolIds.insert(block.value(QStringLiteral("id")).toString());
            else if (block.value(QStringLiteral("type")).toString() ==
                     QStringLiteral("tool_result"))
                toolResultIds.insert(
                    block.value(QStringLiteral("tool_use_id")).toString());
        }
    }
    const QSet<QString> expectedToolIds{
        QStringLiteral("cmd-1"),        QStringLiteral("file-1"),
        QStringLiteral("mcp-1"),        QStringLiteral("dynamic-1"),
        QStringLiteral("collab-1"),     QStringLiteral("web-1"),
        QStringLiteral("image-view-1"), QStringLiteral("image-gen-1"),
        QStringLiteral("plan-turn-fresh"),
        QStringLiteral("subactivity-1"), QStringLiteral("sleep-1")};
    bool hasEveryToolUse = true;
    for (const QString &id : expectedToolIds)
        hasEveryToolUse = hasEveryToolUse && toolIds.contains(id);
    check(eventCount(events, QStringLiteral("assistant")) == 12 &&
              eventCount(events, QStringLiteral("user")) == 11 &&
              toolIds.size() == expectedToolIds.size() + 1 && hasEveryToolUse &&
              toolResultIds == expectedToolIds,
          "every Codex tool family emits one complete Claude-shaped lifecycle");
    check(eventCount(events, QStringLiteral("_codex_usage")) == 1,
          "token usage is normalized");
    QJsonObject mergedRateLimits;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() ==
            QStringLiteral("_codex_rate_limits"))
            mergedRateLimits = event;
    }
    const QJsonObject mergedBuckets =
        mergedRateLimits.value(QStringLiteral("rateLimitsByLimitId")).toObject();
    const QJsonObject mergedGeneral =
        mergedBuckets.value(QStringLiteral("codex")).toObject();
    check(eventCount(events, QStringLiteral("_codex_rate_limits")) == 2 &&
              mergedBuckets.contains(
                  QStringLiteral("gpt-5.3-codex-spark")) &&
              mergedGeneral.value(QStringLiteral("primary"))
                      .toObject()
                      .value(QStringLiteral("usedPercent"))
                      .toInt() == 25 &&
              mergedGeneral.value(QStringLiteral("primary"))
                      .toObject()
                      .value(QStringLiteral("windowDurationMins"))
                      .toInt() == 10'080,
          "full multi-bucket limits are read and sparse updates are merged");
    check(eventCount(events, QStringLiteral("_local_notice")) == 4,
          "warnings, review mode, and compaction become deduplicated notices");
    check(eventCount(events, QStringLiteral("result")) == 1,
          "turn completion emits a result event");

    check(responseWithId(sent, QStringLiteral("approval-command"))
                  .value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("decision"))
                  .toString() == QStringLiteral("acceptForSession"),
          "full-permission Codex accepts command approval for the session");
    check(responseWithId(sent, QStringLiteral("approval-file"))
                  .value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("decision"))
                  .toString() == QStringLiteral("acceptForSession"),
          "full-permission Codex accepts file approval for the session");
    check(responseWithId(sent, QStringLiteral("request-input"))
                  .value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("answers"))
                  .toObject()
                  .value(QStringLiteral("color"))
                  .toObject()
                  .value(QStringLiteral("answers"))
                  .toArray()
                  .first()
                  .toString() == QStringLiteral("Blue"),
          "first requestUserInput answer maps by question id");
    check(responseWithId(sent, QStringLiteral("request-input"))
                  .value(QStringLiteral("result"))
                  .toObject()
                  .value(QStringLiteral("answers"))
                  .toObject()
                  .value(QStringLiteral("size"))
                  .toObject()
                  .value(QStringLiteral("answers"))
                  .toArray()
                  .first()
                  .toString() == QStringLiteral("Large"),
          "second requestUserInput answer maps from displayed question text");
}

void runResumeAndSteerTest(const QString &executable)
{
    QTemporaryDir temp;
    check(temp.isValid(), "resume protocol temp dir created");
    const QString logPath = temp.filePath(QStringLiteral("resume.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("resume"), logPath});
    QList<QJsonObject> events;
    QStringList rawLines;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::rawLine,
                     [&](const QString &line) { rawLines.append(line); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("continue"),
                  QStringLiteral("old-thread"), QStringLiteral("gpt-plan"),
                  QStringLiteral("Plan mode"), QStringLiteral("xhigh"));
    check(pump([&] {
              return rawContainsMethod(rawLines, QStringLiteral("turn/started"));
          }),
          "resumed turn reaches active state");
    session.sendUserText(QStringLiteral("steer now"));
    check(pump([&] { return finished; }), "steered resume session completes");

    const QList<QJsonObject> sent = readMessages(logPath);
    const QJsonObject resume = firstMethod(sent, QStringLiteral("thread/resume"));
    const QJsonObject resumeParams = resume.value(QStringLiteral("params")).toObject();
    check(resumeParams.value(QStringLiteral("threadId")).toString() ==
                  QStringLiteral("old-thread") &&
              resumeParams.value(QStringLiteral("approvalPolicy")).toString() ==
                  QStringLiteral("never") &&
              resumeParams.value(QStringLiteral("sandbox")).toString() ==
                  QStringLiteral("danger-full-access"),
          "Plan mode resumes with never/danger-full-access policy");
    const QJsonObject startParams =
        firstMethod(sent, QStringLiteral("turn/start"))
            .value(QStringLiteral("params"))
            .toObject();
    const QJsonObject collaboration =
        startParams.value(QStringLiteral("collaborationMode")).toObject();
    check(collaboration.value(QStringLiteral("mode")).toString() ==
                  QStringLiteral("plan") &&
              collaboration.value(QStringLiteral("settings"))
                      .toObject()
                      .value(QStringLiteral("model"))
                      .toString() == QStringLiteral("gpt-plan") &&
              collaboration.value(QStringLiteral("settings"))
                      .toObject()
                      .value(QStringLiteral("reasoning_effort"))
                      .toString() == QStringLiteral("xhigh"),
          "Plan turn carries Codex collaborationMode settings");
    const QJsonObject steerParams =
        firstMethod(sent, QStringLiteral("turn/steer"))
            .value(QStringLiteral("params"))
            .toObject();
    check(steerParams.value(QStringLiteral("threadId")).toString() ==
                  QStringLiteral("thread-resumed") &&
              steerParams.value(QStringLiteral("expectedTurnId")).toString() ==
                  QStringLiteral("turn-resumed") &&
              steerParams.value(QStringLiteral("input"))
                      .toArray()
                      .first()
                      .toObject()
                      .value(QStringLiteral("text"))
                      .toString() == QStringLiteral("steer now"),
          "active user text uses turn/steer with expected turn id");
    check(session.threadId() == QStringLiteral("thread-resumed") &&
              eventCount(events, QStringLiteral("result")) == 1,
          "resumed thread id and result are retained");
}

void runModeMappingTest(const QString &executable, const QString &mode,
                        const QString &approval, const QString &sandbox)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("mode.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("mode"), logPath});
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("test mode"), QString(),
                  QStringLiteral("gpt-test"), mode, QString());
    check(pump([&] { return finished; }), "mode mapping stub completes");
    const QJsonObject params =
        firstMethod(readMessages(logPath), QStringLiteral("thread/start"))
            .value(QStringLiteral("params"))
            .toObject();
    check(params.value(QStringLiteral("approvalPolicy")).toString() == approval &&
              params.value(QStringLiteral("sandbox")).toString() == sandbox,
          qPrintable(QStringLiteral("%1 maps to %2/%3")
                         .arg(mode, approval, sandbox)));
}

void runFullAccessSandboxPolicyTest(const QString &executable)
{
    for (const QString &mode : {QStringLiteral("Auto"), QStringLiteral("Plan")}) {
        QTemporaryDir temp;
        const QString logPath = temp.filePath(QStringLiteral("sandbox.jsonl"));
        CodexAppServerSession session;
        session.setAppServerCommand(executable,
                                    {QStringLiteral("--codex-stub"),
                                     QStringLiteral("mode"), logPath});
        bool finished = false;
        QObject::connect(&session, &CodexAppServerSession::finished,
                         [&](int) { finished = true; });
        session.start(temp.path(), QStringList(), QStringLiteral("serve a port"), QString(),
                      QStringLiteral("gpt-test"), mode, QString());
        check(pump([&] { return finished; }), "full-access sandbox stub completes");
        const QJsonObject policy =
            firstMethod(readMessages(logPath), QStringLiteral("turn/start"))
                .value(QStringLiteral("params"))
                .toObject()
                .value(QStringLiteral("sandboxPolicy"))
                .toObject();
        check(policy.value(QStringLiteral("type")).toString() ==
                  QStringLiteral("dangerFullAccess"),
              qPrintable(QStringLiteral("%1 mode turns run with full access").arg(mode)));
    }
}

void runInterruptedTurnTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("interrupted.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("interrupted"), logPath});
    QList<QJsonObject> events;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("interrupt me"));
    check(pump([&] { return finished; }), "interrupted turn stub completes");
    QJsonObject result;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() ==
            QStringLiteral("result"))
            result = event;
    }
    check(result.value(QStringLiteral("is_error")).toBool() &&
              result.value(QStringLiteral("subtype")).toString() ==
                  QStringLiteral("interrupted"),
          "interrupted turns cannot be mistaken for successful completion");
}

void runRejectedSteerTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("steer-reject.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("steer-reject"), logPath});
    QStringList rawLines;
    QList<QJsonObject> events;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::rawLine,
                     [&](const QString &line) { rawLines.append(line); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    session.start(temp.path(), QStringList(), QStringLiteral("initial"),
                  QStringLiteral("old-thread"));
    check(pump([&] {
              return rawContainsMethod(rawLines, QStringLiteral("turn/started"));
          }),
          "rejected-steer turn reaches active state");
    session.sendUserText(QStringLiteral("do not lose me"));
    check(pump([&] { return finished; }),
          "rejected steer is replayed and completes");
    const QList<QJsonObject> sent = readMessages(logPath);
    QList<QJsonObject> turnStarts;
    int steerRequests = 0;
    for (const QJsonObject &message : sent) {
        if (message.value(QStringLiteral("method")).toString() ==
            QStringLiteral("turn/start"))
            turnStarts.append(message);
        if (message.value(QStringLiteral("method")).toString() ==
            QStringLiteral("turn/steer"))
            ++steerRequests;
    }
    check(steerRequests == 1 && turnStarts.size() == 2 &&
              turnStarts.last()
                      .value(QStringLiteral("params"))
                      .toObject()
                      .value(QStringLiteral("input"))
                      .toArray()
                      .first()
                      .toObject()
                      .value(QStringLiteral("text"))
                      .toString() == QStringLiteral("do not lose me"),
          "rejected steer waits for completion, then replays as a fresh turn");
    check(eventCount(events, QStringLiteral("result")) == 1,
          "steer fallback emits only the replayed turn's terminal result");
}

void runResumeFailureTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("resume-error.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("resume-error"), logPath});
    QList<QJsonObject> events;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("resume"),
                  QStringLiteral("missing-thread"), QString(), QString(),
                  QString(), 0, QStringLiteral("recover the saved work"));
    check(pump([&] { return finished; }),
          "missing Codex thread falls back to a fresh transport thread");
    const QList<QJsonObject> sent = readMessages(logPath);
    int resumeRequests = 0;
    int freshThreadRequests = 0;
    QString fallbackTurnText;
    for (const QJsonObject &message : sent) {
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("thread/resume"))
            ++resumeRequests;
        else if (method == QStringLiteral("thread/start"))
            ++freshThreadRequests;
        else if (method == QStringLiteral("turn/start")) {
            const QJsonArray input = message.value(QStringLiteral("params"))
                                         .toObject()
                                         .value(QStringLiteral("input"))
                                         .toArray();
            if (!input.isEmpty())
                fallbackTurnText = input.first().toObject()
                                       .value(QStringLiteral("text"))
                                       .toString();
        }
    }
    QJsonObject terminalResult;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() ==
                QStringLiteral("result") &&
            event.value(QStringLiteral("is_error")).toBool())
            terminalResult = event;
    }
    check(resumeRequests == 1 && freshThreadRequests == 1 &&
              fallbackTurnText == QStringLiteral("recover the saved work"),
          "missing Codex thread starts one fresh thread with recovery context");
    check(terminalResult.isEmpty(),
          "missing Codex thread does not leave the session terminally failed");
}

void runInterruptTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("interrupt.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("interrupt"), logPath});
    QStringList rawLines;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::rawLine,
                     [&](const QString &line) { rawLines.append(line); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("long turn"));
    check(pump([&] {
              return rawContainsMethod(rawLines, QStringLiteral("turn/started"));
          }),
          "interrupt test reaches an active turn");
    session.interrupt();
    check(pump([&] { return finished; }), "interrupt request completes the stub");
    const QJsonObject params =
        firstMethod(readMessages(logPath), QStringLiteral("turn/interrupt"))
            .value(QStringLiteral("params"))
            .toObject();
    check(params.value(QStringLiteral("threadId")).toString() ==
                  QStringLiteral("thread-fresh") &&
              params.value(QStringLiteral("turnId")).toString() ==
                  QStringLiteral("turn-fresh"),
          "interrupt sends the exact active thread and turn ids");
}

void runTurnOptionsTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("options.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("options"), logPath});
    QList<QJsonObject> events;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("first"), QString(),
                  QStringLiteral("gpt-old"), QStringLiteral("Auto mode"),
                  QStringLiteral("low"));
    check(pump([&] {
              return eventCount(events, QStringLiteral("result")) == 1 &&
                     session.running();
          }),
          "turn-options stub completes its initial turn");
    session.setTurnOptions(QStringLiteral("gpt-new"), QStringLiteral("Plan mode"),
                           QStringLiteral("xhigh"));
    session.sendUserText(QStringLiteral("second"));
    check(pump([&] {
              return eventCount(events, QStringLiteral("result")) == 2 &&
                     session.running();
          }),
          "updated Plan turn options complete");
    session.setTurnOptions(QStringLiteral("gpt-final"),
                           QStringLiteral("Auto mode"),
                           QStringLiteral("medium"));
    session.sendUserText(QStringLiteral("third"));
    check(pump([&] { return finished; }), "switching Plan to Auto completes");

    QList<QJsonObject> starts;
    for (const QJsonObject &message : readMessages(logPath)) {
        if (message.value(QStringLiteral("method")).toString() ==
            QStringLiteral("turn/start"))
            starts.append(message);
    }
    const QJsonObject params = starts.size() > 1
                                   ? starts.at(1)
                                         .value(QStringLiteral("params"))
                                         .toObject()
                                   : QJsonObject();
    const QJsonObject collaboration =
        params.value(QStringLiteral("collaborationMode")).toObject();
    check(starts.size() == 3 &&
              params.value(QStringLiteral("model")).toString() ==
                  QStringLiteral("gpt-new") &&
              params.value(QStringLiteral("effort")).toString() ==
                  QStringLiteral("xhigh") &&
              collaboration.value(QStringLiteral("mode")).toString() ==
                  QStringLiteral("plan") &&
              collaboration.value(QStringLiteral("settings"))
                      .toObject()
                      .value(QStringLiteral("model"))
                      .toString() == QStringLiteral("gpt-new") &&
              params.value(QStringLiteral("approvalPolicy")).toString() ==
                  QStringLiteral("never") &&
              params.value(QStringLiteral("sandboxPolicy"))
                      .toObject()
                      .value(QStringLiteral("type"))
                      .toString() == QStringLiteral("dangerFullAccess"),
          "setTurnOptions keeps full Codex permissions in plan collaboration");
    const QJsonObject finalParams =
        starts.size() > 2
            ? starts.at(2).value(QStringLiteral("params")).toObject()
            : QJsonObject();
    check(finalParams.value(QStringLiteral("model")).toString() ==
                  QStringLiteral("gpt-final") &&
              finalParams.value(QStringLiteral("effort")).toString() ==
                  QStringLiteral("medium") &&
              finalParams.value(QStringLiteral("collaborationMode"))
                      .toObject()
                      .value(QStringLiteral("mode"))
                      .toString() == QStringLiteral("default") &&
              finalParams.value(QStringLiteral("collaborationMode"))
                      .toObject()
                      .value(QStringLiteral("settings"))
                      .toObject()
                      .value(QStringLiteral("model"))
                      .toString() == QStringLiteral("gpt-final") &&
              finalParams.value(QStringLiteral("approvalPolicy")).toString() ==
                  QStringLiteral("never") &&
              finalParams.value(QStringLiteral("sandboxPolicy"))
                      .toObject()
                      .value(QStringLiteral("type"))
                      .toString() == QStringLiteral("dangerFullAccess"),
          "switching Plan to Auto explicitly restores default collaboration, "
          "unattended approval, and keeps full access");
}

void runActiveTurnModeSwitchTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("mode-switch.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("mode-switch"), logPath});
    QStringList rawLines;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::rawLine,
                     [&](const QString &line) { rawLines.append(line); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("plan first"),
                  QString(), QStringLiteral("gpt-plan"),
                  QStringLiteral("Plan"), QStringLiteral("high"));
    check(pump([&] {
              return rawContainsMethod(rawLines, QStringLiteral("turn/started"));
          }),
          "mode-switch stub reaches an active Plan turn");

    session.setTurnOptions(QStringLiteral("gpt-auto"), QStringLiteral("Auto"),
                           QStringLiteral("medium"));
    session.sendUserText(QStringLiteral("make the changes"));
    check(pump([&] { return finished; }),
          "a changed-mode follow-up starts after the active turn completes");

    QList<QJsonObject> turnStarts;
    int steerRequests = 0;
    for (const QJsonObject &message : readMessages(logPath)) {
        const QString method = message.value(QStringLiteral("method")).toString();
        if (method == QStringLiteral("turn/start"))
            turnStarts.append(message);
        else if (method == QStringLiteral("turn/steer"))
            ++steerRequests;
    }
    const QJsonObject next =
        turnStarts.size() > 1 ? turnStarts.at(1).value(QStringLiteral("params")).toObject()
                              : QJsonObject();
    check(turnStarts.size() == 2 && steerRequests == 0 &&
              next.value(QStringLiteral("input"))
                      .toArray()
                      .first()
                      .toObject()
                      .value(QStringLiteral("text"))
                      .toString() == QStringLiteral("make the changes") &&
              next.value(QStringLiteral("approvalPolicy")).toString() ==
                  QStringLiteral("never") &&
              next.value(QStringLiteral("sandboxPolicy"))
                      .toObject()
                      .value(QStringLiteral("type")) ==
                  QStringLiteral("dangerFullAccess") &&
              next.value(QStringLiteral("collaborationMode"))
                      .toObject()
                      .value(QStringLiteral("mode")) == QStringLiteral("default"),
          "an active Plan -> Auto change uses a fresh unrestricted Codex turn");
}

// A follow-up whose caller has no model to name — the desktop refuses to hand a
// live ChatGPT thread the Claude model its composer is sitting on, which is what
// silently ate "add" from an Opus composer to a running ChatGPT agent — must
// still start its turn, on the model the thread already runs. An empty model is
// "no opinion", never "clear the model".
void runTurnOptionsKeepModelTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("keep-model.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("options"), logPath});
    QList<QJsonObject> events;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("first"), QString(),
                  QStringLiteral("gpt-old"), QStringLiteral("Auto mode"),
                  QStringLiteral("low"));
    check(pump([&] {
              return eventCount(events, QStringLiteral("result")) == 1 &&
                     session.running();
          }),
          "keep-model stub completes its initial turn");
    session.setTurnOptions(QString(), QStringLiteral("Auto mode"),
                           QStringLiteral("high"));
    session.sendUserText(QStringLiteral("second"));
    check(pump([&] {
              return eventCount(events, QStringLiteral("result")) == 2 &&
                     session.running();
          }),
          "a follow-up naming no model still completes its turn");
    // Third turn only so the stub exits and the log is complete.
    session.sendUserText(QStringLiteral("third"));
    check(pump([&] { return finished; }), "keep-model stub exits");

    QList<QJsonObject> starts;
    for (const QJsonObject &message : readMessages(logPath)) {
        if (message.value(QStringLiteral("method")).toString() ==
            QStringLiteral("turn/start"))
            starts.append(message);
    }
    const QJsonObject params = starts.size() > 1
                                   ? starts.at(1)
                                         .value(QStringLiteral("params"))
                                         .toObject()
                                   : QJsonObject();
    check(starts.size() == 3 &&
              params.value(QStringLiteral("model")).toString() ==
                  QStringLiteral("gpt-old") &&
              params.value(QStringLiteral("effort")).toString() ==
                  QStringLiteral("high"),
          "an empty setTurnOptions model keeps the thread's own model and still "
          "applies the rest");
}

void runFatalErrorTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("fatal-error.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("fatal-error"), logPath});
    QList<QJsonObject> events;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("fail"));
    check(pump([&] { return finished; }), "fatal error stub completes");
    QJsonObject result;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() ==
            QStringLiteral("result"))
            result = event;
    }
    check(eventCount(events, QStringLiteral("result")) == 1 &&
              result.value(QStringLiteral("is_error")).toBool() &&
              result.value(QStringLiteral("result"))
                  .toString()
                  .contains(QStringLiteral("fatal turn")),
          "non-retrying error emits one terminal result");
}

void runUnshownMessageTest(const QString &executable)
{
    QTemporaryDir temp;
    const QString logPath = temp.filePath(QStringLiteral("unshown.jsonl"));
    CodexAppServerSession session;
    session.setAppServerCommand(executable,
                                {QStringLiteral("--codex-stub"),
                                 QStringLiteral("unshown"), logPath});
    QList<QJsonObject> events;
    bool finished = false;
    QObject::connect(&session, &CodexAppServerSession::event,
                     [&](const QJsonObject &event) { events.append(event); });
    QObject::connect(&session, &CodexAppServerSession::finished,
                     [&](int) { finished = true; });
    session.start(temp.path(), QStringList(), QStringLiteral("unshown"));
    check(pump([&] { return finished; }), "unshown-message stub completes");
    QStringList kinds;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() ==
            QLatin1String("_local_raw_line"))
            kinds << forkmesh::transcriptcoverage::eventGaps(event);
    }
    check(kinds == QStringList{QStringLiteral("codex:model/rerouted"),
                               QStringLiteral("codex-item:hookPrompt")},
          qPrintable(QStringLiteral("only the Codex messages the session drops are "
                                    "recorded for the transcript check (got %1)")
                         .arg(kinds.join(QStringLiteral(", ")))));
    QStringList notices;
    QStringList errors;
    for (const QJsonObject &event : events) {
        if (event.value(QStringLiteral("type")).toString() != QLatin1String("_local_notice"))
            continue;
        const QString text = event.value(QStringLiteral("text")).toString();
        notices.append(text);
        if (event.value(QStringLiteral("level")).toString() == QLatin1String("error"))
            errors.append(text);
        check(forkmesh::transcriptcoverage::eventGaps(event).isEmpty(),
              "normalized Codex status notices are drawn transcript events");
    }
    check(notices == QStringList{
              QStringLiteral("Remote control (a): disabled"),
              QStringLiteral("MCP server codex_apps: starting"),
              QStringLiteral("MCP server codex_apps: failed — connection refused; transport"),
              QStringLiteral("Thread settings updated — model: gpt-test, effort: high, approvals: never, directory: /workspace, sandbox: dangerFullAccess, mode: default"),
              QStringLiteral("Remote control: unknown"),
              QStringLiteral("MCP server: unknown"),
              QStringLiteral("Thread settings updated.")},
          "status messages produce readable notices, omit instructions and moderation bookkeeping, and tolerate absent fields");
    check(errors == QStringList{QStringLiteral("MCP server codex_apps: failed — connection refused; transport")},
          "MCP startup failures use the existing error notice style");
}

// Which conversation a resumed session may pick up. A session can be handed
// between Claude Code and Codex between runs, so one transcript can hold both
// CLIs' identities — and Codex stamps its thread id into `session_id` as well
// as `thread_id`, which is exactly the field Claude Code's `--resume` reads.
void runResumeIdentityTest()
{
    using namespace forkmesh::agents;
    auto claudeEvent = [](const QString &id) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("assistant")},
                           {QStringLiteral("session_id"), id}};
    };
    auto codexEvent = [](const QString &id) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("assistant")},
                           {QStringLiteral("session_id"), id},
                           {QStringLiteral("thread_id"), id}};
    };
    const QJsonObject codexInit{
        {QStringLiteral("type"), QStringLiteral("system")},
        {QStringLiteral("subtype"), QStringLiteral("init")},
        {QStringLiteral("session_id"), QStringLiteral("codex-init")},
        {QStringLiteral("provider"), QStringLiteral("codex")}};
    const QJsonObject localTurn{
        {QStringLiteral("type"), QStringLiteral("_local_user")},
        {QStringLiteral("text"), QStringLiteral("Continue where you left off.")}};

    const QList<QJsonObject> codexOnly{codexInit, codexEvent("codex-1"), localTurn};
    check(codexResumeThreadId(codexOnly) == QStringLiteral("codex-1"),
          "a Codex run resumes its own thread");
    check(claudeResumeSessionId(codexOnly).isEmpty(),
          "Claude Code is never handed a Codex thread id to --resume");

    const QList<QJsonObject> claudeOnly{claudeEvent("claude-1"), localTurn};
    check(claudeResumeSessionId(claudeOnly) == QStringLiteral("claude-1"),
          "a Claude Code run resumes its own session");
    check(codexResumeThreadId(claudeOnly).isEmpty(),
          "Codex is never handed a Claude Code session id to resume");

    // Bounced back and forth: each CLI finds the newest conversation it owns,
    // not merely the newest event in the transcript.
    const QList<QJsonObject> mixed{claudeEvent("claude-1"), codexInit,
                                   codexEvent("codex-2"), claudeEvent("claude-3"),
                                   codexEvent("codex-4")};
    check(claudeResumeSessionId(mixed) == QStringLiteral("claude-3"),
          "a hand-back to Claude Code resumes its latest own session");
    check(codexResumeThreadId(mixed) == QStringLiteral("codex-4"),
          "a hand-back to Codex resumes its latest own thread");

    check(claudeResumeSessionId({}).isEmpty() && codexResumeThreadId({}).isEmpty(),
          "an empty transcript resumes nothing");
}

// One CLI, two logins. Each provider account is its own config root, so the
// account that hit its usage limit owns the conversation, and the account picked
// to carry on cannot open it — resuming it anyway fails the turn exactly like
// handing Claude Code a Codex thread id.
void runResumeAccountTest()
{
    using namespace forkmesh::agents;
    auto claudeEvent = [](const QString &id, const QString &account) {
        QJsonObject ev{{QStringLiteral("type"), QStringLiteral("assistant")},
                       {QStringLiteral("session_id"), id}};
        if (!account.isEmpty())
            ev.insert(resumeAccountKey(), account);
        return ev;
    };
    auto codexEvent = [](const QString &id, const QString &account) {
        QJsonObject ev{{QStringLiteral("type"), QStringLiteral("assistant")},
                       {QStringLiteral("session_id"), id},
                       {QStringLiteral("thread_id"), id}};
        if (!account.isEmpty())
            ev.insert(resumeAccountKey(), account);
        return ev;
    };

    const QList<QJsonObject> ranOutOnA{claudeEvent("claude-1", "acct-a")};
    check(claudeResumeSessionId(ranOutOnA, QStringLiteral("acct-a")) ==
              QStringLiteral("claude-1"),
          "the account that ran the turn resumes its own conversation");
    check(claudeResumeSessionId(ranOutOnA, QStringLiteral("acct-b")).isEmpty(),
          "a second account is never handed the first account's conversation");
    check(resumeConversationAccountId(ranOutOnA, /*codexTransport=*/false) ==
              QStringLiteral("acct-a"),
          "the hand-off notice can name the account that owns the work");

    // Switched away and back: the account picked up its earlier conversation
    // rather than starting over, exactly as a hand-back between CLIs does.
    const QList<QJsonObject> bounced{claudeEvent("claude-1", "acct-a"),
                                     claudeEvent("claude-2", "acct-b"),
                                     claudeEvent("claude-3", "acct-a")};
    check(claudeResumeSessionId(bounced, QStringLiteral("acct-b")) ==
              QStringLiteral("claude-2"),
          "each account resumes the newest conversation it owns");
    check(claudeResumeSessionId(bounced, QStringLiteral("acct-a")) ==
              QStringLiteral("claude-3"),
          "a hand-back resumes that account's latest own conversation");

    // Codex accounts are isolated by CODEX_HOME the same way.
    const QList<QJsonObject> codexOnA{codexEvent("codex-1", "acct-a")};
    check(codexResumeThreadId(codexOnA, QStringLiteral("acct-a")) ==
              QStringLiteral("codex-1"),
          "a Codex account resumes its own thread");
    check(codexResumeThreadId(codexOnA, QStringLiteral("acct-b")).isEmpty(),
          "a second Codex account is never handed the first's thread");

    // Sessions recorded before conversations carried an account stamp must keep
    // resuming: an unstamped turn is the selected account's until proven
    // otherwise, so a long-lived single-account session is untouched.
    const QList<QJsonObject> legacy{claudeEvent("claude-1", QString())};
    check(claudeResumeSessionId(legacy, QStringLiteral("acct-a")) ==
              QStringLiteral("claude-1"),
          "an unstamped conversation still resumes under any account");
    check(resumeConversationAccountId(legacy, /*codexTransport=*/false).isEmpty(),
          "an unstamped conversation names no account to hand over from");
}

// How the transcript retells what a turn did (adhoc #34): Codex explores by
// running shell commands, so the difference between a line under "Explored" and
// a spelled-out "Ran <command>" is a reading of the command line itself.
void runTranscriptStyleTest()
{
    using namespace CodexTranscriptStyle;
    QString verb, target;

    check(exploreLine(QStringLiteral("sed -n '400,520p' src/MainWindow.cpp"), verb,
                      target)
              && verb == QStringLiteral("Read")
              && target == QStringLiteral("MainWindow.cpp"),
          "sed range reads as Read <file>");
    check(exploreLine(QStringLiteral("tail -n 40 /var/log/agent.log"), verb, target)
              && verb == QStringLiteral("Read")
              && target == QStringLiteral("agent.log"),
          "tail -n reads the file, not the line count");
    // The pipeline test has to survive a regex argument: 'a|b' is one word.
    check(exploreLine(QStringLiteral("rg -n 'fixBranch|resolveBranch' src/Main.cpp"),
                      verb, target)
              && verb == QStringLiteral("Search")
              && target == QStringLiteral("fixBranch|resolveBranch in Main.cpp"),
          "quoted alternation stays a search, not a pipeline");
    check(exploreLine(QStringLiteral("ls src/agents"), verb, target)
              && verb == QStringLiteral("List")
              && target == QStringLiteral("src/agents"),
          "ls reads as List <dir>");

    check(!exploreLine(QStringLiteral("git status --short"), verb, target),
          "a real command is not exploration");
    check(!exploreLine(QStringLiteral("rg -n TODO src | head -20"), verb, target),
          "an actual pipeline is not exploration");
    check(!exploreLine(QStringLiteral("cat src/a.cpp > /tmp/copy"), verb, target),
          "a redirect makes a read a write");
    check(!exploreLine(QStringLiteral("cat a.txt && rm b.txt"), verb, target),
          "a chained command is not exploration");
    check(!exploreLine(QStringLiteral("set -e\nsed -n '1,5p' a.cpp"), verb, target),
          "a multi-line script is not exploration");

    // The command line itself, however Codex reported it.
    check(commandText(QJsonObject{{QStringLiteral("command"),
                                   QStringLiteral("git status")}})
              == QStringLiteral("git status"),
          "a string command passes through");
    check(commandText(QJsonObject{{QStringLiteral("command"),
                                   QJsonArray{QStringLiteral("bash"),
                                              QStringLiteral("-lc"),
                                              QStringLiteral("ninja -C build")}}})
              == QStringLiteral("ninja -C build"),
          "argv drops the bash -lc wrapper");
    check(commandText(QJsonObject{{QStringLiteral("command"),
                                   QJsonArray{QStringLiteral("git"),
                                              QStringLiteral("status")}}})
              == QStringLiteral("git status"),
          "a plain argv joins back into its command line");
    const QString wrapped = QStringLiteral(
        "/bin/bash -lc \"rg -n \\\"build\\\" .gitignore\n"
        "git status --short\"");
    const QString unwrapped =
        QStringLiteral("rg -n \"build\" .gitignore\ngit status --short");
    check(commandText(QJsonObject{{QStringLiteral("command"), wrapped}})
              == unwrapped,
          "a string command also drops the displayed /bin/bash -lc wrapper");
    check(singleLineCommand(unwrapped)
              == QStringLiteral(
                  "rg -n \"build\" .gitignore; git status --short"),
          "separate Bash statements render on one line with a real separator");
    check(singleLineCommand(QStringLiteral("rg TODO src |\nhead -20"))
              == QStringLiteral("rg TODO src | head -20"),
          "a newline after a shell operator becomes spacing, not a semicolon");
    check(singleLineCommand(QStringLiteral("printf 'first\nsecond'"))
              == QStringLiteral("printf 'first\\nsecond'"),
          "a newline inside a quoted value remains visible on the one-line card");

    check(toolVerb(QStringLiteral("FileChange")) == QStringLiteral("Edited")
              && toolVerb(QStringLiteral("Bash")).isEmpty(),
          "a write is titled by what it did");
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() > 1 && arguments.at(1) == QStringLiteral("--codex-stub"))
        return runStub(arguments);

    const QString executable = QCoreApplication::applicationFilePath();
    runDefaultLaunchEnvironmentTest(executable);
    runFinalTeardownProcessOwnershipTest(executable);
    runFreshProtocolTest(executable);
    runResumeAndSteerTest(executable);
    runModeMappingTest(executable, QStringLiteral("Ask before edits"),
                       QStringLiteral("never"),
                       QStringLiteral("danger-full-access"));
    runModeMappingTest(executable, QStringLiteral("Edit automatically"),
                       QStringLiteral("never"),
                       QStringLiteral("danger-full-access"));
    runModeMappingTest(executable, QStringLiteral("Plan"),
                       QStringLiteral("never"),
                       QStringLiteral("danger-full-access"));
    runModeMappingTest(executable, QStringLiteral("Auto"),
                       QStringLiteral("never"),
                       QStringLiteral("danger-full-access"));
    runFullAccessSandboxPolicyTest(executable);
    runInterruptedTurnTest(executable);
    runRejectedSteerTest(executable);
    runResumeFailureTest(executable);
    runInterruptTest(executable);
    runTurnOptionsTest(executable);
    runActiveTurnModeSwitchTest(executable);
    runTurnOptionsKeepModelTest(executable);
    runFatalErrorTest(executable);
    runUnshownMessageTest(executable);
    runResumeIdentityTest();
    runResumeAccountTest();
    runTranscriptStyleTest();

    if (failures == 0)
        qInfo("All Codex app-server protocol tests passed.");
    else
        qCritical("%d Codex app-server protocol test(s) failed.", failures);
    return failures == 0 ? 0 : 1;
}
