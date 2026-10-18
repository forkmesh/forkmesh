#include "WorkflowsFeature.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>

using namespace forkmesh::workflows;

void workflowsCheck(bool condition, const QString &message);

namespace {

struct Call {
    QByteArray method;
    QString path;
    QJsonObject body;
};

QJsonObject catalogPayload()
{
    return QJsonDocument::fromJson(R"json({
      "ok": true,
      "core": [
        {"type": "core.manual", "name": "Manual trigger", "group": "Triggers", "trigger": true,
         "outputs": ["main"], "description": "Start with Run.", "fields": []},
        {"type": "core.http", "name": "HTTP request", "group": "Core", "outputs": ["main"],
         "description": "Call any HTTPS API.",
         "fields": [{"key": "method", "label": "Method", "type": "select", "default": "GET",
                     "options": [{"value": "GET", "label": "GET"}, {"value": "POST", "label": "POST"}]},
                    {"key": "url", "label": "URL", "type": "string"},
                    {"key": "body", "label": "Body", "type": "text"}]}
      ],
      "integrations": [],
      "operators": []
    })json").object();
}

QJsonObject workflowDetail(const QString &id, const QString &name, bool active)
{
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("active"), active},
        {QStringLiteral("trigger"), QStringLiteral("core.manual")},
        {QStringLiteral("hookPath"), QStringLiteral("/api/workflows/hook/") + id + QStringLiteral("/token")},
        {QStringLiteral("nodes"),
         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("trigger")},
                                {QStringLiteral("type"), QStringLiteral("core.manual")},
                                {QStringLiteral("name"), QStringLiteral("Run")},
                                {QStringLiteral("params"), QJsonObject()}},
                    QJsonObject{{QStringLiteral("id"), QStringLiteral("http1")},
                                {QStringLiteral("type"), QStringLiteral("core.http")},
                                {QStringLiteral("name"), QStringLiteral("Fetch")},
                                {QStringLiteral("params"),
                                 QJsonObject{{QStringLiteral("method"), QStringLiteral("GET")},
                                             {QStringLiteral("url"), QStringLiteral("https://api.example.com")}}}}}},
        {QStringLiteral("connections"),
         QJsonArray{QJsonObject{{QStringLiteral("from"), QStringLiteral("trigger")},
                                {QStringLiteral("output"), QStringLiteral("main")},
                                {QStringLiteral("to"), QStringLiteral("http1")}}}}};
}

