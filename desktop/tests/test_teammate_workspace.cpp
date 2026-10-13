#include "TeammateWorkspace.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>

#include <iostream>

using namespace forkmesh::teammates;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory is available");
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("workspaces"));
    const WorkspaceStore store(root);

    check(store.load(QStringLiteral("quality")).widgets.isEmpty() &&
              store.load(QStringLiteral("quality")).notes.isEmpty(),
          "a special agent without a saved workspace starts empty");
    check(WorkspaceStore(QString()).pathFor(QStringLiteral("quality")).isEmpty(),
          "an empty workspace root never falls back to the process directory");
    check(store.pathFor(QStringLiteral("../escape")).startsWith(root) &&
              !store.pathFor(QStringLiteral("../escape")).contains(QStringLiteral("..")),
          "workspace file names are made safe");

    Workspace workspace;
    workspace.notes = QStringLiteral("Ship the release notes before Friday.");
    Widget checklist = newWidget(kWidgetChecklist, QStringLiteral("Release"));
    checklist.items = {{QStringLiteral("Draft notes"), {}, true},
                       {QStringLiteral("Tag build"), {}, false},
                       {QStringLiteral("  "), {}, false}};
    check(upsertWidget(&workspace, checklist), "a checklist widget is added");
    Widget goal = newWidget(kWidgetGoal, QStringLiteral("Coverage"));
    goal.value = 40;
    goal.target = 80;
    goal.unit = QStringLiteral("%");
    check(upsertWidget(&workspace, goal), "a goal widget is added");
    Widget links = newWidget(kWidgetLinks, QStringLiteral("Docs"));
    links.items = {{QStringLiteral("Plan"), QStringLiteral("https://forkmesh.com/docs"), false},
                   {QStringLiteral("Bad"), QStringLiteral("javascript:alert(1)"), false},
                   {QStringLiteral("Local"), QStringLiteral("file:///etc/passwd"), false}};
    check(upsertWidget(&workspace, links), "a links widget is added");
    check(workspace.widgets.size() == 3 &&
              workspace.widgets.at(0).items.size() == 2 &&
              workspace.widgets.at(2).items.size() == 1,
          "blank checklist items and non-web links are dropped");
    check(qFuzzyCompare(widgetProgress(workspace.widgets.at(0)), 0.5) &&
              qFuzzyCompare(widgetProgress(workspace.widgets.at(1)), 0.5) &&
              openChecklistItems(workspace.widgets.at(0)) == 1,
          "checklists and goals report their progress");
    Widget unknown;
    unknown.kind = QStringLiteral("script");
    check(!upsertWidget(&workspace, unknown) && workspace.widgets.size() == 3,
          "unknown widget kinds are refused");

    appendMessage(&workspace, {kWorkspaceFromYou, QStringLiteral("Please check CI"), 0, 7});
    appendMessage(&workspace, {kWorkspaceFromYou, QStringLiteral("   "), 0, 0});
    check(workspace.messages.size() == 1 && workspace.messages.first().atMs > 0,
          "messages are stamped and blank ones skipped");
    for (int index = 0; index < kWorkspaceMaxMessages + 5; ++index)
        appendMessage(&workspace, {kWorkspaceFromYou, QString::number(index), 0, 0});
    check(workspace.messages.size() == kWorkspaceMaxMessages &&
              workspace.messages.last().text ==
                  QString::number(kWorkspaceMaxMessages + 4),
          "the conversation keeps the newest messages");

    check(store.save(QStringLiteral("quality"), workspace),
          "the workspace saves");
    const Workspace reloaded = store.load(QStringLiteral("quality"));
    check(reloaded.notes == workspace.notes &&
              reloaded.widgets.size() == 3 &&
              reloaded.widgets.at(1).kind == kWidgetGoal &&
              qFuzzyCompare(reloaded.widgets.at(1).value, 40.0) &&
              reloaded.widgets.at(1).unit == QLatin1String("%") &&
              reloaded.widgets.at(0).items.at(0).done &&
              reloaded.widgets.at(2).items.at(0).url ==
                  QLatin1String("https://forkmesh.com/docs") &&
              reloaded.messages.size() == kWorkspaceMaxMessages,
          "notes, widgets and messages round-trip through the file");

    const QString section = workspacePromptSection(reloaded);
    check(section.contains(QStringLiteral("Ship the release notes")) &&
              section.contains(QStringLiteral("[x] Draft notes")) &&
              section.contains(QStringLiteral("[ ] Tag build")) &&
              section.contains(QStringLiteral("40 of 80 %")) &&
              section.contains(QStringLiteral("forkmesh-widget")) &&
              section.contains(reloaded.widgets.at(0).id),
          "the agent prompt carries the notes, widget state and widget contract");
    check(workspacePromptSection(Workspace{}).isEmpty(),
          "an empty workspace adds nothing to the agent prompt");
    check(widgetBuildAssignment(QStringLiteral("Track flaky tests"))
                  .contains(QStringLiteral("Track flaky tests")) &&
              widgetBuildAssignment(QString()).contains(QStringLiteral("forkmesh-widget")),
          "a widget request names the operator's ask and the contract");

    const QString output = QStringLiteral(
        "Here is a tracker.\n```forkmesh-widget\n"
        "{\"kind\": \"checklist\", \"title\": \"Flaky tests\", "
        "\"items\": [\"test_a\", {\"text\": \"test_b\", \"done\": true}]}\n```\n"
        "And an update:\n```forkmesh-widget\n{\"id\": \"%1\", \"kind\": \"goal\", "
        "\"title\": \"Coverage\", \"value\": 55, \"target\": 80}\n```\n"
        "```forkmesh-widget\nnot json\n```\n"
        "```forkmesh-widget\n{\"kind\": \"shell\", \"text\": \"rm -rf /\"}\n```\n")
                               .arg(reloaded.widgets.at(1).id);
    const QList<WidgetProposal> proposals =
        pendingProposals(reloaded, output, 42);
    check(proposals.size() == 2 && !proposals.at(0).updatesExisting &&
              proposals.at(0).widget.items.size() == 2 &&
              proposals.at(0).widget.items.at(1).done &&
              proposals.at(1).updatesExisting && proposals.at(0).sessionId == 42,
          "widget proposals are parsed from agent output, invalid ones skipped");
    Workspace handled = reloaded;
    markProposalHandled(&handled, proposals.at(0).fingerprint);
    check(pendingProposals(handled, output, 42).size() == 1,
          "a proposal the operator handled is not offered again");
    Widget update = proposals.at(1).widget;
    check(upsertWidget(&handled, update) && handled.widgets.size() == 3 &&
              qFuzzyCompare(handled.widgets.at(1).value, 55.0),
          "an update proposal replaces the widget with the same id");
    check(removeWidget(&handled, handled.widgets.at(0).id) &&
              handled.widgets.size() == 2,
          "a widget can be removed");

    QFile broken(store.pathFor(QStringLiteral("broken")));
    check(broken.open(QIODevice::WriteOnly), "broken workspace file can be written");
    broken.write("{not json");
    broken.close();
    check(store.load(QStringLiteral("broken")).widgets.isEmpty(),
          "an unreadable workspace loads empty instead of failing");

    check(failures == 0, "all teammate workspace checks pass");
    if (failures == 0)
        std::cout << "teammate workspace tests passed\n";
    return failures == 0 ? 0 : 1;
}
