#include "AgentTaskList.h"
#include "AgentTaskListBar.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::agenttasks;

static int failures = 0;

static void check(bool ok, const QString &what)
{
    if (ok)
        return;
    std::cerr << "FAIL: " << qPrintable(what) << "\n";
    ++failures;
}

static QJsonObject parse(const QByteArray &json)
{
    return QJsonDocument::fromJson(json).object();
}

static QJsonObject toolUse(const QString &id, const QString &name, const QJsonObject &input,
                           const QString &parent = QString())
{
    QJsonObject event{{QStringLiteral("type"), QStringLiteral("assistant")},
                      {QStringLiteral("message"),
                       QJsonObject{{QStringLiteral("content"),
                                    QJsonArray{QJsonObject{
                                        {QStringLiteral("type"), QStringLiteral("tool_use")},
                                        {QStringLiteral("id"), id},
                                        {QStringLiteral("name"), name},
                                        {QStringLiteral("input"), input}}}}}}};
    if (!parent.isEmpty())
        event.insert(QStringLiteral("parent_tool_use_id"), parent);
    return event;
}

static QJsonObject toolResult(const QString &id, const QString &text, bool isError = false)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("user")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("content"),
                      QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                                             {QStringLiteral("tool_use_id"), id},
                                             {QStringLiteral("content"), text},
                                             {QStringLiteral("is_error"), isError}}}}}}};
}

static QJsonObject create(const QString &subject, const QString &activeForm = QString())
{
    return QJsonObject{{QStringLiteral("subject"), subject},
                       {QStringLiteral("activeForm"), activeForm}};
}

static void testTodoWriteReplacesList()
{
    Tracker tracker;
    const QJsonObject first = parse(R"({"todos":[
        {"content":"Read code","activeForm":"Reading code","status":"completed"},
        {"content":"Fix bug","activeForm":"Fixing bug","status":"in_progress"},
        {"content":"Run tests","activeForm":"Running tests","status":"pending"}]})");
    check(tracker.ingest(toolUse(QStringLiteral("t1"), QStringLiteral("TodoWrite"), first)),
          QStringLiteral("TodoWrite changes the list"));
    check(tracker.tasks().size() == 3, QStringLiteral("three todos"));
    check(tracker.completedCount() == 1, QStringLiteral("one done"));
    check(tracker.current() && tracker.current()->activeForm == QStringLiteral("Fixing bug"),
          QStringLiteral("current task is the in-progress one"));
    check(!tracker.ingest(toolUse(QStringLiteral("t2"), QStringLiteral("TodoWrite"), first)),
          QStringLiteral("the same list again is no change"));
    const QJsonObject second = parse(R"({"todos":[{"content":"Only","status":"completed"}]})");
    check(tracker.ingest(toolUse(QStringLiteral("t3"), QStringLiteral("TodoWrite"), second)),
          QStringLiteral("a new TodoWrite replaces the list"));
    check(tracker.tasks().size() == 1 && tracker.allCompleted(),
          QStringLiteral("replaced list is complete"));
}

