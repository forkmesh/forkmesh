#include "SurveysPage.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QTabWidget>
#include <QTimer>

#include <cstdio>
#include <functional>

using namespace forkmesh::surveys;

namespace {

int pageFailures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ", detail.toUtf8().constData());
        ++pageFailures;
    }
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 4000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs)
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    return condition();
}

QJsonObject object(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

const char kDetail[] = R"({"id": "a", "organization": "acme", "title": "Team offsite feedback", "status": "open",
    "displayStatus": "open", "settings": {}, "questionCount": 2, "responseCount": 3, "publicUrl": "",
    "questions": [
        {"id": "a1b2c3d4", "type": "single", "title": "How was it?", "options": ["Great", "Fine"], "scale": 0},
        {"id": "d1b2c3d4", "type": "paragraph", "title": "Anything else?"}],
    "results": [
        {"id": "a1b2c3d4", "type": "single", "title": "How was it?", "answered": 3, "skipped": 0,
         "options": [{"label": "Great", "count": 2}, {"label": "Fine", "count": 1}]},
        {"id": "d1b2c3d4", "type": "paragraph", "title": "Anything else?", "answered": 2, "skipped": 1,
         "answers": ["More coffee", "Great talks"]}]})";

bool tabFilled(QTabWidget *tabs, int index)
{
    return tabs && tabs->widget(index) && tabs->widget(index)->property("surveysTabFilled").toBool();
}

}

int surveysPageFailures()
{
    QStringList requested;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("surveys");
    host.request = [&requested](const QByteArray &method, const QString &path, const QJsonObject &,
                                forkmesh::market::NativePackageReplyHandler done) {
        requested << QString::fromLatin1(method) + QLatin1Char(' ') + path;
        QJsonObject payload;
        bool ok = method == "GET";
        if (path == QLatin1String("/api/surveys"))
            payload = object(R"({"organizations": [{"name": "acme", "role": "owner", "installed": true}]})");
        else if (path == QLatin1String("/api/surveys/acme"))
            payload = QJsonObject{{QStringLiteral("surveys"), QJsonArray{object(kDetail)}},
                                  {QStringLiteral("summary"), QJsonObject()}};
        else if (path == QLatin1String("/api/surveys/acme/items/a"))
            payload = QJsonObject{{QStringLiteral("survey"), object(kDetail)}};
        else if (path == QLatin1String("/api/surveys/acme/items/a/responses"))
            payload = object(R"({"responses": []})");
        else
            ok = false;
        QTimer::singleShot(0, [done, ok, payload] { done(ok, payload, ok ? QString() : QStringLiteral("not found")); });
    };

    SurveysPage page(host);
    page.resize(1100, 800);
    page.show();
    page.refresh();
    check(waitFor([&] { return requested.contains(QStringLiteral("GET /api/surveys/acme")); }),
          "the page loads the organization's surveys", requested.join(QStringLiteral(", ")));
    QApplication::processEvents();
    page.activateSearchHit(QStringLiteral("a"));
    QTabWidget *tabs = nullptr;
    check(waitFor([&] {
              tabs = page.findChild<QTabWidget *>(QStringLiteral("surveysDetailTabs"));
              return tabs != nullptr && tabs->count() == 3;
          }),
          "opening a survey shows its Results, Responses and Activity tabs");
    if (!tabs)
        return pageFailures;
    check(tabs->currentIndex() == 0 && tabFilled(tabs, 0) && !tabFilled(tabs, 1) && !tabFilled(tabs, 2),
          "only the shown tab is built, so the detail does not lay out every tab's text");
    tabs->setCurrentIndex(2);
    check(tabFilled(tabs, 2) && !tabFilled(tabs, 1), "a tab is built the first time it is shown");
    tabs->setCurrentIndex(1);
    check(waitFor([&] {
              tabs = page.findChild<QTabWidget *>(QStringLiteral("surveysDetailTabs"));
              return requested.contains(QStringLiteral("GET /api/surveys/acme/items/a/responses")) && tabs &&
                     tabs->currentIndex() == 1 && tabFilled(tabs, 1);
          }),
          "the Responses tab loads its answers and stays open after the detail redraws");
    return pageFailures;
}
