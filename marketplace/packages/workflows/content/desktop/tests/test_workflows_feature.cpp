#include "WorkflowsFeature.h"
#include "WorkflowsRunFlow.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>

#include <functional>

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
        if (parts.value(1) == QLatin1String("run")) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const QJsonObject run{{QStringLiteral("id"), QStringLiteral("r%1").arg(9 + runsStarted++)},
                                  {QStringLiteral("trigger"), QStringLiteral("manual")},
                                  {QStringLiteral("status"), QStringLiteral("success")},
                                  {QStringLiteral("startedAt"), double(now)},
                                  {QStringLiteral("finishedAt"), double(now + 5)}};
            QJsonArray list = runs.value(id);
            list.prepend(run);
            runs.insert(id, list);
            lastRunAt = now;
            return QJsonObject{{QStringLiteral("run"), run}};
        }
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
    int runsStarted = 0;
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

QList<WorkflowCard *> cardsOf(QWidget *page)
{
    QList<WorkflowCard *> cards;
    for (QFrame *frame : page->findChildren<QFrame *>(QStringLiteral("workflowCard")))
        if (auto *card = dynamic_cast<WorkflowCard *>(frame); card && card->isVisibleTo(page))
            cards.append(card);
    return cards;
}

WorkflowCard *cardNamed(QWidget *page, const QString &name)
{
    for (WorkflowCard *card : cardsOf(page))
        if (card->property("workflowName").toString() == name)
            return card;
    return nullptr;
}

QAbstractButton *control(WorkflowCard *card, const QString &name)
{
    return card ? named<QAbstractButton>(card, name) : nullptr;
}

