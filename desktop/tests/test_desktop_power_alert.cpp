#include "DesktopPowerAlert.h"

#include <iostream>

using namespace forkmesh::poweralert;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static Supply mains(const char *online)
{
    return {QStringLiteral("Mains"), QString(), QString::fromLatin1(online), QString(), QString()};
}

static Supply battery(const char *status, const char *capacity, const char *scope = "")
{
    return {QStringLiteral("Battery"), QString::fromLatin1(scope), QString(),
            QString::fromLatin1(status), QString::fromLatin1(capacity)};
}

static void checkSupplies()
{
    check(fromSupplies({}).source == Source::Unknown, "no supplies is unknown");
    check(fromSupplies({mains("1")}).source == Source::Mains, "an online adapter is mains");
    check(fromSupplies({mains("0"), battery("Discharging", "80")}).source == Source::Battery,
          "an offline adapter with a discharging battery is battery");
    check(fromSupplies({mains("0"), battery("Discharging", "80")}).percent == 80,
          "the battery capacity is the percent");
    check(fromSupplies({mains("1"), battery("Discharging", "80")}).source == Source::Mains,
          "an online adapter wins over a lagging battery status");
    check(fromSupplies({mains("0"), battery("Not charging", "")}).source == Source::Battery,
          "an offline adapter beside a system battery is battery");
    check(fromSupplies({battery("Discharging", "40")}).source == Source::Battery,
          "a discharging battery with no adapter listed is battery");
    check(fromSupplies({battery("Charging", "40")}).source == Source::Mains,
          "a charging battery is mains");
    check(fromSupplies({battery("Discharging", "30", "Device")}).source == Source::Unknown,
          "a mouse battery says nothing about the desktop");
    Supply usb{QStringLiteral("USB"), QString(), QStringLiteral("1"), QString(), QString()};
    check(fromSupplies({usb, battery("Charging", "50")}).source == Source::Mains,
          "a USB-C charger counts as mains");
}

static void checkUPower()
{
    const Reading sysfs = fromSupplies({mains("1")});
    check(combine(-1, sysfs).source == Source::Mains, "no UPower keeps sysfs");
    check(combine(1, sysfs).source == Source::Battery, "UPower on battery covers a UPS");
    check(combine(0, fromSupplies({})).source == Source::Mains, "UPower off battery is mains");
}

static void checkWatch()
{
    Watch fresh;
    check(fresh.observe(Source::Battery, 0) == Change::None, "starting on battery does not alarm");
    check(fresh.observe(Source::Battery, kConfirmMs * 4) == Change::None,
          "battery since launch never alarms");

    Watch watch;
    check(watch.observe(Source::Mains, 1000) == Change::None, "mains is quiet");
    check(watch.observe(Source::Battery, 2000) == Change::None, "a fresh switch waits");
    check(watch.observe(Source::Mains, 6000) == Change::None, "a flicker never alarms");
    check(watch.observe(Source::Battery, 10000) == Change::None, "the wait restarts");
    check(watch.observe(Source::Unknown, 20000) == Change::None, "unknown changes nothing");
    check(watch.observe(Source::Battery, 10000 + kConfirmMs) == Change::LostPower,
          "battery for the confirm window alarms");
    check(watch.observe(Source::Battery, 60000) == Change::None, "the alarm rings once");
    check(watch.observe(Source::Mains, 10000 + 120000) == Change::Restored,
          "mains after an alarm is restored");
    check(watch.lastOutageMs() == 120000, "the outage runs from the switch");
    check(watch.observe(Source::Mains, 200000) == Change::None, "restored is told once");
}

static void checkText()
{
    Reading reading;
    reading.source = Source::Battery;
    reading.percent = 63;
    const auto lost = lostSource(QStringLiteral("tower"), reading, 5);
    check(lost.kind == lostKind(), "the lost ping has its kind");
    check(lost.warning, "losing power is a warning");
    check(lost.title == QLatin1String("tower lost power"), "the title names the desktop");
    check(lost.body.contains(QLatin1String("63%")), "the body has the charge");
    const auto restored = restoredSource(QString(), 3 * 60000, 5);
    check(restored.kind == restoredKind(), "the restored ping has its kind");
    check(!restored.warning, "power coming back is not a warning");
    check(restored.title == QLatin1String("Your desktop is back on mains power"),
          "an unnamed desktop is your desktop");
    check(restored.body.contains(QLatin1String("3 min")), "the body says how long");
    check(isPowerKind(lostKind()) && isPowerKind(restoredKind()) &&
              !isPowerKind(QStringLiteral("agent-done")),
          "only the two power kinds are power kinds");
    const auto ping = forkmesh::pingforward::pingFor(lost);
    check(forkmesh::pingforward::valid(ping), "the lost ping forwards");
    check(forkmesh::pingforward::forwardsKind(lost.kind), "the power kind is forwarded");
}

int main()
{
    checkSupplies();
    checkUPower();
    checkWatch();
    checkText();
    if (failures == 0)
        std::cout << "desktop power alert tests passed\n";
    return failures == 0 ? 0 : 1;
}
