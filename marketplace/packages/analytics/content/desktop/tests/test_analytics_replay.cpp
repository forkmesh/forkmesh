#include "AnalyticsReplay.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace forkmesh::analytics;

namespace {

int failures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ",
                    detail.toUtf8().constData());
        ++failures;
    }
}

QJsonArray motion(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).array();
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    MotionTrack track;
    track.append(motion(R"([
      {"t": 1000, "k": "meta", "d": {"href": "https://example.test/", "width": 1000, "height": 600, "pageHeight": 2400}},
      {"t": 1000, "k": "full", "d": {"doc": 1, "x": 0, "y": 0}},
      {"t": 1200, "k": "move", "d": {"positions": [[100, 100, 0], [200, 150, 400]]}},
      {"t": 1500, "k": "scroll", "d": {"id": 1, "x": 0, "y": 300, "h": 2600}},
      {"t": 1550, "k": "scroll", "d": {"id": 9, "x": 0, "y": 80}},
      {"t": 1700, "k": "mouse", "d": {"type": "click", "x": 200, "y": 150}}
    ])"));
    track.append(motion(R"([
      {"t": 9000, "k": "meta", "d": {"href": "https://example.test/docs?x=1", "width": 1000, "height": 600}},
      {"t": 9000, "k": "full", "d": {"doc": 4, "x": 0, "y": 0}},
      {"t": 9100, "k": "scroll", "d": {"id": 4, "x": 0, "y": 120}}
    ])"));
    track.finish();

    check(!track.isEmpty() && track.start() == 1000 && track.end() == 9100, "the track spans the first to the last event");
    check(track.pages().size() == 2 && track.pages().at(1).href == QStringLiteral("https://example.test/docs?x=1"),
          "each full snapshot's meta starts a page");
    check(track.scrolls().size() == 4, "only window scrolls are kept",
          QString::number(track.scrolls().size()));

    const MotionState middle = track.stateAt(1400);
    check(middle.hasCursor && qAbs(middle.cursor.x() - 150) < 0.5 && qAbs(middle.cursor.y() - 125) < 0.5,
          "the cursor is interpolated between samples",
          QStringLiteral("%1,%2").arg(middle.cursor.x()).arg(middle.cursor.y()));
    check(middle.scrollY == 0 && middle.viewport == QSize(1000, 600) && middle.pageHeight == 2400,
          "before the scroll the page sits at the top");

    const MotionState scrolled = track.stateAt(1750);
    check(scrolled.scrollY == 300 && scrolled.pageHeight == 2600, "a window scroll moves the page and grows it",
          QString::number(scrolled.scrollY));
    check(scrolled.clicks.size() == 1 && track.clicks().first().scrollY == 300,
          "a recent click shows and remembers the scroll it happened at");
    check(track.stateAt(3000).clicks.isEmpty(), "a click ripple fades");

    const MotionState second = track.stateAt(9050);
    check(second.pageIndex == 1 && second.scrollY == 0 && second.pageHeight == 600,
          "a new page starts at its own scroll position",
          QStringLiteral("%1 %2").arg(second.scrollY).arg(second.pageHeight));
    check(track.nextActivity(2000) == 9000, "inactivity skips to the next page");

    check(MotionTrack::countryFlag(QStringLiteral("nl")) == QString::fromUcs4(U"\U0001F1F3\U0001F1F1"),
          "a country code becomes its flag");
    check(MotionTrack::countryFlag(QStringLiteral("N1")).isEmpty(), "a malformed code has no flag");
    check(MotionTrack::pathOf(QStringLiteral("https://example.test/docs?x=1")) == QStringLiteral("/docs"),
          "a recording page shows as its path");
    check(AnalyticsReplayDialog::clockText(3723000) == QStringLiteral("1:02:03")
              && AnalyticsReplayDialog::clockText(65000) == QStringLiteral("1:05"),
          "playback clock text");

    MotionTrack empty;
    empty.finish();
    check(empty.isEmpty() && !empty.stateAt(0).hasCursor && empty.stateAt(0).viewport == QSize(1280, 800),
          "an empty recording has a default stage and no cursor");

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
