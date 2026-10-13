#include "UserActivityChart.h"
#include "UserActivityLog.h"

#include <QApplication>
#include <QDateTime>
#include <QLineEdit>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVBoxLayout>
#include <QWidget>

#include <iostream>

using namespace forkmesh::activity;

static int failures = 0;

static void check(bool ok, const QString &what)
{
    if (ok)
        return;
    std::cerr << "FAIL: " << qPrintable(what) << "\n";
    ++failures;
}

static int sum(const QVector<int> &counts)
{
    int total = 0;
    for (int value : counts)
        total += value;
    return total;
}

static void testSlots()
{
    check(kActivitySlots == 288, QStringLiteral("24 columns of 12 rows"));
    check(activitySlotFor(QTime(0, 0)) == 0, QStringLiteral("midnight is slot 0"));
    check(activitySlotFor(QTime(0, 4, 59)) == 0, QStringLiteral("00:04 is slot 0"));
    check(activitySlotFor(QTime(0, 5)) == 1, QStringLiteral("00:05 is slot 1"));
    check(activitySlotFor(QTime(14, 37)) == 14 * 12 + 7,
          QStringLiteral("14:37 is hour 14 row 7"));
    check(activitySlotFor(QTime(23, 59, 59)) == 287,
          QStringLiteral("23:59 is the last slot"));
    check(activitySlotFor(QTime()) == -1, QStringLiteral("invalid time"));
}

static void testRecordAndRollover()
{
    UserActivityLog log{QString()};
    const QDate day(2026, 10, 2);
    log.record(QDateTime(day, QTime(9, 1)));
    log.record(QDateTime(day, QTime(9, 3)));
    log.record(QDateTime(day, QTime(17, 55)));
    const QVector<int> counts = log.slotsFor(day);
    check(counts.size() == kActivitySlots, QStringLiteral("full day of counts"));
    check(counts.at(9 * 12) == 2, QStringLiteral("two in 09:00-09:05"));
    check(counts.at(17 * 12 + 11) == 1, QStringLiteral("one in 17:55-18:00"));
    check(sum(log.slotsFor(day.addDays(1))) == 0,
          QStringLiteral("another day reads empty"));
    log.record(QDateTime(day.addDays(1), QTime(0, 2)));
    check(sum(log.slotsFor(day)) == 0, QStringLiteral("rollover drops yesterday"));
    check(log.slotsFor(day.addDays(1)).at(0) == 1,
          QStringLiteral("rollover starts the new day"));
}

static void testPersistence()
{
    QTemporaryDir dir;
    check(dir.isValid(), QStringLiteral("temp dir"));
    const QString path = dir.filePath(QStringLiteral("state/activity.json"));
    const QDate day(2026, 10, 2);
    {
        UserActivityLog log(path);
        for (int i = 0; i < 5; ++i)
            log.record(QDateTime(day, QTime(11, 20)));
    }
    UserActivityLog reread(path);
    check(reread.slotsFor(day).at(11 * 12 + 4) == 5,
          QStringLiteral("counts survive a restart"));
    UserActivityLog none{QString()};
    check(sum(none.slotsFor(day)) == 0, QStringLiteral("no path, no file"));
}

static void testDeviceKeyAndOthers()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("activity.json"));
    const QDate day = QDate::currentDate();
    QString key;
    {
        UserActivityLog log(path);
        key = log.deviceKey();
        check(key.size() >= 8 && key.size() <= 64,
              QStringLiteral("a device key the server accepts"));
        const quint64 before = log.revision();
        log.record(QDateTime(day, QTime(8, 0)));
        check(log.revision() == before + 1, QStringLiteral("a record bumps the revision"));
    }
    UserActivityLog log(path);
    check(log.deviceKey() == key, QStringLiteral("the device key survives a restart"));
    check(UserActivityLog{QString()}.deviceKey() != key,
          QStringLiteral("another log gets its own key"));

    QVector<int> others(kActivitySlots, 0);
    others[8 * 12] = 4;
    others[20 * 12] = 2;
    log.setOthers(day, others, {QStringLiteral("ios"), QStringLiteral("web")});
    check(log.slotsFor(day).at(8 * 12) == 5,
          QStringLiteral("other devices add to this desktop's slot"));
    check(log.today().at(20 * 12) == 2, QStringLiteral("today shows the other devices"));
    check(log.localSlotsFor(day).at(8 * 12) == 1 && log.localSlotsFor(day).at(20 * 12) == 0,
          QStringLiteral("the local counts stay this desktop's own"));
    check(sum(log.slotsFor(day.addDays(-1))) == 0,
          QStringLiteral("other devices' counts belong to their day"));
    check(log.otherKindsFor(day) == QStringList({QStringLiteral("ios"), QStringLiteral("web")}),
          QStringLiteral("the kinds that contributed"));
    log.setOthers(day, QVector<int>(3, 1), {});
    check(log.today().at(8 * 12) == 5, QStringLiteral("a short answer is ignored"));
    log.clearOthers();
    check(sum(log.today()) == 1 && log.otherKindsFor(day).isEmpty(),
          QStringLiteral("sign-out drops the other devices"));

    check(activitySourcesText(QStringLiteral("desktop"), {}) == QStringLiteral("this desktop"),
          QStringLiteral("sources text alone"));
    check(activitySourcesText(QStringLiteral("desktop"),
                              {QStringLiteral("ios"), QStringLiteral("web"),
                               QStringLiteral("ios")}) ==
              QStringLiteral("this desktop, iOS device and browser"),
          QStringLiteral("sources text lists each kind once"));
    check(activitySourcesText(QStringLiteral("desktop"), {QStringLiteral("desktop")}) ==
              QStringLiteral("this desktop and another desktop"),
          QStringLiteral("sources text names a second desktop"));
}