class FakeServer
{
public:
    FakeServer()
    {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        workflows.insert(QStringLiteral("a1"), workflowDetail(QStringLiteral("a1"), QStringLiteral("Morning report"), true));
        workflows.insert(QStringLiteral("b2"), workflowDetail(QStringLiteral("b2"), QStringLiteral("Lead alert"), false));
        runs.insert(QStringLiteral("a1"),
                    QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("r1")},
                                           {QStringLiteral("trigger"), QStringLiteral("manual")},
                                           {QStringLiteral("status"), QStringLiteral("success")},
                                           {QStringLiteral("startedAt"), double(now - 4000)},
                                           {QStringLiteral("finishedAt"), double(now - 3000)},
                                           {QStringLiteral("output"), QJsonObject{{QStringLiteral("text"), QStringLiteral("1 item")}}}},
                               QJsonObject{{QStringLiteral("id"), QStringLiteral("r2")},
                                           {QStringLiteral("trigger"), QStringLiteral("manual")},
                                           {QStringLiteral("status"), QStringLiteral("error")},
                                           {QStringLiteral("startedAt"), double(now - 90000)},
                                           {QStringLiteral("finishedAt"), double(now - 89000)}}});
        lastRunAt = now - 4000;
    }

    QJsonObject summary(const QJsonObject &detail) const
    {
        QJsonObject out = detail;
        out.insert(QStringLiteral("nodes"), int(detail.value(QStringLiteral("nodes")).toArray().size()));
        out.remove(QStringLiteral("connections"));
        if (detail.value(QStringLiteral("id")).toString() == QLatin1String("a1")) {
            out.insert(QStringLiteral("lastRunAt"), double(lastRunAt));
            out.insert(QStringLiteral("lastStatus"), QStringLiteral("success"));
        }
        return out;
    }

    QJsonObject reply(const QByteArray &method, const QString &path, const QJsonObject &body, bool *ok)
    {
        calls.append({method, path, body});
        *ok = true;
        const QString prefix = QStringLiteral("/api/workflows");
        if (path == prefix + QStringLiteral("/catalog"))
            return catalogPayload();
        if (path == prefix + QStringLiteral("/credentials"))
            return QJsonObject{{QStringLiteral("credentials"), QJsonArray()}};
        if (path == prefix && method == "GET") {
            QJsonArray list;
            for (const QJsonObject &detail : std::as_const(workflows))
                list.append(summary(detail));
            return QJsonObject{{QStringLiteral("workflows"), list}};
        }
        const QStringList parts = path.mid(prefix.size() + 1).split(QLatin1Char('/'));
        const QString id = parts.value(0);
        if (!workflows.contains(id)) {
            *ok = false;
            return QJsonObject{{QStringLiteral("error"), QStringLiteral("not_found")}};
        }
        if (parts.size() == 1 && method == "GET")
            return QJsonObject{{QStringLiteral("workflow"), workflows.value(id)}};
        if (parts.size() == 1 && method == "POST") {
            QJsonObject updated = workflows.value(id);
            for (const QString &key : {QStringLiteral("name"), QStringLiteral("active"), QStringLiteral("nodes"),
                                       QStringLiteral("connections")})
                if (body.contains(key))
                    updated.insert(key, body.value(key));
            workflows.insert(id, updated);
            return QJsonObject{{QStringLiteral("workflow"), updated}};
        }
        if (parts.value(1) == QLatin1String("runs") && parts.size() == 2)
            return QJsonObject{{QStringLiteral("runs"), runs.value(id)}};
        if (parts.value(1) == QLatin1String("runs") && parts.size() == 3)
            return QJsonObject{{QStringLiteral("run"),
                                QJsonObject{{QStringLiteral("status"), QStringLiteral("success")},
                                            {QStringLiteral("nodes"),
                                             QJsonObject{{QStringLiteral("http1"),
                                                          QJsonObject{{QStringLiteral("status"), QStringLiteral("success")},
                                                                      {QStringLiteral("ms"), 12}}}}}}}};
        if (parts.value(1) == QLatin1String("run"))
            return QJsonObject{{QStringLiteral("run"),
                                QJsonObject{{QStringLiteral("id"), QStringLiteral("r9")},
                                            {QStringLiteral("status"), QStringLiteral("success")}}}};
        *ok = false;
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("not_found")}};
    }

    int count(const QByteArray &method, const QString &path) const
    {
        int found = 0;
        for (const Call &call : calls)
            found += call.method == method && call.path == path ? 1 : 0;
        return found;
    }

    QHash<QString, QJsonObject> workflows;
    QHash<QString, QJsonArray> runs;
    QList<Call> calls;
    qint64 lastRunAt = 0;
};

