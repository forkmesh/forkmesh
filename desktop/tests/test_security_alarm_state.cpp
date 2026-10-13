#include "SavedSearchStore.h"
#include "SecurityAlarmState.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

QMap<QString, int> issues(int count)
{
    QMap<QString, int> out;
    if (count > 0)
        out.insert(QStringLiteral("issue"), count);
    return out;
}

QString key(int number)
{
    return SavedSearchStore::resultKey(QStringLiteral("issue"), QString::number(number));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::cerr << "FAIL: temporary directory is available\n";
        return 1;
    }
    SavedSearchStore store(QDir(dir.path()).filePath(QStringLiteral("saved.json")));
    const QString pertinent =
        store.add(QStringLiteral("CVE-"), QString(), false, 1000).id;
    const QString plain =
        store.add(QStringLiteral("flaky"), QString(), true, 1000).id;
    check(store.setSecurityPertinent(pertinent, true) &&
              !store.setSecurityPertinent(pertinent, true) &&
              store.hasSecurityPertinent(),
          "the security flag toggles and reports changes");

    SecurityAlarmState state;
    SecurityAlarmUpdate update = state.update(store.searches());
    check(update.active.isEmpty() && !state.isActive(),
          "a search that never ran raises no alarm");

    store.recordRun(plain, {key(1), key(2)}, issues(2), 2000, false);
    update = state.update(store.searches());
    check(update.active.isEmpty(),
          "results on a search that is not security pertinent never alarm");

    store.recordRun(pertinent, {}, issues(0), 2000, false);
    update = state.update(store.searches());
    check(update.active.isEmpty(), "a pertinent search with no results is quiet");

    store.recordRun(pertinent, {key(7)}, issues(1), 3000, false);
    update = state.update(store.searches());
    check(update.active.size() == 1 && update.raised.size() == 1 &&
              update.active.first().id == pertinent &&
              update.active.first().results == 1 && update.changed,
          "a pertinent search with a result raises its alarm");
    check(SecurityAlarmState::summary(update.active).contains(QStringLiteral("CVE-")) &&
              SecurityAlarmState::summary(update.active)
                  .contains(QStringLiteral("1 result in all repositories")),
          "the summary names the search and its count");

    update = state.update(store.searches());
    check(update.active.size() == 1 && update.raised.isEmpty() && !update.changed,
          "an alarm already raised is not raised twice");

    check(store.acknowledgeSecurityAlarm(pertinent) &&
              !store.acknowledgeSecurityAlarm(pertinent),
          "acknowledging records the result fingerprint once");
    update = state.update(store.searches());
    check(update.active.isEmpty() && update.cleared == QStringList{pertinent},
          "acknowledging silences the alarm");

    store.recordRun(pertinent, {key(7)}, issues(1), 4000, false);
    check(state.update(store.searches()).active.isEmpty(),
          "the same results stay acknowledged");

    store.recordRun(pertinent, {key(7), key(8)}, issues(2), 5000, false);
    update = state.update(store.searches());
    check(update.active.size() == 1 && update.raised.size() == 1 &&
              update.active.first().results == 2,
          "a new result re-raises an acknowledged alarm");

    store.acknowledgeSecurityAlarm(pertinent);
    store.recordRun(pertinent, {key(8)}, issues(1), 6000, false);
    check(state.update(store.searches()).active.isEmpty(),
          "results dropping out keep the acknowledgement");

    store.recordRun(pertinent, {key(8), key(7)}, issues(2), 6500, false);
    check(state.update(store.searches()).active.isEmpty(),
          "an acknowledged result that comes back stays acknowledged");

    store.recordRun(pertinent, {key(9)}, issues(1), 7000, false);
    check(state.update(store.searches()).active.size() == 1,
          "a never-seen result alarms even when the count does not grow");

    check(store.setSecurityPertinent(pertinent, false), "the flag turns off");
    update = state.update(store.searches());
    check(update.active.isEmpty() && update.cleared == QStringList{pertinent},
          "unchecking security pertinent stops the alarm");
    store.setSecurityPertinent(pertinent, true);
    check(state.update(store.searches()).active.size() == 1,
          "checking it again with unacknowledged results alarms again");

    store.recordRun(pertinent, {}, issues(0), 8000, false);
    update = state.update(store.searches());
    check(update.active.isEmpty() && update.cleared == QStringList{pertinent},
          "the alarm stops when the results drop to zero");

    store.recordRun(pertinent, {key(9)}, issues(1), 9000, false);
    check(state.update(store.searches()).active.size() == 1,
          "results returning after zero alarm again");

    const QString second =
        store.add(QStringLiteral("leaked token"), QStringLiteral("me/app"), false, 9500).id;
    store.setSecurityPertinent(second, true);
    store.recordRun(second, {key(1), key(2), key(3)}, issues(3), 9600, false);
    update = state.update(store.searches());
    check(update.active.size() == 2 && update.raised.size() == 1 &&
              update.raised.first().id == second,
          "each pertinent search alarms on its own");
    check(SecurityAlarmState::summary(update.active)
              .contains(QStringLiteral("and 1 more security-pertinent search")),
          "the summary counts the other alarms");

    if (failures == 0)
        std::cout << "security alarm state tests passed\n";
    return failures == 0 ? 0 : 1;
}
