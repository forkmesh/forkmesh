#include "AgentTicker.h"

#include <cstdio>

using namespace forkmesh::ui;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

void checkLine(const QString &prose, const QString &expected, const char *message)
{
    const QString got = agentTickerLine(prose);
    if (got == expected)
        return;
    std::fprintf(stderr, "FAIL: %s\n  got:      %s\n  expected: %s\n", message,
                 qPrintable(got), qPrintable(expected));
    ++failures;
}

}

int main()
{
    checkLine(QString(), QString(), "empty prose gives no line");
    checkLine(QStringLiteral("Running the tests now."),
              QStringLiteral("Running the tests now."), "plain prose is kept");
    checkLine(QStringLiteral("First paragraph.\n\nSecond one\nwraps here."),
              QStringLiteral("Second one wraps here."),
              "the last paragraph is joined into one line");
    checkLine(QStringLiteral("I changed it:\n\n```cpp\nint x = 1;\n```\n"),
              QStringLiteral("I changed it:"), "a fenced code block is skipped");
    checkLine(QStringLiteral("## **Done** with `build`"),
              QStringLiteral("Done with build"), "heading and inline marks are stripped");
    checkLine(QStringLiteral("- see [the docs](https://example.com) for *more*"),
              QStringLiteral("see the docs for more"), "bullet, link and emphasis are stripped");
    checkLine(QStringLiteral("Summary.\n\n| a | b |\n|---|---|\n| 1 | 2 |"),
              QStringLiteral("Summary."), "a table is skipped");
    checkLine(QStringLiteral("Keep snake_case_names as they are."),
              QStringLiteral("Keep snake_case_names as they are."),
              "underscores inside words are kept");
    const QString longLine = agentTickerLine(QString(300, QLatin1Char('a')));
    check(longLine.size() == 120, "a long line is cut to 120 characters");
    check(longLine.endsWith(QChar(0x2026)), "a cut line ends with an ellipsis");

    AgentTickerTrain train;
    check(train.layout(200, 0).isEmpty(), "an empty train shows nothing");
    train.setLine(QStringLiteral("A"), 100, 200, 0);
    QList<AgentTickerTrain::Car> cars = train.layout(200, 0);
    check(!cars.isEmpty() && cars.first().left == 0, "the line starts at the lane's left edge");
    check(cars.size() == 2, "copies fill the lane behind a short line");
    cars = train.layout(200, AgentTickerTrain::kHoldMs);
    check(!cars.isEmpty() && cars.first().left == 0, "the line waits before it scrolls");
    cars = train.layout(200, AgentTickerTrain::kHoldMs + 500);
    check(!cars.isEmpty() && qAbs(cars.first().left + 40) < 0.01,
          "the line scrolls at 80 px a second");
    check(cars.size() >= 2 && qAbs(cars.at(1).left - (cars.first().left + 105)) < 0.01,
          "the next copy follows 5 px behind");

    const qint64 lapMs = qint64(105 * 1000 / AgentTickerTrain::kSpeedPxPerSec);
    cars = train.layout(200, AgentTickerTrain::kHoldMs + lapMs);
    check(!cars.isEmpty() && qAbs(cars.first().left) < 1.0,
          "after one lap the next copy is where the first started");

    AgentTickerTrain swap;
    swap.setLine(QStringLiteral("A"), 100, 150, 0);
    swap.layout(150, AgentTickerTrain::kHoldMs + 500);
    swap.setLine(QStringLiteral("B"), 50, 150, AgentTickerTrain::kHoldMs + 500);
    cars = swap.layout(150, AgentTickerTrain::kHoldMs + 500);
    check(cars.size() == 2 && cars.at(0).text == QLatin1String("A") &&
              cars.at(1).text == QLatin1String("A"),
          "a new line leaves the copies already in the lane alone");
    cars = swap.layout(150, AgentTickerTrain::kHoldMs + 500 + lapMs);
    check(cars.size() >= 2 && cars.at(0).text == QLatin1String("A") &&
              cars.at(1).text == QLatin1String("B"),
          "the new line comes in right behind what was showing");
    check(swap.line() == QLatin1String("B"), "the train remembers its newest line");
    swap.setLine(QString(), 0, 200, 0);
    check(swap.isEmpty(), "an empty line closes the lane");

    if (failures == 0)
        std::printf("agent ticker tests passed\n");
    return failures == 0 ? 0 : 1;
}
