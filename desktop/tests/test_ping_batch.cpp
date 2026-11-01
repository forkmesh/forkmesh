#include "PingBatch.h"

#include <iostream>

using namespace forkmesh::pingbatch;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static Item ping(const QString &title, const QString &body = QString(),
                 bool warning = false)
{
    Item item;
    item.title = title;
    item.body = body;
    item.warning = warning;
    return item;
}

static void testThresholds()
{
    check(nextThreshold(0) == 10, "first summary at 10");
    check(nextThreshold(9) == 10, "still 10 below it");
    check(nextThreshold(10) == 100, "then 100");
    check(nextThreshold(99) == 100, "100 below it");
    check(nextThreshold(100) == 200, "then every 100");
    check(nextThreshold(250) == 300, "next hundred");
}

static void testSlowPingsShowOneByOne()
{
    Batcher batch;
    qint64 now = 1000;
    for (int i = 0; i < 20; ++i) {
        check(batch.offer(ping(QStringLiteral("Ping %1").arg(i)), now) == Step::Show,
              "a ping after a quiet gap shows on its own");
        now += kQuietMs;
    }
    check(batch.held() == 0, "nothing held for slow pings");
}

static void testBurstGoesToTenThenHundred()
{
    Batcher batch;
    qint64 now = 1000;
    int shown = 0;
    int summaries = 0;
    QList<int> summaryCounts;
    for (int i = 1; i <= 100; ++i) {
        const Step step = batch.offer(
            ping(QStringLiteral("Agent #%1 is done!").arg(i), QStringLiteral("task %1").arg(i)),
            now);
        if (step == Step::Show)
            ++shown;
        if (step == Step::Summarize) {
            ++summaries;
            summaryCounts.append(batch.take().count);
        }
        now += 100;
    }
    check(shown == kSoloCards, "only the first three show individually");
    check(summaries == 2, "a summary at 10 and at 100");
    check(summaryCounts == QList<int>({7, 90}), "summaries cover the held pings");
    check(batch.held() == 0, "nothing left after the 100 summary");
    check(batch.burstTotal() == 100, "burst total counts every ping");
}

static void testQuietFlushesTheRest()
{
    Batcher batch;
    qint64 now = 1000;
    for (int i = 0; i < 6; ++i)
        batch.offer(ping(QStringLiteral("Action failed #%1").arg(i), QString(), true), now++);
    check(batch.held() == 3, "three held after the solo cards");
    check(!batch.due(now), "not due while pings still arrive");
    check(batch.dueAtMs() == now - 1 + kQuietMs, "due once the burst goes quiet");
    check(batch.due(now - 1 + kQuietMs), "due after the quiet gap");
    const Card card = batch.take();
    check(card.summary && card.count == 3, "quiet flush is a summary of three");
    check(card.warning, "a held warning makes the summary a warning");
    check(card.title.contains(QStringLiteral("3 with a problem")), "title counts problems");
    check(batch.offer(ping(QStringLiteral("next")), now + 2 * kQuietMs) == Step::Show,
          "a new burst starts with solo cards");
}

static void testOneHeldPingFlushesAsItself()
{
    Batcher batch;
    for (int i = 0; i < 4; ++i)
        batch.offer(ping(QStringLiteral("Ping %1").arg(i), QStringLiteral("body")), 1000 + i);
    const Card card = batch.take();
    check(!card.summary, "a single held ping is not a summary");
    check(card.title == QLatin1String("Ping 3") && card.body == QLatin1String("body"),
          "a single held ping keeps its own text");
}

static void testMaxHold()
{
    Batcher batch;
    qint64 now = 0;
    for (int i = 0; i < 4; ++i)
        batch.offer(ping(QStringLiteral("p")), now += 1000);
    const qint64 firstHeld = now;
    while (now < firstHeld + kMaxHoldMs) {
        now += kQuietMs / 2;
        if (batch.due(now))
            break;
        batch.offer(ping(QStringLiteral("p")), now);
    }
    check(batch.due(now), "a steady trickle still flushes after the hold limit");
    check(now - firstHeld <= kMaxHoldMs + kQuietMs, "flush lands near the hold limit");
}

static void testSummaryText()
{
    QList<Item> items;
    for (int i = 1; i <= 8; ++i)
        items.append(ping(QStringLiteral("Agent #%1 is done!").arg(i), QStringLiteral("task %1").arg(i)));
    items.append(ping(QStringLiteral("Action failed"), QStringLiteral("Tests on main"), true));
    items.append(ping(QStringLiteral("New message from sam"), QStringLiteral("hello there")));
    const Card card = summarize(items, 13);
    check(card.title == QLatin1String("10 more pings, 1 with a problem"), "summary title");
    const QStringList lines = card.body.split(QLatin1Char('\n'));
    check(lines.size() == 4, "one line per kind plus the total");
    check(lines.value(0).startsWith(QStringLiteral("Action failed")), "problems come first");
    check(lines.value(0).contains(QStringLiteral("Tests on main")), "a lone ping shows its detail");
    check(lines.value(1).startsWith(QString::fromUtf8("8\xC3\x97 Agent is done!")),
          "repeats are counted under one title");
    check(lines.value(1).contains(QStringLiteral("#4, #5, #6, #7, #8 +3")),
          "repeats list their latest numbers");
    check(lines.value(2).contains(QStringLiteral("hello there")), "chat detail shown");
    check(lines.value(3).contains(QStringLiteral("13 pings in this burst")), "burst total shown");
}

static void testSummaryCapsKinds()
{
    QList<Item> items;
    for (int i = 0; i < 7; ++i)
        items.append(ping(QStringLiteral("Kind %1").arg(QChar(u'A' + i))));
    const Card card = summarize(items, 7);
    const QStringList lines = card.body.split(QLatin1Char('\n'));
    check(lines.size() == kSummaryGroups + 2, "extra kinds fold into one line");
    check(lines.value(kSummaryGroups) == QLatin1String("+ 3 more of 3 other kinds"),
          "folded kinds are counted");
}

static void testGroupTitle()
{
    QStringList refs;
    check(groupTitle(QStringLiteral("Agent #12 is done!"), &refs) == QLatin1String("Agent is done!"),
          "numbers drop out of the group title");
    check(refs == QStringList({QStringLiteral("#12")}), "numbers become refs");
    check(groupTitle(QStringLiteral("#7")) == QLatin1String("#7"), "a bare number stays");
}

int main()
{
    testThresholds();
    testSlowPingsShowOneByOne();
    testBurstGoesToTenThenHundred();
    testQuietFlushesTheRest();
    testOneHeldPingFlushesAsItself();
    testMaxHold();
    testSummaryText();
    testSummaryCapsKinds();
    testGroupTitle();
    if (failures)
        return 1;
    std::cout << "ping batch tests passed\n";
    return 0;
}
