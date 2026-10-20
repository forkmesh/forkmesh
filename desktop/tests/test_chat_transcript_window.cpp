#include "ChatTranscriptWindow.h"

#include <cstdio>

namespace {

int failures = 0;

void expectEq(qsizetype actual, qsizetype expected, const char *what)
{
    if (actual == expected)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n  expected: %lld\n  actual:   %lld\n", what,
                 static_cast<long long>(expected),
                 static_cast<long long>(actual));
}

void expectTrue(bool ok, const char *what)
{
    if (ok)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what);
}

QList<qsizetype> rowsOf(int count, qsizetype chars)
{
    return QList<qsizetype>(count, chars);
}

}

int main()
{
    using namespace forkmesh::chattranscript;

    expectEq(firstRenderedRow({}, 1), 0, "an empty conversation renders nothing");
    expectEq(firstRenderedRow(rowsOf(12, 80), 1), 0,
             "a short chat renders every message");
    expectEq(firstRenderedRow(rowsOf(500, 40), 1), 500 - kBatchMessages,
             "a long chat of short lines stops at the message budget");
    expectEq(firstRenderedRow(rowsOf(500, 40), 2), 500 - 2 * kBatchMessages,
             "each Show earlier press adds another message batch");

    const QList<qsizetype> mailLog = rowsOf(277, 2200);
    const int first = firstRenderedRow(mailLog, 1);
    qsizetype rendered = 0;
    for (int index = first; index < mailLog.size(); ++index)
        rendered += mailLog.at(index);
    expectTrue(first > 0 && rendered <= kBatchChars,
               "a feed of long messages stops at the character budget");
    expectTrue(mailLog.size() - first >= 5,
               "the character budget still shows several recent messages");
    expectTrue(firstRenderedRow(mailLog, 2) < first,
               "Show earlier reaches further back through long messages");

    expectEq(firstRenderedRow({100, 100, 4 * kBatchChars}, 1), 2,
             "the newest message renders even when it alone exceeds the budget");
    expectEq(firstRenderedRow(rowsOf(30, 10), 0), 0,
             "a zero batch count is treated as one batch");

    expectTrue(!overBudget(kBatchMessages, kBatchChars, 1),
               "a freshly rendered window is within budget");
    expectTrue(overBudget(2 * kBatchMessages + 1, 0, 1),
               "live appends past twice the message budget ask for a re-window");
    expectTrue(overBudget(10, 2 * kBatchChars + 1, 1),
               "live appends past twice the character budget ask for a re-window");
    expectTrue(!overBudget(1, 10 * kBatchChars, 1),
               "one oversized message never loops into re-windowing");

    if (failures == 0)
        std::printf("chat transcript window: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
