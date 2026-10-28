#include "PackageModules.h"
#include "WebEngine.h"

#include <QApplication>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <QWidget>

#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool engineMapped()
{
    QFile maps(QStringLiteral("/proc/self/maps"));
    if (!maps.open(QIODevice::ReadOnly))
        return false;
    return maps.readAll().contains("libQt6WebEngineCore");
}

}

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    namespace web = forkmesh::web;

    forkmesh::market::modules::install();
    check(web::built(), "the build has an engine");
    check(!web::loaded(), "starting the app does not load the engine");
    check(web::available(), "the module's sidecar makes the engine available");
    check(!engineMapped(), "Qt WebEngine is not mapped before a page opens");

    QObject *profile = web::createProfile(web::ProfileOptions());
    check(profile != nullptr, "the first profile loads the module");
    check(web::loaded(), "the engine registers itself when its module loads");
    check(engineMapped(), "Qt WebEngine is mapped once the module is loaded");
    if (!profile) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    QWidget host;
    host.resize(480, 320);
    host.show();
    web::PageOptions options;
    options.acceptNavigation = [](const QUrl &, bool, bool) { return true; };
    web::View *view = web::createView(profile, options, &host);
    check(view != nullptr, "the engine makes a view");
    if (view) {
        check(web::viewOf(view->widget()) == view, "the view is found from its widget");
        QEventLoop loop;
        bool loaded = false;
        QVariant answer;
        view->onLoadFinished(&host, [&](bool ok) {
            loaded = ok;
            view->runJavaScript(QStringLiteral("6 * 7"), web::World::Application,
                                [&](const QVariant &value) {
                                    answer = value;
                                    loop.quit();
                                });
        });
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        view->setHtml(QStringLiteral("<title>engine</title><p>hello</p>"),
                      QUrl(QStringLiteral("about:blank")));
        loop.exec();
        check(loaded, "a page loads in the module's view");
        check(answer.toInt() == 42, "scripts run in the module's view");
        check(view->title() == QLatin1String("engine"), "the view reports the page title");
        delete view->widget();
    }
    delete profile;

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("web engine module: all checks passed\n");
    return 0;
}