void pressCard(WorkflowCard *card)
{
    if (!card)
        return;
    const QPointF at(4, 4);
    QMouseEvent press(QEvent::MouseButtonPress, at, card->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(card, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, card->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(card, &release);
    settle();
}

QString detailsName(QWidget *page)
{
    auto *name = named<QLabel>(page, QStringLiteral("workflowsDetailsName"));
    return name && name->isVisibleTo(page) ? name->text() : QString();
}

} // namespace

void workflowsFeatureChecks()
{
    {
        RunFlow lone;
        lone.resize(420, lone.sizeHint().height());
        lone.show();
        lone.setSteps(QStringLiteral("pending"),
                      {{QStringLiteral("A"), QStringLiteral("queued")},
                       {QStringLiteral("B"), QStringLiteral("queued")},
                       {QStringLiteral("C"), QStringLiteral("queued")}},
                      true);
        lone.setSteps(QStringLiteral("run"),
                      {{QStringLiteral("A"), QStringLiteral("success")},
                       {QStringLiteral("B"), QStringLiteral("success")},
                       {QStringLiteral("C"), QStringLiteral("error")}},
                      false, QStringLiteral("pending"));
        workflowsCheck(lone.shownStatus(0) == QLatin1String("success") &&
                           lone.shownStatus(2) == QLatin1String("queued") && lone.animating(),
                       QStringLiteral("finished steps turn one after another, not all at once"));
        lone.setSteps(QStringLiteral("other"), {{QStringLiteral("A"), QStringLiteral("error")}}, false);
        workflowsCheck(lone.steps().size() == 1 && lone.shownStatus(0) == QLatin1String("error"),
                       QStringLiteral("another run's steps replace the old ones without a turn"));
    }

    FakeServer server;
    bool holdRuns = false;
    QList<std::function<void()>> heldRuns;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("workflows");
    host.request = [&server, &holdRuns, &heldRuns](const QByteArray &method, const QString &path,
                                                   const QJsonObject &body,
                                                   forkmesh::market::NativePackageReplyHandler handler) {
        bool ok = false;
        const QJsonObject payload = server.reply(method, path, body, &ok);
        const auto deliver = [handler, ok, payload] {
            handler(ok, payload, ok ? QString() : payload.value(QStringLiteral("error")).toString());
        };
        if (holdRuns && path.endsWith(QLatin1String("/run"))) {
            heldRuns.append(deliver);
            return;
        }
        QTimer::singleShot(0, deliver);
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

    QSettings().remove(QStringLiteral("workflows/viewMode"));
    auto *runs = named<QTableWidget>(page, QStringLiteral("workflowsRuns"));
    QStringList kpis;
    for (QLabel *label : page->findChildren<QLabel *>())
        if (label->objectName() == QLatin1String("workflowsKpi"))
            kpis << label->accessibleName() + QLatin1Char('=') + label->text();
    workflowsCheck(cardsOf(page).size() == 2 && cardNamed(page, QStringLiteral("Lead alert")) &&
                       cardNamed(page, QStringLiteral("Morning report"))
                               ->entry()
                               .value(QStringLiteral("lastOutput"))
                               .toString() == QLatin1String("1 item"),
                   QStringLiteral("the dashboard shows every workflow as a card with its latest output"));
    workflowsCheck(!control(cardNamed(page, QStringLiteral("Lead alert")), QStringLiteral("Pin to rail")),
                   QStringLiteral("without the desktop's workflows a card offers no rail pin"));
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

    auto *turnOn = control(cardNamed(page, QStringLiteral("Lead alert")), QStringLiteral("Turn on"));
    workflowsCheck(turnOn != nullptr, QStringLiteral("an off workflow's card has a Turn on switch"));
    if (turnOn)
        turnOn->click();
    settle();
    workflowsCheck(server.workflows.value(QStringLiteral("b2")).value(QStringLiteral("active")).toBool(),
                   QStringLiteral("Turn on saves the whole workflow with Active set"));

    holdRuns = true;
    if (auto *run = control(cardNamed(page, QStringLiteral("Morning report")), QStringLiteral("Run")))
        run->click();
    settle();
    workflowsCheck(server.count("POST", QStringLiteral("/api/workflows/a1/run")) == 1,
                   QStringLiteral("Run starts the workflow on the server"));
    auto *runOutput = named<QPlainTextEdit>(page, QStringLiteral("workflowsRunOutput"));
    auto *flow = dynamic_cast<RunFlow *>(named<QWidget>(page, QStringLiteral("workflowsRunFlow")));
    workflowsCheck(runs->currentRow() == 0 &&
                       runs->item(0, 0)->data(Qt::UserRole + 1).toString() == QLatin1String("pending:a1") &&
                       runs->item(0, 0)->text() == QLatin1String("Morning report"),
                   QStringLiteral("a started run is in Recent runs and selected before the server answers"));
    workflowsCheck(page->findChild<QWidget *>(QStringLiteral("workflowsRunSparkle")) != nullptr,
                   QStringLiteral("the new run sparkles into Recent runs"));
    workflowsCheck(flow && flow->isVisibleTo(page) && flow->live() && flow->animating() &&
                       flow->steps().size() == 2 && flow->steps().at(1).name == QLatin1String("Fetch") &&
                       flow->steps().at(1).status == QLatin1String("queued"),
                   QStringLiteral("the run's steps show at once beside Recent runs, animated while it runs"));
    workflowsCheck(runOutput && runOutput->toPlainText().contains(QLatin1String("Waiting for ForkMesh")),
                   QStringLiteral("the log under the steps says the run is under way"));
    holdRuns = false;
    for (const auto &deliver : std::as_const(heldRuns))
        deliver();
    heldRuns.clear();
    settle();
    workflowsCheck(flow && !flow->live() && flow->key() == QLatin1String("r9") && flow->steps().size() == 1 &&
                       flow->steps().first().status == QLatin1String("success") &&
                       flow->shownStatus(0) == QLatin1String("success"),
                   QStringLiteral("each step turns to its result when the run finishes"));
    workflowsCheck(runs->currentRow() >= 0 && !runs->selectedItems().isEmpty() &&
                       runs->item(runs->currentRow(), 0)->data(Qt::UserRole + 1).toString() == QLatin1String("r9"),
                   QStringLiteral("a run started on the page is selected in Recent runs"));
    workflowsCheck(runOutput && runOutput->toPlainText().contains(QLatin1String("Fetch")),
                   QStringLiteral("the selected run's output shows beside Recent runs"));
    page->activateSearchHit(QStringLiteral("run:a1"));
    settle();
    workflowsCheck(server.count("POST", QStringLiteral("/api/workflows/a1/run")) == 2,
                   QStringLiteral("a rail pin runs an account workflow through the page"));

    auto *details = named<QFrame>(page, QStringLiteral("Workflow details"));
    auto *runFilter = named<QComboBox>(page, QStringLiteral("Runs of workflow"));
    workflowsCheck(details && details->isHidden(), QStringLiteral("the details pane stays closed until a workflow is picked"));
    pressCard(cardNamed(page, QStringLiteral("Lead alert")));
    workflowsCheck(details && details->isVisible() && detailsName(page) == QLatin1String("Lead alert"),
                   QStringLiteral("clicking a workflow opens its details pane on the right"));
    workflowsCheck(runFilter && runFilter->currentData().toString() == QLatin1String("b2") && runs->rowCount() == 0 &&
                       runOutput->toPlainText().isEmpty(),
                   QStringLiteral("a workflow that never ran shows no runs and an empty log"));
    bool neverRanNote = false;
    for (QLabel *label : page->findChildren<QLabel *>())
        neverRanNote = neverRanNote || label->text().contains(QLatin1String("Lead alert has not run yet"));
    workflowsCheck(neverRanNote, QStringLiteral("the run detail says the picked workflow has not run yet"));
    pressCard(cardNamed(page, QStringLiteral("Morning report")));
    workflowsCheck(detailsName(page) == QLatin1String("Morning report") &&
                       runFilter->currentData().toString() == QLatin1String("a1") && runs->rowCount() >= 1 &&
                       !runs->selectedItems().isEmpty() && runOutput->toPlainText().contains(QLatin1String("Fetch")),
                   QStringLiteral("clicking a workflow lists its runs and shows its latest log at the bottom right"));
    workflowsCheck(named<QPushButton>(details, QStringLiteral("Duplicate")) &&
                       named<QPushButton>(details, QStringLiteral("Edit steps")) &&
                       named<QPushButton>(details, QStringLiteral("Turn off")) &&
                       named<QPushButton>(details, QStringLiteral("Delete…")) &&
                       named<QPushButton>(details, QStringLiteral("Run with an assignment…")),
                   QStringLiteral("the details pane offers every action on the workflow"));
    const int runsBefore = server.count("POST", QStringLiteral("/api/workflows/a1/run"));
    if (auto *paneRun = named<QAbstractButton>(details, QStringLiteral("Run now")))
        paneRun->click();
    settle();
    workflowsCheck(server.count("POST", QStringLiteral("/api/workflows/a1/run")) == runsBefore + 1 &&
                       runFilter->currentData().toString() == QLatin1String("a1"),
                   QStringLiteral("Run now in the details pane runs it and keeps its runs listed"));
    named<QAbstractButton>(details, QStringLiteral("Close details"))->click();
    settle();
    workflowsCheck(details->isHidden() && runFilter->currentIndex() == 0 && runs->rowCount() >= 2,
                   QStringLiteral("closing the details pane lists every workflow's runs again"));

    for (const QString &view : {QStringLiteral("List view"), QStringLiteral("Compact grid view"),
                                QStringLiteral("Thumbnails view")}) {
        named<QAbstractButton>(page, view)->click();
        settle();
        workflowsCheck(cardsOf(page).size() == 2 && control(cardNamed(page, QStringLiteral("Morning report")),
                                                            QStringLiteral("Run")),
                       QStringLiteral("%1 shows every workflow with its controls").arg(view));
    }
    named<QAbstractButton>(page, QStringLiteral("Compact grid view"))->click();
    settle();
    workflowsCheck(QSettings().value(QStringLiteral("workflows/viewMode")).toString() == QLatin1String("compact"),
                   QStringLiteral("the chosen view is remembered"));
    auto *filter = named<QLineEdit>(page, QStringLiteral("Filter workflows"));
    filter->setText(QStringLiteral("lead"));
    settle();
    workflowsCheck(cardsOf(page).size() == 1, QStringLiteral("the filter narrows the cards"));
    filter->clear();
    named<QAbstractButton>(page, QStringLiteral("Thumbnails view"))->click();
    settle();

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
    workflowsCheck(cardsOf(page).size() == 2, QStringLiteral("without the desktop's workflows the page shows only the account's"));
    delete page;

    struct LocalCall {
        QString repo;
        QString id;
        QString action;
        QJsonObject payload;
    };
    QList<LocalCall> localCalls;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject agentFile = QJsonDocument::fromJson(R"json({
      "kind": "forkmesh.workflow", "version": 1, "id": "sentinel", "name": "Sentinel", "icon": "sentinel",
      "enabled": true,
      "nodes": [{"id": "schedule", "type": "workflow.schedule", "name": "Schedule", "x": 0, "y": 0,
                 "params": {"mode": "weekdays"}},
                {"id": "agent", "type": "agent.run", "name": "Sentinel", "x": 288, "y": 0,
                 "params": {"specialty": "Security", "permissions": ["read-repositories"],
                            "skills": ["Threat models"]}}],
      "connections": [{"from": "schedule", "output": "main", "to": "agent"}]
    })json").object();
    QJsonObject localPayload{
        {QStringLiteral("catalog"), QJsonDocument::fromJson(R"json({
          "core": [
            {"type": "workflow.manual", "name": "Run by hand", "group": "Triggers", "trigger": true,
             "outputs": ["main"], "fields": []},
            {"type": "workflow.schedule", "name": "Schedule", "group": "Triggers", "trigger": true,
             "outputs": ["main"], "fields": [{"key": "mode", "label": "How often", "type": "select",
               "options": [{"value": "daily", "label": "Every day"}, {"value": "weekdays", "label": "Weekdays"}]}]},
            {"type": "agent.run", "name": "Special agent", "group": "Agents", "outputs": ["main"],
             "fields": [{"key": "avatar", "label": "Portrait", "type": "avatar"},
                        {"key": "specialty", "label": "Specialty", "type": "string"},
                        {"key": "permissions", "label": "Permissions", "type": "checks",
                         "options": [{"value": "read-repositories", "label": "Read repositories"},
                                     {"value": "modify-code", "label": "Modify code"}]},
                        {"key": "skills", "label": "Skill names (one per line)", "type": "list"},
                        {"key": "shortcut", "label": "Playbook shortcut", "type": "shortcut"}]},
            {"type": "workflow.shortcut", "name": "Run shortcut", "group": "Shortcuts", "outputs": ["main"],
             "fields": [{"key": "file", "label": "Shortcut file", "type": "shortcut"}]}
          ], "integrations": [], "operators": []
        })json").object()},
        {QStringLiteral("avatars"), QJsonArray{QJsonObject{{QStringLiteral("value"), QStringLiteral("sentinel")},
                                                           {QStringLiteral("label"), QStringLiteral("Sentinel")}}}},
        {QStringLiteral("repositories"),
         QJsonArray{QJsonObject{{QStringLiteral("key"), QStringLiteral("forkmesh/forkmesh")},
                                {QStringLiteral("title"), QStringLiteral("forkmesh/forkmesh")},
                                {QStringLiteral("shortcuts"),
                                 QJsonArray{QJsonObject{{QStringLiteral("file"), QStringLiteral("review-security.md")},
                                                        {QStringLiteral("name"), QStringLiteral("Review security")},
                                                        {QStringLiteral("kind"), QStringLiteral("prompt")}}}}}}},
        {QStringLiteral("workflows"),
         QJsonArray{QJsonObject{{QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
                                {QStringLiteral("id"), QStringLiteral("sentinel")},
                                {QStringLiteral("name"), QStringLiteral("Sentinel")},
                                {QStringLiteral("specialAgent"), true},
                                {QStringLiteral("enabled"), true},
                                {QStringLiteral("hasSchedule"), true},
                                {QStringLiteral("trigger"), QStringLiteral("Weekdays at 09:00 (off)")},
                                {QStringLiteral("lastStatus"), QStringLiteral("error")},
                                {QStringLiteral("lastRunAt"), double(now - 60000)},
                                {QStringLiteral("recentRuns"),
                                 QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("lr1")},
                                                        {QStringLiteral("workflowId"), QStringLiteral("sentinel")},
                                                        {QStringLiteral("status"), QStringLiteral("error")},
                                                        {QStringLiteral("startedAtMs"), double(now - 60000)},
                                                        {QStringLiteral("finishedAtMs"), double(now - 30000)},
                                                        {QStringLiteral("trigger"), QStringLiteral("By hand")},
                                                        {QStringLiteral("error"), QStringLiteral("Sentinel: tests failed")},
                                                        {QStringLiteral("outputFile"), QStringLiteral("/tmp/lr1.md")},
                                                        {QStringLiteral("steps"),
                                                         QJsonArray{QJsonObject{
                                                             {QStringLiteral("name"), QStringLiteral("Sentinel")},
                                                             {QStringLiteral("type"), QStringLiteral("agent.run")},
                                                             {QStringLiteral("status"), QStringLiteral("error")},
                                                             {QStringLiteral("sessionId"), 77},
                                                             {QStringLiteral("error"), QStringLiteral("tests failed")}}}}}}}},
                    QJsonObject{{QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
                                {QStringLiteral("id"), QStringLiteral("clean")},
                                {QStringLiteral("name"), QStringLiteral("Clean up")},
                                {QStringLiteral("specialAgent"), false},
                                {QStringLiteral("enabled"), true},
                                {QStringLiteral("trigger"), QStringLiteral("By hand")},
                                {QStringLiteral("pinned"), QStringLiteral("bottom")}}}},
        {QStringLiteral("pins"), QJsonArray()}};
    FakeServer second;
    forkmesh::market::NativePackageHost localHost = host;
    localHost.request = [&second](const QByteArray &method, const QString &path, const QJsonObject &body,
                                  forkmesh::market::NativePackageReplyHandler handler) {
        bool ok = false;
        const QJsonObject payload = second.reply(method, path, body, &ok);
        QTimer::singleShot(0, [handler, ok, payload] {
            handler(ok, payload, ok ? QString() : payload.value(QStringLiteral("error")).toString());
        });
    };
    localHost.repositoryWorkflows = [&localPayload] { return localPayload; };
    const auto cleanRun = [now](const QString &output) {
        return QJsonObject{{QStringLiteral("id"), QStringLiteral("lr2")},
                           {QStringLiteral("workflowId"), QStringLiteral("clean")},
                           {QStringLiteral("status"), QStringLiteral("running")},
                           {QStringLiteral("startedAtMs"), double(QDateTime::currentMSecsSinceEpoch())},
                           {QStringLiteral("trigger"), QStringLiteral("By hand · Workflows page")},
                           {QStringLiteral("steps"),
                            QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Clean caches")},
                                                   {QStringLiteral("type"), QStringLiteral("workflow.shortcut")},
                                                   {QStringLiteral("status"), QStringLiteral("running")},
                                                   {QStringLiteral("startedAtMs"), double(now)},
                                                   {QStringLiteral("output"), output}}}}};
    };
    bool sentinelApproved = false;
    bool allowSentinel = false;
    localHost.repositoryWorkflowAction = [&localCalls, &agentFile, &localPayload, &sentinelApproved, &allowSentinel,
                                          cleanRun](const QString &repo, const QString &id, const QString &action,
                                                    const QJsonObject &payload,
                                                    forkmesh::market::NativePackageReplyHandler done) {
        localCalls.append({repo, id, action, payload});
        if (id == QLatin1String("sentinel") && action == QLatin1String("review")) {
            sentinelApproved = allowSentinel;
            QTimer::singleShot(0, [done, approved = allowSentinel] {
                done(approved, {},
                     approved ? QStringLiteral("Approved Sentinel.")
                              : QStringLiteral("Sentinel did not run: it is not approved on this desktop."));
            });
            return;
        }
        if (id == QLatin1String("sentinel") && action == QLatin1String("run") && !sentinelApproved) {
            QTimer::singleShot(0, [done] {
                done(false, {{QStringLiteral("code"), QStringLiteral("approval_required")}},
                     QStringLiteral("Approve Sentinel on this desktop before it runs: use Review on the notice."));
            });
            return;
        }
        if (id == QLatin1String("sentinel") && action == QLatin1String("run")) {
            QJsonArray workflows = localPayload.value(QStringLiteral("workflows")).toArray();
            QJsonObject sentinel = workflows.at(0).toObject();
            QJsonArray recent = sentinel.value(QStringLiteral("recentRuns")).toArray();
            recent.prepend(QJsonObject{{QStringLiteral("id"), QStringLiteral("lr3")},
                                       {QStringLiteral("workflowId"), QStringLiteral("sentinel")},
                                       {QStringLiteral("status"), QStringLiteral("running")},
                                       {QStringLiteral("startedAtMs"), double(QDateTime::currentMSecsSinceEpoch())},
                                       {QStringLiteral("trigger"), QStringLiteral("By hand · Workflows page")},
                                       {QStringLiteral("steps"),
                                        QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Sentinel")},
                                                               {QStringLiteral("type"), QStringLiteral("agent.run")},
                                                               {QStringLiteral("status"), QStringLiteral("running")},
                                                               {QStringLiteral("output"),
                                                                QStringLiteral("Reading open issues")}}}}});
            sentinel.insert(QStringLiteral("running"), true);
            sentinel.insert(QStringLiteral("recentRuns"), recent);
            workflows.replace(0, sentinel);
            localPayload.insert(QStringLiteral("workflows"), workflows);
            QTimer::singleShot(0, [done] { done(true, {{QStringLiteral("runId"), QStringLiteral("lr3")}}, QString()); });
            return;
        }
        QJsonObject reply;
        if (action == QLatin1String("get") && id == QLatin1String("clean"))
            reply = QJsonObject{{QStringLiteral("workflow"), agentFile},
                                {QStringLiteral("runs"), QJsonArray()},
                                {QStringLiteral("active"), cleanRun(QStringLiteral("Scanning\nFreed 2 GB so far"))}};
        else if (action == QLatin1String("get"))
            reply = QJsonObject{{QStringLiteral("workflow"), agentFile}, {QStringLiteral("runs"), QJsonArray()}};
        if (action == QLatin1String("run") && id == QLatin1String("clean")) {
            QJsonArray workflows = localPayload.value(QStringLiteral("workflows")).toArray();
            QJsonObject clean = workflows.at(1).toObject();
            clean.insert(QStringLiteral("running"), true);
            clean.insert(QStringLiteral("recentRuns"), QJsonArray{cleanRun(QStringLiteral("Scanning"))});
            workflows.replace(1, clean);
            localPayload.insert(QStringLiteral("workflows"), workflows);
            reply = QJsonObject{{QStringLiteral("runId"), QStringLiteral("lr2")}};
        }
        if (action == QLatin1String("save")) {
            agentFile = payload.value(QStringLiteral("workflow")).toObject();
            reply = QJsonObject{{QStringLiteral("workflow"), agentFile}};
        }
        QTimer::singleShot(0, [done, reply] { done(true, reply, QStringLiteral("Done.")); });
    };
    auto *desk = static_cast<WorkflowsFeature *>(contribution->factory(localHost, nullptr));
    desk->resize(1280, 860);
    desk->show();
    desk->refresh();
    settle();
    WorkflowCard *sentinel = cardNamed(desk, QStringLiteral("Sentinel"));
    WorkflowCard *clean = cardNamed(desk, QStringLiteral("Clean up"));
    workflowsCheck(cardsOf(desk).size() == 4 && sentinel && clean &&
                       sentinel->entry().value(QStringLiteral("kindLabel")).toString() == QLatin1String("Special agent") &&
                       clean->entry().value(QStringLiteral("pinned")).toString() == QLatin1String("bottom"),
                   QStringLiteral("special agents and the account's workflows share one list with their kind and rail"));
    workflowsCheck(control(sentinel, QStringLiteral("Pin to rail")) &&
                       control(cardNamed(desk, QStringLiteral("Morning report")), QStringLiteral("Pin to rail")) &&
                       control(sentinel, QStringLiteral("Start schedule")) &&
                       control(sentinel, QStringLiteral("Last output")),
                   QStringLiteral("every card has the special agent controls: rail pin, schedule and last output"));
    named<QAbstractButton>(desk, QStringLiteral("Show special agents"))->click();
    settle();
    workflowsCheck(cardsOf(desk).size() == 1, QStringLiteral("the Special agents chip shows only special agents"));
    named<QAbstractButton>(desk, QStringLiteral("Show all"))->click();
    settle();
    auto *deskRuns = named<QTableWidget>(desk, QStringLiteral("workflowsRuns"));
    bool localRunListed = false;
    for (int row = 0; deskRuns && row < deskRuns->rowCount(); ++row)
        localRunListed = localRunListed || deskRuns->item(row, 0)->text() == QLatin1String("Sentinel");
    workflowsCheck(localRunListed, QStringLiteral("desktop runs join the recent runs"));
    const auto hits = desk->searchHits(QStringLiteral("senti"), 5);
    workflowsCheck(!hits.isEmpty() && hits.first().id == QLatin1String("repo:forkmesh/forkmesh|sentinel"),
                   QStringLiteral("the top search box finds desktop workflows"));
    workflowsCheck(desk->navigationBadge() >= 1, QStringLiteral("a failed desktop workflow counts on the rail badge"));
    if (auto *toggle = control(cardNamed(desk, QStringLiteral("Clean up")), QStringLiteral("Turn off")))
        toggle->click();
    settle();
    workflowsCheck(!localCalls.isEmpty() && localCalls.last().action == QLatin1String("setEnabled") &&
                       !localCalls.last().payload.value(QStringLiteral("enabled")).toBool(),
                   QStringLiteral("the switch turns a desktop workflow off"));
    if (auto *run = control(cardNamed(desk, QStringLiteral("Clean up")), QStringLiteral("Run")))
        run->click();
    settle();
    bool cleanStarted = false;
    for (const LocalCall &call : std::as_const(localCalls))
        cleanStarted = cleanStarted || (call.action == QLatin1String("run") && call.id == QLatin1String("clean"));
    workflowsCheck(cleanStarted, QStringLiteral("Run starts the desktop workflow on this desktop"));
    auto *deskFlow = dynamic_cast<RunFlow *>(named<QWidget>(desk, QStringLiteral("workflowsRunFlow")));
    workflowsCheck(deskFlow && deskFlow->live() && deskFlow->key() == QLatin1String("lr2") &&
                       !deskFlow->steps().isEmpty() &&
                       deskFlow->steps().first().name == QLatin1String("Clean caches") &&
                       deskFlow->steps().first().status == QLatin1String("running") &&
                       desk->findChild<QWidget *>(QStringLiteral("workflowsRunSparkle")) != nullptr,
                   QStringLiteral("a desktop run sparkles in and shows its running step live"));
    workflowsCheck(deskRuns->currentRow() >= 0 &&
                       deskRuns->item(deskRuns->currentRow(), 0)->data(Qt::UserRole + 1).toString() ==
                           QLatin1String("lr2"),
                   QStringLiteral("a desktop run started on the page is selected in Recent runs"));
    auto *deskOutput = named<QPlainTextEdit>(desk, QStringLiteral("workflowsRunOutput"));
    workflowsCheck(deskOutput && deskOutput->toPlainText().contains(QLatin1String("Freed 2 GB so far")) &&
                       deskOutput->toPlainText().contains(QLatin1String("Clean caches")),
                   QStringLiteral("the running step's live output shows beside Recent runs"));

    bool synced = false;
    for (const LocalCall &call : std::as_const(localCalls))
        synced = synced || (call.action == QLatin1String("sync") && call.repo == QLatin1String(kAccountRepo) &&
                            call.payload.value(QStringLiteral("workflows")).toArray().size() == 2);
    workflowsCheck(synced, QStringLiteral("the page tells the desktop the account workflows' names for their rail pins"));

    pressCard(cardNamed(desk, QStringLiteral("Sentinel")));
    auto *deskDetails = named<QFrame>(desk, QStringLiteral("Workflow details"));
    auto *deskFilter = named<QComboBox>(desk, QStringLiteral("Runs of workflow"));
    workflowsCheck(deskDetails && deskDetails->isVisible() && detailsName(desk) == QLatin1String("Sentinel") &&
                       deskFilter->currentData().toString() == QLatin1String("local:forkmesh/forkmesh|sentinel") &&
                       deskOutput->toPlainText().contains(QLatin1String("tests failed")),
                   QStringLiteral("clicking a special agent opens its details and its last log"));
    workflowsCheck(named<QPushButton>(deskDetails, QStringLiteral("Agent home")) &&
                       named<QPushButton>(deskDetails, QStringLiteral("Start schedule")) &&
                       named<QPushButton>(deskDetails, QStringLiteral("Pin to rail…")) &&
                       named<QPushButton>(deskDetails, QStringLiteral("Open output file")),
                   QStringLiteral("a special agent's details pane offers its agent, schedule, rail and output"));
    named<QPushButton>(deskDetails, QStringLiteral("Show file"))->click();
    settle();
    workflowsCheck(localCalls.last().action == QLatin1String("openFile") && localCalls.last().id == QLatin1String("sentinel"),
                   QStringLiteral("Show file in the details pane opens the workflow's file"));

    desk->activateSearchHit(QStringLiteral("repo:forkmesh/forkmesh|sentinel"));
    settle();
    auto *deskPages = desk->findChild<QStackedWidget *>();
    auto *deskSteps = named<QListWidget>(desk, QStringLiteral("workflowsSteps"));
    workflowsCheck(deskPages && deskPages->currentIndex() == 1 && deskSteps && deskSteps->count() == 2,
                   QStringLiteral("a desktop workflow opens in the editor with its steps"));
    deskSteps->setCurrentRow(1);
    settle();
    QCheckBox *modify = nullptr;
    for (QCheckBox *box : desk->findChildren<QCheckBox *>())
        if (box->text() == QLatin1String("Modify code"))
            modify = box;
    auto *skills = named<QPlainTextEdit>(desk, QStringLiteral("Skill names (one per line)"));
    workflowsCheck(modify && !modify->isChecked() && skills &&
                       skills->toPlainText() == QLatin1String("Threat models"),
                   QStringLiteral("the special agent step shows its permissions as boxes and its skills as lines"));
    if (modify)
        modify->setChecked(true);
    if (skills)
        skills->setPlainText(QStringLiteral("Threat models\nFuzzing"));
    named<QPushButton>(desk, QStringLiteral("Save"))->click();
    settle();
    QJsonObject savedAgent;
    for (const QJsonValue &value : agentFile.value(QStringLiteral("nodes")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == QLatin1String("agent"))
            savedAgent = value.toObject().value(QStringLiteral("params")).toObject();
    workflowsCheck(localCalls.last().action == QLatin1String("save") &&
                       savedAgent.value(QStringLiteral("permissions")).toArray().contains(QStringLiteral("modify-code")) &&
                       savedAgent.value(QStringLiteral("skills")).toArray().size() == 2 &&
                       agentFile.value(QStringLiteral("kind")).toString() == QLatin1String("forkmesh.workflow"),
                   QStringLiteral("Save writes the edited special agent back to its workflow file"));
    delete desk;

    auto *approving = static_cast<WorkflowsFeature *>(contribution->factory(localHost, nullptr));
    approving->resize(1280, 860);
    approving->show();
    approving->refresh();
    settle();
    const auto sentinelCalls = [&localCalls](const QString &action) {
        int count = 0;
        for (const LocalCall &call : std::as_const(localCalls))
            count += call.id == QLatin1String("sentinel") && call.action == action ? 1 : 0;
        return count;
    };
    auto *approvingRuns = named<QTableWidget>(approving, QStringLiteral("workflowsRuns"));
    auto *approvingOutput = named<QPlainTextEdit>(approving, QStringLiteral("workflowsRunOutput"));
    const auto selectedRun = [approvingRuns](int column) {
        const int row = approvingRuns ? approvingRuns->currentRow() : -1;
        return row >= 0 && approvingRuns->item(row, column) ? approvingRuns->item(row, column) : nullptr;
    };
    if (auto *run = control(cardNamed(approving, QStringLiteral("Sentinel")), QStringLiteral("Run")))
        run->click();
    settle();
    workflowsCheck(sentinelCalls(QStringLiteral("run")) == 1 && sentinelCalls(QStringLiteral("review")) == 1,
                   QStringLiteral("Run on a workflow this desktop has not approved opens the approval box"));
    workflowsCheck(selectedRun(3) && selectedRun(3)->text() == QLatin1String("Failed") &&
                       selectedRun(0)->data(Qt::UserRole + 1).toString().startsWith(QLatin1String("pending:")) &&
                       approvingOutput->toPlainText().contains(QLatin1String("did not start")) &&
                       approvingOutput->toPlainText().contains(QLatin1String("not approved")),
                   QStringLiteral("a run that is not approved stays in Recent runs as Failed with the reason"));
    allowSentinel = true;
    if (auto *run = control(cardNamed(approving, QStringLiteral("Sentinel")), QStringLiteral("Run")))
        run->click();
    settle();
    auto *approvingFlow = dynamic_cast<RunFlow *>(named<QWidget>(approving, QStringLiteral("workflowsRunFlow")));
    workflowsCheck(sentinelCalls(QStringLiteral("run")) == 3 && sentinelCalls(QStringLiteral("review")) == 2 &&
                       selectedRun(0) && selectedRun(0)->data(Qt::UserRole + 1).toString() == QLatin1String("lr3") &&
                       approvingFlow && approvingFlow->live() && approvingFlow->key() == QLatin1String("lr3") &&
                       approvingOutput->toPlainText().contains(QLatin1String("Reading open issues")),
                   QStringLiteral("after Allow the run starts and shows its steps and live output"));
    delete approving;
}
