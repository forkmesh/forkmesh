#include "AgentUsageResetClock.h"

#include <QTimeZone>

#include <iostream>

using namespace forkmesh::usageclock;

static int failures = 0;

static void expect(const QString &actual, const QString &wanted, const char *what)
{
    if (actual == wanted)
        return;
    std::cerr << "FAIL: " << what << ": got \"" << actual.toStdString()
              << "\", wanted \"" << wanted.toStdString() << "\"\n";
    ++failures;
}

static qint64 at(const QDateTime &now, int days, int hour, int minute)
{
    return QDateTime(now.date().addDays(days), QTime(hour, minute), now.timeZone())
        .toMSecsSinceEpoch();
}

int main()
{
    const QDateTime now(QDate(2026, 10, 19), QTime(10, 5), QTimeZone::utc());
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");

    expect(resetClockLabel(at(now, 0, 15, 45), now), QStringLiteral("Resets 3:45 PM"),
           "later today");
    expect(resetClockLabel(at(now, 0, 23, 59), now), QStringLiteral("Resets 11:59 PM"),
           "late tonight");
    expect(resetClockLabel(at(now, 1, 0, 0), now), QStringLiteral("Resets Tue 12:00 AM"),
           "midnight is tomorrow");
    expect(resetClockLabel(at(now, 1, 9, 0), now), QStringLiteral("Resets Tue 9:00 AM"),
           "tomorrow morning");
    expect(resetClockLabel(at(now, 6, 12, 30), now), QStringLiteral("Resets Sun 12:30 PM"),
           "six days out");
    expect(resetClockLabel(at(now, 7, 10, 0), now), QStringLiteral("Resets Oct 26, 10:00 AM"),
           "a week out names the date");
    expect(resetClockLabel(at(now, 0, 9, 0), now), QString(), "already passed");
    expect(resetClockLabel(0, now), QString(), "no reset time");

    const QDateTime kolkata(QDate(2026, 10, 19), QTime(23, 0),
                            QTimeZone(QByteArrayLiteral("Asia/Kolkata")));
    expect(resetClockLabel(kolkata.toMSecsSinceEpoch() + 2 * 3600 * 1000, kolkata),
           QStringLiteral("Resets Tue 1:00 AM"), "uses the viewer's time zone");

    expect(noteWithResetClock(QStringLiteral("resets in 5 h 40 min") + dot +
                                  QStringLiteral("on pace"),
                              at(now, 0, 15, 45), now),
           QStringLiteral("Resets 3:45 PM (in 5 h 40 min)") + dot + QStringLiteral("on pace"),
           "replaces the relative segment");
    expect(noteWithResetClock(QStringLiteral("58% available") + dot +
                                  QStringLiteral("resets in 2 d"),
                              at(now, 2, 9, 0), now),
           QStringLiteral("58% available") + dot + QStringLiteral("Resets Wed 9:00 AM (in 2 d)"),
           "keeps the availability first");
    expect(noteWithResetClock(QStringLiteral("on pace"), at(now, 0, 15, 45), now),
           QStringLiteral("on pace") + dot + QStringLiteral("Resets 3:45 PM"),
           "appends when no relative text");
    expect(noteWithResetClock(QString(), at(now, 0, 15, 45), now),
           QStringLiteral("Resets 3:45 PM"), "empty note");
    expect(noteWithResetClock(QStringLiteral("ready"), 0, now), QStringLiteral("ready"),
           "unchanged without a reset");

    if (failures)
        return 1;
    std::cout << "agent usage reset clock: ok\n";
    return 0;
}
