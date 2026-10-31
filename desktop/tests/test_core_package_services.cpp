#include "CorePackageServices.h"

#include <QSet>

#include <cstdio>

using forkmesh::CorePackageServices;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

}

int main()
{
    {
        CorePackageServices services;
        QStringList events;
        services.add(QStringLiteral("watch"), {QStringLiteral("agents"), QStringLiteral("desktops")},
                     [&] { events << QStringLiteral("start watch"); },
                     [&] { events << QStringLiteral("stop watch"); });
        services.add(QStringLiteral("feeds"), {QStringLiteral("logs")},
                     [&] { events << QStringLiteral("start feeds"); },
                     [&] { events << QStringLiteral("stop feeds"); });

        check(services.running().isEmpty(), "nothing runs before the first sync");

        QSet<QString> installed;
        const auto check_installed = [&](const QString &id) { return installed.contains(id); };

        services.sync(check_installed);
        check(events.isEmpty(), "an empty shell starts no service");
        check(!services.isRunning(QStringLiteral("watch")), "watch stays off with nothing installed");

        installed.insert(QStringLiteral("desktops"));
        services.sync(check_installed);
        check(events == QStringList{QStringLiteral("start watch")},
              "any one of a service's packages starts it");
        check(services.isRunning(QStringLiteral("watch")), "watch runs once desktops is installed");

        installed.insert(QStringLiteral("agents"));
        services.sync(check_installed);
        check(events.size() == 1, "a second package does not start a running service again");

        installed.remove(QStringLiteral("desktops"));
        services.sync(check_installed);
        check(events.size() == 1, "the service keeps running while one package remains");

        installed.remove(QStringLiteral("agents"));
        services.sync(check_installed);
        check(events.last() == QStringLiteral("stop watch"),
              "removing the last package stops the service");
        check(!services.isRunning(QStringLiteral("watch")), "watch is off after removal");

        installed.insert(QStringLiteral("logs"));
        installed.insert(QStringLiteral("agents"));
        services.sync(check_installed);
        check(services.running() ==
                  QStringList({QStringLiteral("watch"), QStringLiteral("feeds")}),
              "running lists services in registration order");
        events.clear();
        services.stopAll();
        check(events == QStringList({QStringLiteral("stop feeds"), QStringLiteral("stop watch")}),
              "stopAll stops services in reverse order");
        check(services.running().isEmpty(), "nothing runs after stopAll");
    }

    {
        CorePackageServices services;
        int starts = 0;
        services.add(QStringLiteral("one"), {QStringLiteral("a")}, [&] { ++starts; }, {});
        services.add(QStringLiteral("one"), {QStringLiteral("b")}, [&] { starts += 100; }, {});
        check(services.services() == QStringList{QStringLiteral("one")},
              "a duplicate service id is ignored");
        check(services.packagesFor(QStringLiteral("one")) == QStringList{QStringLiteral("a")},
              "the first registration keeps its packages");
        services.sync([](const QString &id) { return id == QLatin1String("a"); });
        check(starts == 1, "only the first registration's hook runs");
        services.add(QString(), {QStringLiteral("a")}, {}, {});
        check(services.services().size() == 1, "an empty id is ignored");
    }

    {
        CorePackageServices services;
        QSet<QString> installed{QStringLiteral("a")};
        const auto check_installed = [&](const QString &id) { return installed.contains(id); };
        int startsB = 0;
        services.add(QStringLiteral("first"), {QStringLiteral("a")},
                     [&] {
                         installed.insert(QStringLiteral("b"));
                         services.sync(check_installed);
                     },
                     {});
        services.add(QStringLiteral("second"), {QStringLiteral("b")}, [&] { ++startsB; }, {});
        services.sync(check_installed);
        check(services.isRunning(QStringLiteral("first")), "the outer sync starts its service");
        check(services.isRunning(QStringLiteral("second")),
              "a sync asked for from inside a hook is applied");
        check(startsB == 1, "a nested sync does not start a service twice");
    }

    {
        CorePackageServices services;
        bool started = false;
        services.add(QStringLiteral("x"), {QStringLiteral("a")}, [&] { started = true; }, {});
        services.sync({});
        check(!started, "a sync without an installed check does nothing");
        check(!services.isKnown(QStringLiteral("y")), "an unknown service is not known");
        check(!services.isRunning(QStringLiteral("y")), "an unknown service is not running");
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("core package services: all checks passed\n");
    return 0;
}
