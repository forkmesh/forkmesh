#include "AgentCloudStream.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::agents;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject chunk(qint64 seq, qint64 job, const QByteArray &data)
{
    return QJsonObject{{QStringLiteral("seq"), static_cast<double>(seq)},
                       {QStringLiteral("job"), static_cast<double>(job)},
                       {QStringLiteral("data"), QString::fromLatin1(data.toBase64())}};
}

static QJsonObject answer(const QList<QJsonObject> &chunks, qint64 stream = 1000,
                          bool truncated = false, bool more = false, bool reset = false)
{
    QJsonArray array;
    for (const QJsonObject &c : chunks)
        array.append(c);
    const qint64 next = chunks.isEmpty()
                            ? 0
                            : static_cast<qint64>(
                                  chunks.last().value(QStringLiteral("seq")).toDouble());
    return QJsonObject{{QStringLiteral("ok"), true},
                       {QStringLiteral("stream"), static_cast<double>(stream)},
                       {QStringLiteral("chunks"), array},
                       {QStringLiteral("nextSeq"), static_cast<double>(next)},
                       {QStringLiteral("truncated"), truncated},
                       {QStringLiteral("more"), more},
                       {QStringLiteral("reset"), reset}};
}

static const QByteArray kAssistant =
    R"({"type":"assistant","message":{"content":[{"type":"text","text":"hello"}]}})";
static const QByteArray kTool =
    R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"ok"}]}})";
static const QByteArray kResult = R"({"type":"result","result":"done"})";

static void decodesEventsSplitAcrossChunks()
{
    CloudAgentStreamDecoder decoder;
    const QByteArray all = kAssistant + "\n" + kTool + "\n" + kResult + "\n";
    const int cut1 = 30, cut2 = kAssistant.size() + 20;
    CloudAgentStreamBatch first = decoder.apply(
        answer({chunk(1, 7, all.left(cut1)), chunk(2, 7, all.mid(cut1, cut2 - cut1))}));
    check(first.events.size() == 1, "one complete event from the first read");
    check(first.events.value(0).value("type").toString() == "assistant",
          "the assistant turn decodes");
    check(decoder.after() == 2, "the cursor follows nextSeq");
    CloudAgentStreamBatch second = decoder.apply(answer({chunk(3, 7, all.mid(cut2))}));
    check(second.events.size() == 2, "the rest of the events arrive");
    check(second.events.value(0).value("type").toString() == "user" &&
              second.events.value(1).value("type").toString() == "result",
          "tool result then result, in order");
    check(!first.restarted && !second.restarted, "one run, no restart");
}

static void skipsJunkAndPartialAfterAGap()
{
    CloudAgentStreamDecoder decoder;
    decoder.apply(answer({chunk(1, 7, kAssistant.left(10))}));
    CloudAgentStreamBatch gap = decoder.apply(answer(
        {chunk(40, 7, "tail of something\nnot json\n" + kResult + "\n")}, 1000, true));
    check(gap.events.size() == 1 && gap.events.first().value("type").toString() == "result",
          "after a gap the partial line is dropped and the next event decodes");
    CloudAgentStreamBatch junk = decoder.apply(answer({chunk(41, 7, "{\"no\":\"type\"}\n[1]\n")}));
    check(junk.events.isEmpty(), "objects without a type and non-objects are skipped");
}

static void aNewJobOrStreamRestartsTheRun()
{
    CloudAgentStreamDecoder decoder;
    decoder.apply(answer({chunk(1, 7, kAssistant + "\n")}));
    CloudAgentStreamBatch next = decoder.apply(
        answer({chunk(2, 7, kTool + "\n"), chunk(3, 9, kResult + "\n")}));
    check(next.restarted, "a follow-up job restarts the live run");
    check(next.events.size() == 1 && next.events.first().value("type").toString() == "result",
          "only the new job's events are kept");
    check(decoder.job() == 9, "the decoder follows the new job");
    CloudAgentStreamBatch other = decoder.apply(answer({chunk(1, 9, kAssistant + "\n")}, 2000));
    check(other.restarted && other.events.size() == 1,
          "a recreated relay stream starts over");
    CloudAgentStreamBatch reset = decoder.apply(answer({}, 2000, false, false, true));
    check(reset.restarted && decoder.after() == 0, "a reset answer rewinds the cursor");
}

static void watchFollowsOnlyTheSelectedRunningVisibleSession()
{
    CloudAgentStreamWatch watch;
    check(!watch.follow(0, QString(), false, true), "nothing selected, nothing to change");
    check(!watch.subscribed() && !watch.due(0), "no subscription without a selection");
    check(!watch.follow(-1000001, "a/b/s1", false, true), "a finished run is not followed");
    check(!watch.follow(-1000001, "a/b/s1", true, false), "a hidden page is not followed");
    check(watch.follow(-1000001, "a/b/s1", true, true), "a visible running run is followed");
    check(watch.subscribed() && watch.sessionId() == -1000001, "subscribed to the selection");
    check(watch.due(0), "the first read is due at once");
    const quint64 generation = watch.generation();
    check(!watch.follow(-1000001, "a/b/s1", true, true), "the same selection changes nothing");
    watch.started(1000);
    check(!watch.due(1000) && watch.inFlight(), "one read at a time");
    CloudAgentStreamBatch batch =
        watch.finished(true, answer({chunk(1, 7, kAssistant + "\n")}), 1500);
    check(batch.events.size() == 1 && watch.after() == 1, "the read's events are returned");
    check(!watch.due(1600), "the fallback cadence waits");
    check(watch.due(1500 + CloudAgentStreamWatch::kFallbackMs), "and then reads again");
    watch.ring();
    check(watch.due(1600), "a doorbell reads at once");
    watch.started(1600);
    watch.finished(true, answer({chunk(2, 7, kTool + "\n")}, 1000, false, true), 1700);
    check(watch.due(1700), "a page with more reads again at once");
    watch.started(1700);
    watch.finished(false, {}, 1800);
    check(!watch.due(1800 + CloudAgentStreamWatch::kFallbackMs) &&
              watch.due(1800 + CloudAgentStreamWatch::kRetryMs),
          "a failed read backs off");
    check(watch.follow(-1000002, "a/b/s2", true, true), "another selection moves the watch");
    check(watch.generation() != generation && watch.after() == 0,
          "and starts it from the beginning");
    watch.started(2000);
    check(watch.follow(-1000002, "a/b/s2", false, true), "the run finishing unsubscribes");
    check(!watch.subscribed() && !watch.inFlight() && !watch.due(99999),
          "nothing is read for a finished run");
    watch.follow(-1000002, "a/b/s2", true, true);
    check(watch.follow(0, QString(), false, true), "deselecting unsubscribes");
    watch.follow(-1000002, "a/b/s2", true, true);
    check(watch.follow(-1000002, "a/b/s2", true, false), "hiding the page unsubscribes");
}

int main()
{
    decodesEventsSplitAcrossChunks();
    skipsJunkAndPartialAfterAGap();
    aNewJobOrStreamRestartsTheRun();
    watchFollowsOnlyTheSelectedRunningVisibleSession();
    if (failures == 0)
        std::cout << "agent cloud stream: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