static void testTaskToolsBuildList()
{
    Tracker tracker;
    check(tracker.ingest(toolUse(QStringLiteral("c1"), QStringLiteral("TaskCreate"),
                                 create(QStringLiteral("Plan"), QStringLiteral("Planning")))),
          QStringLiteral("TaskCreate adds a task at once"));
    check(tracker.tasks().size() == 1 && tracker.tasks().first().status == QStringLiteral("pending"),
          QStringLiteral("created task is pending"));
    check(tracker.ingest(toolResult(QStringLiteral("c1"),
                                    QStringLiteral("Task #1 created successfully: Plan"))),
          QStringLiteral("result assigns the id"));
    check(tracker.tasks().first().id == QStringLiteral("1"), QStringLiteral("id from result"));
    tracker.ingest(toolUse(QStringLiteral("c2"), QStringLiteral("TaskCreate"),
                           create(QStringLiteral("Build"))));
    tracker.ingest(toolResult(QStringLiteral("c2"), QStringLiteral("Task #2 created successfully")));
    tracker.ingest(toolUse(QStringLiteral("c3"), QStringLiteral("TaskCreate"),
                           create(QStringLiteral("Broken"))));
    tracker.ingest(toolResult(QStringLiteral("c3"), QStringLiteral("failed"), true));
    check(tracker.tasks().size() == 2, QStringLiteral("a failed create is dropped"));
    check(tracker.ingest(toolUse(QStringLiteral("u1"), QStringLiteral("TaskUpdate"),
                                 parse(R"({"taskId":"1","status":"in_progress"})"))),
          QStringLiteral("TaskUpdate changes status"));
    check(tracker.current() && tracker.current()->id == QStringLiteral("1"),
          QStringLiteral("task 1 in progress"));
    tracker.ingest(toolUse(QStringLiteral("u2"), QStringLiteral("TaskUpdate"),
                           parse(R"({"taskId":"1","status":"completed"})")));
    tracker.ingest(toolUse(QStringLiteral("u3"), QStringLiteral("TaskUpdate"),
                           parse(R"({"taskId":"2","subject":"Build all"})")));
    check(tracker.tasks().at(1).subject == QStringLiteral("Build all") &&
              tracker.tasks().at(1).status == QStringLiteral("pending"),
          QStringLiteral("subject update keeps status"));
    check(tracker.ingest(toolUse(QStringLiteral("u4"), QStringLiteral("TaskUpdate"),
                                 parse(R"({"taskId":"2","status":"deleted"})"))),
          QStringLiteral("deleted removes"));
    check(tracker.tasks().size() == 1 && tracker.allCompleted(),
          QStringLiteral("remaining task is complete"));
    check(!tracker.ingest(toolUse(QStringLiteral("u5"), QStringLiteral("TaskUpdate"),
                                  parse(R"({"taskId":"9","status":"completed"})"))),
          QStringLiteral("unknown id is ignored"));
}

static void testSubagentGroups()
{
    Tracker tracker;
    tracker.ingest(toolUse(QStringLiteral("agent1"), QStringLiteral("Task"),
                           parse(R"({"description":"Explore the parser","prompt":"go"})")));
    const QJsonObject todos = parse(R"({"todos":[
        {"content":"Sub A","activeForm":"Reading A","status":"in_progress"},
        {"content":"Sub B","status":"pending"}]})");
    check(tracker.ingest(toolUse(QStringLiteral("s1"), QStringLiteral("TodoWrite"), todos,
                                 QStringLiteral("agent1"))),
          QStringLiteral("subagent todo list is tracked"));
    check(tracker.tasks().isEmpty(), QStringLiteral("main list stays separate"));
    const TaskGroup *group = tracker.subagentGroup(QStringLiteral("agent1"));
    check(group && group->title == QStringLiteral("Explore the parser") && group->tasks.size() == 2,
          QStringLiteral("group titled by the subagent description"));
    check(tracker.current() && tracker.current()->activeForm == QStringLiteral("Reading A"),
          QStringLiteral("subagent work counts as current when the main list is idle"));
    tracker.ingest(toolUse(QStringLiteral("c1"), QStringLiteral("TaskCreate"),
                           create(QStringLiteral("Sub C")), QStringLiteral("agent1")));
    QJsonObject result = toolResult(QStringLiteral("c1"), QStringLiteral("Task #1 created successfully"));
    result.insert(QStringLiteral("parent_tool_use_id"), QStringLiteral("agent1"));
    tracker.ingest(result);
    check(tracker.subagentGroup(QStringLiteral("agent1"))->tasks.size() == 3 &&
              tracker.subagentGroup(QStringLiteral("agent1"))->tasks.last().id == QStringLiteral("1"),
          QStringLiteral("subagent TaskCreate gets its id in its own group"));
    check(tracker.counts().total == 3 && tracker.groups().size() == 1,
          QStringLiteral("counts include subagent tasks"));
    check(tracker.ingest(toolResult(QStringLiteral("agent1"), QStringLiteral("report"))),
          QStringLiteral("subagent finishing changes the list"));
    check(tracker.isEmpty(), QStringLiteral("finished subagent's group is dropped"));
}

static void testCodexPlanDeltas()
{
    Tracker tracker;
    const QJsonObject delta{
        {QStringLiteral("type"), QStringLiteral("_codex_tool_delta")},
        {QStringLiteral("method"), QStringLiteral("turn/plan/updated")},
        {QStringLiteral("delta"),
         QStringLiteral(R"([{"step":"Inspect","status":"completed"},{"step":"Fix","status":"inProgress"}])")}};
    check(tracker.ingest(delta), QStringLiteral("Codex plan delta updates the list"));
    check(tracker.tasks().size() == 2 && tracker.current() &&
              tracker.current()->subject == QStringLiteral("Fix"),
          QStringLiteral("Codex inProgress maps to in_progress"));
    QJsonObject other = delta;
    other.insert(QStringLiteral("method"), QStringLiteral("item/commandExecution/outputDelta"));
    check(!tracker.ingest(other), QStringLiteral("other deltas are ignored"));
}

