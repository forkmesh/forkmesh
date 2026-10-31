#include "LocalControlWorkflows.h"

#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::localcontrol::workflows;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void testSubjects()
{
    check(familyPrefix() == QLatin1String("workflows."), "prefix");
    QString error;
    const QJsonObject good{{QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
                           {QStringLiteral("id"), QStringLiteral("free-disk-space")}};
    check(subjectFor(QStringLiteral("run"), good, &error) == QLatin1String("forkmesh/forkmesh|free-disk-space"),
          "run subject");
    check(subjectFor(QStringLiteral("stop"), good, &error) == QLatin1String("forkmesh/forkmesh|free-disk-space"),
          "stop subject");
    check(subjectFor(QStringLiteral("delete"), good, &error).isEmpty() && error == QLatin1String("unsupported_action"),
          "delete refused");
    error.clear();
    check(subjectFor(QStringLiteral("run"),
                     {{QStringLiteral("repo"), QStringLiteral("forkmesh")}, {QStringLiteral("id"), QStringLiteral("a")}},
                     &error)
                  .isEmpty() &&
              error == QLatin1String("bad_request"),
          "repo needs owner and name");
    check(!validRepo(QStringLiteral("a/b|c")), "no bar in repo");
    check(!validRepo(QStringLiteral("a/b c")), "no space in repo");
    check(validRepo(QStringLiteral("jett/my.repo-1")), "plain repo");
    check(!validId(QStringLiteral("../x")), "no path in id");
    check(!validId(QStringLiteral("")), "empty id");
    check(validId(QStringLiteral("release-manager")), "plain id");
    check(actionSummary(QStringLiteral("run"), QStringLiteral("Free disk space"), QStringLiteral("forkmesh/forkmesh"))
              .contains(QLatin1String("Free disk space")),
          "summary names the workflow");
    check(actionSummary(QStringLiteral("stop"), QString(), QString()).startsWith(QLatin1String("Stop")), "stop summary");
}

static void testRows()
{
    const QJsonObject row{{QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
                          {QStringLiteral("id"), QStringLiteral("cfo")},
                          {QStringLiteral("name"), QStringLiteral("CFO")},
                          {QStringLiteral("face"), QStringLiteral("iVBORw0KGgo=")},
                          {QStringLiteral("path"), QStringLiteral("/home/user/src/.forkmesh/workflows/cfo.json")},
                          {QStringLiteral("recentRuns"), QJsonArray{QJsonObject{}}},
                          {QStringLiteral("specialAgent"), true},
                          {QStringLiteral("lastSummary"), QString(1000, QLatin1Char('x'))},
                          {QStringLiteral("running"), false}};
    const QJsonObject out = listRow(row);
    check(!out.contains(QStringLiteral("face")), "no portrait bytes");
    check(!out.contains(QStringLiteral("path")), "no local path");
    check(!out.contains(QStringLiteral("recentRuns")), "no runs in the list");
    check(out.value(QStringLiteral("specialAgent")).toBool(), "kind kept");
    check(out.value(QStringLiteral("lastSummary")).toString().size() == kMaxSummaryChars, "summary bounded");
    QJsonArray many;
    for (int index = 0; index < kMaxWorkflows + 5; ++index)
        many.append(row);
    check(listRows(many).size() == kMaxWorkflows, "list bounded");
}

static void testDetail()
{
    QJsonArray logs;
    for (int index = 0; index < 30; ++index)
        logs.append(QStringLiteral("line %1").arg(index));
    const QJsonObject step{{QStringLiteral("nodeId"), QStringLiteral("usage")},
                           {QStringLiteral("name"), QStringLiteral("Measure the disk")},
                           {QStringLiteral("status"), QStringLiteral("success")},
                           {QStringLiteral("output"), QString(5000, QLatin1Char('o'))},
                           {QStringLiteral("logs"), logs}};
    QJsonArray runs;
    for (int index = 0; index < 20; ++index)
        runs.append(QJsonObject{{QStringLiteral("id"), QString::number(index)},
                                {QStringLiteral("status"), QStringLiteral("success")},
                                {QStringLiteral("assignment"), QStringLiteral("secret plan")},
                                {QStringLiteral("outputFile"), QStringLiteral("/home/user/out.md")},
                                {QStringLiteral("steps"), QJsonArray{step}}});
    const QJsonObject reply{{QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
                            {QStringLiteral("workflow"),
                             QJsonObject{{QStringLiteral("id"), QStringLiteral("clean")},
                                         {QStringLiteral("name"), QStringLiteral("Clean")},
                                         {QStringLiteral("nodes"),
                                          QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("usage")},
                                                                 {QStringLiteral("type"), QStringLiteral("desktop.diskUsage")},
                                                                 {QStringLiteral("name"), QStringLiteral("Measure")},
                                                                 {QStringLiteral("params"), QJsonObject{}}}}}}},
                            {QStringLiteral("runs"), runs},
                            {QStringLiteral("active"), runs.first()}};
    const QJsonObject out = detail(reply);
    check(out.value(QStringLiteral("id")).toString() == QLatin1String("clean"), "detail id");
    check(out.value(QStringLiteral("runs")).toArray().size() == kMaxRuns, "runs bounded");
    const QJsonObject first = out.value(QStringLiteral("runs")).toArray().first().toObject();
    check(!first.contains(QStringLiteral("assignment")), "no assignment text");
    check(!first.contains(QStringLiteral("outputFile")), "no local output path");
    const QJsonObject firstStep = first.value(QStringLiteral("steps")).toArray().first().toObject();
    check(firstStep.value(QStringLiteral("output")).toString().size() == kMaxStepOutputChars, "step output bounded");
    check(firstStep.value(QStringLiteral("logs")).toArray().size() == kMaxLogLines, "logs bounded");
    check(firstStep.value(QStringLiteral("logs")).toArray().last().toString() == QLatin1String("line 29"),
          "newest log lines kept");
    check(!out.value(QStringLiteral("nodes")).toArray().first().toObject().contains(QStringLiteral("params")),
          "no step settings");
    check(out.contains(QStringLiteral("active")), "active run");
}

int main()
{
    testSubjects();
    testRows();
    testDetail();
    if (failures)
        return 1;
    std::cout << "local control workflows: ok\n";
    return 0;
}
