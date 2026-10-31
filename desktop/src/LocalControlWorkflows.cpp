#include "LocalControlWorkflows.h"

#include <QRegularExpression>

namespace forkmesh::localcontrol::workflows {

namespace {

QString fail(QString *error, const char *code)
{
    if (error)
        *error = QString::fromLatin1(code);
    return {};
}

QJsonArray lastLines(const QJsonArray &lines, int count)
{
    QJsonArray out;
    for (qsizetype index = qMax<qsizetype>(0, lines.size() - count); index < lines.size(); ++index)
        out.append(lines.at(index).toString().left(kMaxSummaryChars));
    return out;
}

} // namespace

QString familyPrefix()
{
    return QStringLiteral("workflows.");
}

QStringList actions()
{
    return {QStringLiteral("run"), QStringLiteral("stop")};
}

bool validRepo(const QString &repo)
{
    static const QRegularExpression pattern(QStringLiteral("^[^/|\\s]+/[^|\\s]+$"));
    return repo.size() <= 200 && pattern.match(repo).hasMatch();
}

bool validId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]*$"));
    return id.size() <= 120 && pattern.match(id).hasMatch();
}

QString subjectFor(const QString &action, const QJsonObject &params, QString *error)
{
    if (!actions().contains(action))
        return fail(error, "unsupported_action");
    const QString repo = params.value(QStringLiteral("repo")).toString().trimmed();
    const QString id = params.value(QStringLiteral("id")).toString().trimmed();
    if (!validRepo(repo) || !validId(id))
        return fail(error, "bad_request");
    return repo + QLatin1Char('|') + id;
}

QString actionSummary(const QString &action, const QString &name, const QString &repo)
{
    const QString label = name.trimmed().isEmpty() ? QStringLiteral("this workflow") : name.trimmed();
    if (action == QLatin1String("stop"))
        return QStringLiteral("Stop %1 on the desktop.").arg(label);
    return QStringLiteral("Run %1 from %2 on the desktop now.").arg(label, repo);
}

QJsonObject listRow(const QJsonObject &row)
{
    static const QStringList kept{
        QStringLiteral("repo"),       QStringLiteral("id"),          QStringLiteral("name"),
        QStringLiteral("icon"),       QStringLiteral("enabled"),     QStringLiteral("specialAgent"),
        QStringLiteral("steps"),      QStringLiteral("trigger"),     QStringLiteral("running"),
        QStringLiteral("runningStep"), QStringLiteral("runningSince"), QStringLiteral("hasSchedule"),
        QStringLiteral("scheduleActive"), QStringLiteral("nextRunAt"), QStringLiteral("lastRunAt"),
        QStringLiteral("lastStatus"), QStringLiteral("runs"),        QStringLiteral("failedRuns"),
        QStringLiteral("agentWorking")};
    QJsonObject out;
    for (const QString &key : kept)
        if (row.contains(key))
            out.insert(key, row.value(key));
    out.insert(QStringLiteral("description"),
               row.value(QStringLiteral("description")).toString().left(kMaxSummaryChars));
    out.insert(QStringLiteral("lastSummary"),
               row.value(QStringLiteral("lastSummary")).toString().left(kMaxSummaryChars));
    out.insert(QStringLiteral("error"), row.value(QStringLiteral("error")).toString().left(kMaxErrorChars));
    return out;
}

QJsonArray listRows(const QJsonArray &rows)
{
    QJsonArray out;
    for (const QJsonValue &value : rows) {
        if (out.size() >= kMaxWorkflows)
            break;
        out.append(listRow(value.toObject()));
    }
    return out;
}

QJsonObject runRow(const QJsonObject &run)
{
    QJsonArray steps;
    for (const QJsonValue &value : run.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject step = value.toObject();
        if (steps.size() >= kMaxSteps)
            break;
        QJsonObject entry{{QStringLiteral("nodeId"), step.value(QStringLiteral("nodeId"))},
                          {QStringLiteral("name"), step.value(QStringLiteral("name"))},
                          {QStringLiteral("type"), step.value(QStringLiteral("type"))},
                          {QStringLiteral("status"), step.value(QStringLiteral("status"))},
                          {QStringLiteral("startedAtMs"), step.value(QStringLiteral("startedAtMs"))},
                          {QStringLiteral("finishedAtMs"), step.value(QStringLiteral("finishedAtMs"))}};
        const QString output = step.value(QStringLiteral("output")).toString();
        if (!output.isEmpty())
            entry.insert(QStringLiteral("output"), output.left(kMaxStepOutputChars));
        const QString error = step.value(QStringLiteral("error")).toString();
        if (!error.isEmpty())
            entry.insert(QStringLiteral("error"), error.left(kMaxErrorChars));
        const QJsonArray logs = step.value(QStringLiteral("logs")).toArray();
        if (!logs.isEmpty())
            entry.insert(QStringLiteral("logs"), lastLines(logs, kMaxLogLines));
        steps.append(entry);
    }
    QJsonObject out{{QStringLiteral("id"), run.value(QStringLiteral("id"))},
                    {QStringLiteral("trigger"), run.value(QStringLiteral("trigger"))},
                    {QStringLiteral("status"), run.value(QStringLiteral("status"))},
                    {QStringLiteral("startedAtMs"), run.value(QStringLiteral("startedAtMs"))},
                    {QStringLiteral("finishedAtMs"), run.value(QStringLiteral("finishedAtMs"))},
                    {QStringLiteral("caption"),
                     run.value(QStringLiteral("caption")).toString().left(kMaxSummaryChars)},
                    {QStringLiteral("output"),
                     run.value(QStringLiteral("output")).toString().left(kMaxRunOutputChars)},
                    {QStringLiteral("steps"), steps}};
    const QString error = run.value(QStringLiteral("error")).toString();
    if (!error.isEmpty())
        out.insert(QStringLiteral("error"), error.left(kMaxErrorChars));
    return out;
}

QJsonObject detail(const QJsonObject &reply)
{
    const QJsonObject workflow = reply.value(QStringLiteral("workflow")).toObject();
    QJsonArray nodes;
    for (const QJsonValue &value : workflow.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject node = value.toObject();
        if (nodes.size() >= kMaxSteps)
            break;
        nodes.append(QJsonObject{{QStringLiteral("id"), node.value(QStringLiteral("id"))},
                                 {QStringLiteral("type"), node.value(QStringLiteral("type"))},
                                 {QStringLiteral("name"), node.value(QStringLiteral("name"))}});
    }
    QJsonArray runs;
    for (const QJsonValue &value : reply.value(QStringLiteral("runs")).toArray()) {
        if (runs.size() >= kMaxRuns)
            break;
        runs.append(runRow(value.toObject()));
    }
    QJsonObject out{{QStringLiteral("repo"), reply.value(QStringLiteral("repo"))},
                    {QStringLiteral("id"), workflow.value(QStringLiteral("id"))},
                    {QStringLiteral("name"), workflow.value(QStringLiteral("name"))},
                    {QStringLiteral("description"),
                     workflow.value(QStringLiteral("description")).toString().left(kMaxSummaryChars)},
                    {QStringLiteral("enabled"), workflow.value(QStringLiteral("enabled")).toBool(true)},
                    {QStringLiteral("error"),
                     reply.value(QStringLiteral("error")).toString().left(kMaxErrorChars)},
                    {QStringLiteral("nodes"), nodes},
                    {QStringLiteral("runs"), runs}};
    if (reply.contains(QStringLiteral("active")))
        out.insert(QStringLiteral("active"), runRow(reply.value(QStringLiteral("active")).toObject()));
    return out;
}

} // namespace forkmesh::localcontrol::workflows