void settle()
{
    for (int round = 0; round < 20; ++round) {
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

template <typename T>
T *named(QWidget *root, const QString &name)
{
    for (T *child : root->findChildren<T *>())
        if (child->objectName() == name || child->accessibleName() == name)
            return child;
    return nullptr;
}

} // namespace

void workflowsFeatureChecks()
{
    FakeServer server;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("workflows");
    host.request = [&server](const QByteArray &method, const QString &path, const QJsonObject &body,
                             forkmesh::market::NativePackageReplyHandler handler) {
        bool ok = false;
        const QJsonObject payload = server.reply(method, path, body, &ok);
        QTimer::singleShot(0, [handler, ok, payload] {
            handler(ok, payload, ok ? QString() : payload.value(QStringLiteral("error")).toString());
        });
    };
    int badgeChanges = 0;
    host.navigationBadgeChanged = [&badgeChanges] { ++badgeChanges; };

    const forkmesh::market::NativePackageContribution *contribution =
        forkmesh::market::nativePackageContribution(QStringLiteral("builtin:workflows"),
                                                    QStringLiteral("extension.native"));
    workflowsCheck(contribution && contribution->factory && contribution->browserPath.isEmpty() &&
                       contribution->apiPrefix == QLatin1String("/api/workflows"),
                   QStringLiteral("Workflows registers a native desktop page instead of a browser link"));
    if (!contribution || !contribution->factory)
        return;
    auto *page = static_cast<WorkflowsFeature *>(contribution->factory(host, nullptr));
    page->resize(1280, 860);
    page->show();
    page->refresh();
    settle();

    auto *table = named<QTableWidget>(page, QStringLiteral("workflowsTable"));
    auto *runs = named<QTableWidget>(page, QStringLiteral("workflowsRuns"));
    QStringList kpis;
    for (QLabel *label : page->findChildren<QLabel *>())
        if (label->objectName() == QLatin1String("workflowsKpi"))
            kpis << label->accessibleName() + QLatin1Char('=') + label->text();
    workflowsCheck(table && table->rowCount() == 2, QStringLiteral("the dashboard lists every workflow"));
    workflowsCheck(runs && runs->rowCount() == 2, QStringLiteral("the dashboard lists the runs of every workflow"));
    workflowsCheck(kpis.contains(QStringLiteral("Workflows=2")) && kpis.contains(QStringLiteral("Active=1")) &&
                       kpis.contains(QStringLiteral("Runs=2")) && kpis.contains(QStringLiteral("Success rate=50%")),
                   QStringLiteral("the stats count workflows, active ones, runs and the success rate"));
    workflowsCheck(server.count("GET", QStringLiteral("/api/workflows/a1/runs")) == 1 &&
                       server.count("GET", QStringLiteral("/api/workflows/b2/runs")) == 0,
                   QStringLiteral("runs are fetched only for workflows that have run"));
    workflowsCheck(badgeChanges > 0, QStringLiteral("the rail badge is told when the list changes"));
    workflowsCheck(page->searchHits(QStringLiteral("lead"), 5).size() == 1,
                   QStringLiteral("the top search box finds workflows by name"));

    runs->selectRow(0);
    settle();
    auto *steps = named<QTableWidget>(page, QStringLiteral("workflowsRunSteps"));
    workflowsCheck(steps && steps->rowCount() == 1 && steps->item(0, 0)->text() == QLatin1String("Fetch"),
                   QStringLiteral("a selected run shows each step by name"));

    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->text() == QLatin1String("Lead alert"))
            table->selectRow(row);
    named<QPushButton>(page, QStringLiteral("Turn on"))->click();
    settle();
    workflowsCheck(server.workflows.value(QStringLiteral("b2")).value(QStringLiteral("active")).toBool(),
                   QStringLiteral("Turn on saves the whole workflow with Active set"));

    for (int row = 0; row < table->rowCount(); ++row)
        if (table->item(row, 0)->text() == QLatin1String("Morning report"))
            table->selectRow(row);
    named<QPushButton>(page, QStringLiteral("Run"))->click();
    settle();
    workflowsCheck(server.count("POST", QStringLiteral("/api/workflows/a1/run")) == 1,
                   QStringLiteral("Run starts the workflow on the server"));

    page->activateSearchHit(QStringLiteral("a1"));
    settle();
    auto *pages = page->findChild<QStackedWidget *>();
    auto *list = named<QListWidget>(page, QStringLiteral("workflowsSteps"));
    workflowsCheck(pages && pages->currentIndex() == 1 && list && list->count() == 2,
                   QStringLiteral("opening a workflow shows its steps in run order"));
    workflowsCheck(!named<QLineEdit>(page, QStringLiteral("URL")),
                   QStringLiteral("a workflow opens on its trigger"));
    auto *name = named<QLineEdit>(page, QStringLiteral("Workflow name"));
    name->setText(QStringLiteral("Evening report"));
    Q_EMIT name->textEdited(name->text());
    list->setCurrentRow(1);
    settle();
    auto *url = named<QLineEdit>(page, QStringLiteral("URL"));
    workflowsCheck(url && url->text() == QLatin1String("https://api.example.com"),
                   QStringLiteral("the step form shows the selected step's settings"));
    url->setText(QStringLiteral("https://api.example.com/v2"));
    Q_EMIT url->textEdited(url->text());
    named<QPushButton>(page, QStringLiteral("Save"))->click();
    settle();
    const QJsonObject saved = server.workflows.value(QStringLiteral("a1"));
    QString savedUrl;
    for (const QJsonValue &value : saved.value(QStringLiteral("nodes")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == QLatin1String("http1"))
            savedUrl = value.toObject().value(QStringLiteral("params")).toObject().value(QStringLiteral("url")).toString();
    workflowsCheck(saved.value(QStringLiteral("name")).toString() == QLatin1String("Evening report") &&
                       savedUrl == QLatin1String("https://api.example.com/v2"),
                   QStringLiteral("Save sends the edited name and step settings"));
    named<QPushButton>(page, QStringLiteral("‹ All workflows"))->click();
    settle();
    workflowsCheck(pages->currentIndex() == 0, QStringLiteral("a saved workflow returns to the dashboard without asking"));
    delete page;
}