static void testCreatedTaskId()
{
    check(createdTaskId(QStringLiteral("Task #12 created successfully: x")) == QStringLiteral("12"),
          QStringLiteral("id parsed"));
    check(createdTaskId(QStringLiteral("nothing")).isEmpty(), QStringLiteral("no id"));
    check(normalizedStatus(QString()) == QStringLiteral("pending"), QStringLiteral("empty is pending"));
    Counts counts;
    counts.total = 4;
    counts.completed = 1;
    counts.inProgress = 1;
    check(summaryText(counts) == QStringLiteral("4 tasks (1 done, 1 in progress, 2 open)"),
          summaryText(counts));
}

static void testBar()
{
    Tracker tracker;
    AgentTaskListBar bar;
    bar.resize(400, 100);
    bar.setTasks(tracker);
    check(bar.isHidden(), QStringLiteral("hidden without tasks"));
    tracker.ingest(toolUse(QStringLiteral("t1"), QStringLiteral("TodoWrite"),
                           parse(R"({"todos":[
        {"content":"A","status":"completed"},
        {"content":"B","activeForm":"Doing B","status":"in_progress"},
        {"content":"C <i>","status":"pending"}]})")));
    bar.setTasks(tracker);
    check(!bar.isHidden(), QStringLiteral("shown with tasks"));
    check(bar.headerText().contains(QStringLiteral("3 tasks (1 done, 1 in progress, 1 open)")),
          bar.headerText());
    check(bar.rowsHtml().contains(QStringLiteral("◼")) && bar.rowsHtml().contains(QStringLiteral("◻")) &&
              bar.rowsHtml().contains(QStringLiteral("✔")),
          QStringLiteral("Claude Code glyphs"));
    check(bar.rowsHtml().contains(QStringLiteral("Doing B")), QStringLiteral("active form shown"));
    check(bar.rowsHtml().contains(QStringLiteral("C &lt;i&gt;")),
          QStringLiteral("subjects are escaped"));
    check(bar.heightForWidth(400) > 0, QStringLiteral("has height"));
    bar.setCollapsed(true);
    check(bar.headerText().contains(QStringLiteral("Doing B")),
          QStringLiteral("collapsed header names the current task"));
    bar.setCollapsed(false);
    tracker.ingest(toolUse(QStringLiteral("t2"), QStringLiteral("TodoWrite"),
                           parse(R"({"todos":[{"content":"A","status":"completed"}]})")));
    bar.setTasks(tracker);
    check(bar.isCollapsed(), QStringLiteral("collapses when everything is done"));
    check(bar.headerText().contains(QStringLiteral("1 task (1 done)")), bar.headerText());
    QJsonArray many;
    for (int i = 0; i < AgentTaskListBar::kMaxRows + 3; ++i)
        many.append(QJsonObject{{QStringLiteral("content"), QStringLiteral("T%1").arg(i)},
                                {QStringLiteral("status"), QStringLiteral("pending")}});
    tracker.ingest(toolUse(QStringLiteral("t3"), QStringLiteral("TodoWrite"),
                           QJsonObject{{QStringLiteral("todos"), many}}));
    bar.setTasks(tracker);
    check(!bar.isCollapsed(), QStringLiteral("reopens when new work arrives"));
    check(bar.rowsHtml().contains(QStringLiteral("+3 more")), QStringLiteral("long lists are capped"));
    Tracker withAgent;
    withAgent.ingest(toolUse(QStringLiteral("agent1"), QStringLiteral("Agent"),
                             parse(R"({"description":"Check <docs>"})")));
    withAgent.ingest(toolUse(QStringLiteral("s1"), QStringLiteral("TodoWrite"),
                             parse(R"({"todos":[{"content":"Look","status":"pending"}]})"),
                             QStringLiteral("agent1")));
    bar.setTasks(withAgent);
    check(bar.rowsHtml().contains(QStringLiteral("Check &lt;docs&gt;")),
          QStringLiteral("subagent heading shown and escaped"));
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    testTodoWriteReplacesList();
    testTaskToolsBuildList();
    testSubagentGroups();
    testCodexPlanDeltas();
    testCreatedTaskId();
    testBar();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "agent task list tests passed\n";
    return 0;
}