static void testCountsRealInput()
{
    UserActivityLog log{QString()};
    qApp->installEventFilter(&log);
    QWidget window;
    auto *layout = new QVBoxLayout(&window);
    auto *edit = new QLineEdit(&window);
    layout->addWidget(edit);
    window.resize(240, 120);
    window.show();
    check(QTest::qWaitForWindowExposed(&window), QStringLiteral("window shown"));
    QWindow *handle = window.windowHandle();
    const QPoint inside = edit->mapTo(&window, edit->rect().center());

    const auto counted = [&] { return sum(log.today()); };
    const int before = counted();
    QTest::mouseClick(handle, Qt::LeftButton, Qt::NoModifier, inside);
    check(counted() == before + 1,
          QStringLiteral("one click on a child widget counts once (got %1)")
              .arg(counted() - before));
    QTest::mouseMove(handle, inside + QPoint(3, 3));
    check(counted() == before + 1, QStringLiteral("a mouse move is not counted"));
    edit->setFocus();
    QTest::keyClick(handle, Qt::Key_A);
    QTest::keyClick(handle, Qt::Key_B);
    check(counted() == before + 3,
          QStringLiteral("each key press counts once (got %1)")
              .arg(counted() - before));
    QTest::mouseDClick(handle, Qt::LeftButton, Qt::NoModifier, inside);
    check(counted() == before + 5,
          QStringLiteral("a double click is two presses (got %1)")
              .arg(counted() - before));
    QTest::mouseClick(edit, Qt::LeftButton);
    check(counted() == before + 5,
          QStringLiteral("a synthetic widget event is not user input"));
    qApp->removeEventFilter(&log);
}

static void testSummaryAndLevels()
{
    QVector<int> counts(kActivitySlots, 0);
    counts[10 * 12 + 2] = 8;
    counts[10 * 12 + 3] = 4;
    counts[15 * 12] = 10;
    const ActivitySummary summary = summarizeActivity(counts);
    check(summary.total == 22, QStringLiteral("total"));
    check(summary.peak == 10 && summary.peakSlot == 15 * 12,
          QStringLiteral("busiest five minutes"));
    check(summary.peakHour == 10 && summary.peakHourTotal == 12,
          QStringLiteral("busiest hour"));
    check(summary.activeSlots == 3, QStringLiteral("active counts"));
    check(summarizeActivity({}).peakSlot == -1, QStringLiteral("empty summary"));

    check(activityLevel(0, 10) == 0, QStringLiteral("zero is level 0"));
    check(activityLevel(1, 100) == 1, QStringLiteral("any activity is level 1"));
    check(activityLevel(50, 100) == 2, QStringLiteral("half is level 2"));
    check(activityLevel(100, 100) == 4, QStringLiteral("the peak is level 4"));
}

static void testImage()
{
    QVector<int> counts(kActivitySlots, 0);
    counts[3 * 12 + 5] = 9;
    for (bool dark : {false, true}) {
        const QImage image = activityHeatImage(counts, 20 * 12, dark);
        check(image.size() == activityHeatSize(), QStringLiteral("image size"));
        const auto centre = [](int hour, int row) {
            return QPoint(kActivityLabelWidth + hour * kActivityCellPitch +
                              kActivityCellPitch / 2,
                          row * kActivityCellPitch + kActivityCellPitch / 2);
        };
        check(image.pixelColor(centre(3, 5)) == activityLevelColor(4, dark),
              QStringLiteral("the busiest cell is the darkest green"));
        check(image.pixelColor(centre(0, 0)) == activityLevelColor(0, dark),
              QStringLiteral("an idle past cell is the empty colour"));
        check(image.pixelColor(centre(23, 11)).alpha() < 255,
              QStringLiteral("a future cell is faded"));
    }
    check(activityHeatSize().width() <= 720,
          QStringLiteral("fits beside its stats in the certificate width"));
}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ForkMeshTests"));
    app.setApplicationName(QStringLiteral("forkmesh-user-activity-tests"));
    check(UserActivityLog::defaultPath().isEmpty(),
          QStringLiteral("tests never write the real activity file"));
    testSlots();
    testRecordAndRollover();
    testPersistence();
    testDeviceKeyAndOthers();
    testCountsRealInput();
    testSummaryAndLevels();
    testImage();
    if (failures == 0)
        std::cout << "test_user_activity: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
