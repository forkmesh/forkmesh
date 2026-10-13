#include "LocalControlTeammates.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace forkmesh::localcontrol::teammates {

using forkmesh::teammates::Config;

namespace {

bool matches(const QString &pattern, const QString &value)
{
    return QRegularExpression(pattern).match(value).hasMatch();
}

bool validId(const QString &id)
{
    return matches(QStringLiteral("^[a-z0-9][a-z0-9-]{0,63}$"), id);
}

bool retiredAgentField(const QString &key)
{
    return key == QLatin1String("provider") || key == QLatin1String("model") ||
           key == QLatin1String("mode") || key == QLatin1String("backupModels") ||
           key == QLatin1String("taskAgents");
}

QStringList scheduleModes()
{
    return {forkmesh::teammates::kScheduleOnDemand, forkmesh::teammates::kScheduleContinuous,
            forkmesh::teammates::kScheduleHourly,   forkmesh::teammates::kScheduleDaily,
            forkmesh::teammates::kScheduleWeekly,   forkmesh::teammates::kScheduleWeekdays,
            forkmesh::teammates::kScheduleCustom,   forkmesh::teammates::kScheduleEvents};
}

QJsonArray lastItems(const QStringList &items, int count, int maxChars)
{
    QJsonArray out;
    for (const QString &item : items.mid(qMax<qsizetype>(0, items.size() - count)))
        out.append(item.left(maxChars));
    return out;
}

bool fail(QString *error, const QString &why)
{
    if (error)
        *error = why;
    return false;
}

bool readText(const QJsonObject &changes, const QString &key, int maxChars, bool required,
              QString *out, QString *error)
{
    const QJsonValue value = changes.value(key);
    if (!value.isString())
        return fail(error, QStringLiteral("bad_field:") + key);
    const QString text = value.toString().trimmed();
    if ((required && text.isEmpty()) || text.size() > maxChars)
        return fail(error, QStringLiteral("bad_field:") + key);
    *out = text;
    return true;
}

}

QString familyPrefix()
{
    return QStringLiteral("teammates.");
}

QString statusFor(const Config &config, const QString &sessionStatus, bool nativeInFlight)
{
    if (nativeInFlight)
        return QStringLiteral("Working");
    if (!config.enabled)
        return QStringLiteral("Paused");
    if (sessionStatus == QLatin1String("running"))
        return QStringLiteral("Working");
    if (sessionStatus == QLatin1String("queued"))
        return QStringLiteral("Queued");
    if (sessionStatus == QLatin1String("waiting"))
        return QStringLiteral("Waiting");
    if (config.schedule.active &&
        config.schedule.mode != forkmesh::teammates::kScheduleOnDemand)
        return QStringLiteral("Scheduled");
    for (const forkmesh::teammates::LogMonitor &monitor : config.logMonitors)
        if (monitor.active)
            return QStringLiteral("Scheduled");
    return QStringLiteral("Idle");
}

bool statusIsWorking(const QString &status)
{
    return status == QLatin1String("Working") || status == QLatin1String("Queued") ||
           status == QLatin1String("Waiting");
}

QJsonObject teammateRow(const Config &config, const QString &status, const QDateTime &now)
{
    QJsonObject schedule = config.schedule.toJson();
    schedule.insert(QStringLiteral("human"), forkmesh::teammates::humanSchedule(config.schedule));
    schedule.insert(QStringLiteral("next"), forkmesh::teammates::humanNextInvocation(config, now));
    schedule.insert(QStringLiteral("progress"), forkmesh::teammates::scheduleProgress(config, now));
    int monitors = 0;
    for (const forkmesh::teammates::LogMonitor &monitor : config.logMonitors)
        monitors += monitor.active ? 1 : 0;
    const forkmesh::teammates::AgentChoice primary =
        forkmesh::teammates::agentChoiceFor(config);
    return {{QStringLiteral("id"), config.id},
            {QStringLiteral("name"), config.name},
            {QStringLiteral("avatar"), config.avatar},
            {QStringLiteral("specialty"), config.specialty.left(kMaxTextField)},
            {QStringLiteral("focus"), config.focus.left(kMaxTextField)},
            {QStringLiteral("provider"), primary.provider},
            {QStringLiteral("model"), primary.model},
            {QStringLiteral("status"), status},
            {QStringLiteral("enabled"), config.enabled},
            {QStringLiteral("schedule"), schedule},
            {QStringLiteral("skills"), QJsonArray::fromStringList(config.skills.mid(0, kMaxList))},
            {QStringLiteral("monitors"), monitors},
            {QStringLiteral("lastRunAtMs"), double(config.lastRunAtMs)},
            {QStringLiteral("lastActive"),
             forkmesh::teammates::humanLastActive(config.lastRunAtMs, now)},
            {QStringLiteral("lastSessionId"), config.lastSessionId},
            {QStringLiteral("lastTrigger"), config.lastTrigger.left(kMaxTextField)},
            {QStringLiteral("workflow"), config.workflow},
            {QStringLiteral("marketingChannels"),
             QJsonArray::fromStringList(config.marketingChannels)},
            {QStringLiteral("improvementCycles"), config.improvementCycles}};
}

QJsonObject teammateDetail(const Config &config, const QString &status,
                           const QString &latestOutput, const QDateTime &now)
{
    QJsonObject row = teammateRow(config, status, now);
    QJsonArray monitors;
    for (const forkmesh::teammates::LogMonitor &monitor : config.logMonitors)
        monitors.append(QJsonObject{{QStringLiteral("badge"), monitor.badge.left(80)},
                                    {QStringLiteral("threshold"), monitor.threshold},
                                    {QStringLiteral("taskType"), monitor.taskType},
                                    {QStringLiteral("active"), monitor.active}});
    QStringList permissions = config.permissions.values();
    permissions.sort();
    QJsonArray permissionRows;
    for (const QString &permission : permissions)
        permissionRows.append(QJsonObject{
            {QStringLiteral("id"), permission},
            {QStringLiteral("label"), forkmesh::teammates::permissionLabel(permission)}});
    row.insert(QStringLiteral("description"), config.description.left(2000));
    row.insert(QStringLiteral("instructions"), config.instructions.left(kMaxLongText));
    row.insert(QStringLiteral("logMonitors"), monitors);
    row.insert(QStringLiteral("permissions"), permissionRows);
    row.insert(QStringLiteral("relayChannel"), config.relayChannel);
    row.insert(QStringLiteral("selfImproving"), config.selfImproving);
    row.insert(QStringLiteral("improvementNotes"), lastItems(config.improvementNotes, 10, 400));
    row.insert(QStringLiteral("activity"), lastItems(config.activity, 15, 300));
    row.insert(QStringLiteral("shortcut"), config.shortcut);
    row.insert(QStringLiteral("mention"), forkmesh::teammates::mentionHandle(config));
    row.insert(QStringLiteral("latestOutput"), boundedOutput(latestOutput, 6000));
    return row;
}

QStringList editableFields()
{
    return {QStringLiteral("name"),         QStringLiteral("avatar"),
            QStringLiteral("specialty"),    QStringLiteral("focus"),
            QStringLiteral("description"),  QStringLiteral("instructions"),
            QStringLiteral("skills"),       QStringLiteral("schedule"),
            QStringLiteral("relayChannel"),
            QStringLiteral("enabled"),      QStringLiteral("selfImproving")};
}

bool applyEdit(Config *config, const QJsonObject &changes, QString *error)
{
    if (!config)
        return fail(error, QStringLiteral("bad_params"));
    if (changes.isEmpty())
        return fail(error, QStringLiteral("no_changes"));
    const QStringList allowed = editableFields();
    for (auto it = changes.begin(); it != changes.end(); ++it)
        if (!allowed.contains(it.key()) && !retiredAgentField(it.key()))
            return fail(error, QStringLiteral("field_not_editable:") + it.key());
    Config next = *config;
    QString text;
    if (changes.contains(QStringLiteral("name"))) {
        if (!readText(changes, QStringLiteral("name"), 80, true, &text, error))
            return false;
        next.name = text;
    }
    if (changes.contains(QStringLiteral("avatar"))) {
        if (!readText(changes, QStringLiteral("avatar"), 32, true, &text, error) ||
            !matches(QStringLiteral("^[a-z0-9-]+$"), text))
            return fail(error, QStringLiteral("bad_field:avatar"));
        next.avatar = text;
    }
    struct TextField {
        QString key;
        int limit;
        QString *target;
    };
    const TextField textFields[] = {
        {QStringLiteral("specialty"), kMaxTextField, &next.specialty},
        {QStringLiteral("focus"), kMaxTextField, &next.focus},
        {QStringLiteral("description"), 2000, &next.description},
        {QStringLiteral("instructions"), kMaxLongText, &next.instructions},
    };
    for (const TextField &field : textFields) {
        if (!changes.contains(field.key))
            continue;
        if (!readText(changes, field.key, field.limit, false, &text, error))
            return false;
        *field.target = text;
    }
    if (changes.contains(QStringLiteral("skills"))) {
        const QJsonValue value = changes.value(QStringLiteral("skills"));
        if (!value.isArray() || value.toArray().size() > kMaxList)
            return fail(error, QStringLiteral("bad_field:skills"));
        QStringList skills;
        for (const QJsonValue &item : value.toArray()) {
            const QString skill = item.toString().trimmed();
            if (!item.isString() || skill.isEmpty() || skill.size() > 100)
                return fail(error, QStringLiteral("bad_field:skills"));
            if (!skills.contains(skill))
                skills.append(skill);
        }
        next.skills = skills;
    }
    if (changes.contains(QStringLiteral("schedule"))) {
        const QJsonObject schedule = changes.value(QStringLiteral("schedule")).toObject();
        const QString mode = schedule.value(QStringLiteral("mode")).toString(next.schedule.mode);
        if (!scheduleModes().contains(mode))
            return fail(error, QStringLiteral("bad_field:schedule"));
        next.schedule.mode = mode;
        if (schedule.contains(QStringLiteral("time"))) {
            const QTime time =
                QTime::fromString(schedule.value(QStringLiteral("time")).toString(), QStringLiteral("HH:mm"));
            if (!time.isValid())
                return fail(error, QStringLiteral("bad_field:schedule"));
            next.schedule.time = time;
        }
        if (schedule.contains(QStringLiteral("days"))) {
            QList<int> days;
            for (const QJsonValue &item : schedule.value(QStringLiteral("days")).toArray()) {
                const int day = item.toInt(0);
                if (day < 1 || day > 7)
                    return fail(error, QStringLiteral("bad_field:schedule"));
                if (!days.contains(day))
                    days.append(day);
            }
            std::sort(days.begin(), days.end());
            next.schedule.days = days;
        }
        if (schedule.contains(QStringLiteral("events"))) {
            QStringList events;
            const QStringList known = forkmesh::teammates::knownTaskTypes();
            for (const QJsonValue &item : schedule.value(QStringLiteral("events")).toArray()) {
                const QString event = forkmesh::teammates::normalizedTaskType(item.toString());
                if (!known.contains(event))
                    return fail(error, QStringLiteral("bad_field:schedule"));
                if (!events.contains(event))
                    events.append(event);
            }
            next.schedule.events = events;
        }
        if (schedule.contains(QStringLiteral("active"))) {
            if (!schedule.value(QStringLiteral("active")).isBool())
                return fail(error, QStringLiteral("bad_field:schedule"));
            next.schedule.active = schedule.value(QStringLiteral("active")).toBool();
        }
    }
    if (changes.contains(QStringLiteral("relayChannel"))) {
        if (!readText(changes, QStringLiteral("relayChannel"), 65, false, &text, error) ||
            !(text.isEmpty() || matches(QStringLiteral("^#[a-z0-9_-]{1,64}$"), text)))
            return fail(error, QStringLiteral("bad_field:relayChannel"));
        next.relayChannel = text;
    }
    struct FlagField {
        QString key;
        bool *target;
    };
    const FlagField flagFields[] = {
        {QStringLiteral("enabled"), &next.enabled},
        {QStringLiteral("selfImproving"), &next.selfImproving},
    };
    for (const FlagField &field : flagFields) {
        if (!changes.contains(field.key))
            continue;
        if (!changes.value(field.key).isBool())
            return fail(error, QStringLiteral("bad_field:") + field.key);
        *field.target = changes.value(field.key).toBool();
    }
    *config = next;
    return true;
}

QString fingerprint(const QString &text)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

QString fingerprint(const QJsonObject &object)
{
    return fingerprint(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

QStringList mutatingActions()
{
    return {QStringLiteral("runTeam"),        QStringLiteral("run"),
            QStringLiteral("briefing"),       QStringLiteral("enableSchedule"),
            QStringLiteral("disableSchedules"), QStringLiteral("grade"),
            QStringLiteral("prompt"),         QStringLiteral("stop"),
            QStringLiteral("continue"),       QStringLiteral("edit"),
            QStringLiteral("create")};
}

QString subjectFor(const QString &action, const QJsonObject &params, QString *error)
{
    const auto failWith = [error](const char *why) {
        if (error)
            *error = QString::fromLatin1(why);
        return QString();
    };
    const QString id = params.value(QStringLiteral("id")).toString().trimmed();
    if (action == QLatin1String("runTeam") || action == QLatin1String("disableSchedules"))
        return QStringLiteral("team");
    if (action == QLatin1String("briefing")) {
        if (id.isEmpty())
            return QStringLiteral("team");
        return validId(id) ? id : failWith("bad_id");
    }
    if (action == QLatin1String("run") || action == QLatin1String("stop") ||
        action == QLatin1String("enableSchedule"))
        return validId(id) ? id : failWith("bad_id");
    if (action == QLatin1String("grade")) {
        const int session = params.value(QStringLiteral("session")).toInt(0);
        const int grade = params.value(QStringLiteral("grade")).toInt(-1);
        if (session <= 0 || grade < 0 || grade > 5)
            return failWith("bad_grade");
        return QStringLiteral("%1:%2").arg(session).arg(grade);
    }
    if (action == QLatin1String("prompt") || action == QLatin1String("continue")) {
        if (!validId(id))
            return failWith("bad_id");
        const QString text = params.value(QStringLiteral("text")).toString().trimmed();
        if (action == QLatin1String("prompt") && text.isEmpty())
            return failWith("empty_prompt");
        if (text.size() > kMaxPrompt)
            return failWith("prompt_too_long");
        return id + QLatin1Char('|') + fingerprint(text);
    }
    if (action == QLatin1String("edit") || action == QLatin1String("create")) {
        const QJsonObject changes = params.value(QStringLiteral("changes")).toObject();
        if (changes.isEmpty())
            return failWith("no_changes");
        Config probe;
        probe.name = QStringLiteral("probe");
        QString why;
        if (!applyEdit(&probe, changes, &why)) {
            if (error)
                *error = why;
            return {};
        }
        if (action == QLatin1String("create")) {
            if (changes.value(QStringLiteral("name")).toString().trimmed().isEmpty())
                return failWith("bad_field:name");
            return QStringLiteral("new|") + fingerprint(changes);
        }
        if (!validId(id))
            return failWith("bad_id");
        return id + QLatin1Char('|') + fingerprint(changes);
    }
    return failWith("unsupported_action");
}

QString actionSummary(const QString &action, const QString &subject, const QString &name)
{
    const QString who = name.isEmpty() ? subject.section(QLatin1Char('|'), 0, 0) : name;
    if (action == QLatin1String("runTeam"))
        return QStringLiteral("Run every available special agent's configured work now, in one "
                              "shared special agents session.");
    if (action == QLatin1String("run"))
        return QStringLiteral("Run %1 now on its configured work.").arg(who);
    if (action == QLatin1String("briefing"))
        return subject == QLatin1String("team")
                   ? QStringLiteral("Generate today's briefing: every special agent reports "
                                    "on the repository without changing it.")
                   : QStringLiteral("Refresh %1's part of today's briefing.").arg(who);
    if (action == QLatin1String("enableSchedule"))
        return QStringLiteral("Turn %1's schedule on.").arg(who);
    if (action == QLatin1String("disableSchedules"))
        return QStringLiteral("Pause every automatic special agent schedule in this repository. "
                              "One-time work and log monitors stay available.");
    if (action == QLatin1String("grade"))
        return QStringLiteral("Grade the task %1 out of 5.")
            .arg(subject.section(QLatin1Char(':'), 1, 1));
    if (action == QLatin1String("prompt"))
        return QStringLiteral("Send this prompt to %1.").arg(who);
    if (action == QLatin1String("stop"))
        return QStringLiteral("Stop %1's running work. Its branch and worktree are kept.")
            .arg(who);
    if (action == QLatin1String("continue"))
        return QStringLiteral("Continue %1's last session.").arg(who);
    if (action == QLatin1String("edit"))
        return QStringLiteral("Save these changes to %1. Log monitors and permissions stay as "
                              "they are.")
            .arg(who);
    if (action == QLatin1String("create"))
        return QStringLiteral("Add a new special agent named %1 to this repository.").arg(name);
    return {};
}

QJsonObject teamStats(const QList<Config> &teammates, const QList<TaskRecord> &tasks,
                      int maxTasks)
{
    struct Tally {
        int runs = 0;
        int succeeded = 0;
        int failed = 0;
        int graded = 0;
        int gradeTotal = 0;
        double cost = 0;
    };
    QHash<QString, Tally> tallies;
    for (const TaskRecord &task : tasks) {
        QStringList ids = task.teammateIds;
        if (!task.teammateId.isEmpty() && !ids.contains(task.teammateId))
            ids.append(task.teammateId);
        for (const QString &id : ids) {
            Tally &tally = tallies[id];
            ++tally.runs;
            tally.succeeded += task.status == QLatin1String("success") ? 1 : 0;
            tally.failed += task.status == QLatin1String("failed") ? 1 : 0;
            if (task.grade > 0) {
                ++tally.graded;
                tally.gradeTotal += task.grade;
            }
            tally.cost += task.costUsd / qMax<qsizetype>(1, ids.size());
        }
    }
    QJsonArray rows;
    for (const Config &config : teammates) {
        const Tally tally = tallies.value(config.id);
        rows.append(QJsonObject{
            {QStringLiteral("id"), config.id},
            {QStringLiteral("name"), config.name},
            {QStringLiteral("avatar"), config.avatar},
            {QStringLiteral("runs"), tally.runs},
            {QStringLiteral("succeeded"), tally.succeeded},
            {QStringLiteral("failed"), tally.failed},
            {QStringLiteral("graded"), tally.graded},
            {QStringLiteral("averageGrade"),
             tally.graded > 0 ? double(tally.gradeTotal) / tally.graded : 0.0},
            {QStringLiteral("costUsd"), tally.cost}});
    }
    QList<TaskRecord> sorted = tasks;
    std::stable_sort(sorted.begin(), sorted.end(), [](const TaskRecord &a, const TaskRecord &b) {
        return a.finishedAtMs > b.finishedAtMs;
    });
    QJsonArray recent;
    for (const TaskRecord &task : sorted.mid(0, maxTasks)) {
        QStringList ids = task.teammateIds;
        if (!task.teammateId.isEmpty() && !ids.contains(task.teammateId))
            ids.prepend(task.teammateId);
        const bool terminal = task.status == QLatin1String("success") ||
                              task.status == QLatin1String("failed") ||
                              task.status == QLatin1String("stopped");
        recent.append(QJsonObject{{QStringLiteral("session"), task.session},
                                  {QStringLiteral("teammates"), QJsonArray::fromStringList(ids)},
                                  {QStringLiteral("title"), task.title.left(kMaxTextField)},
                                  {QStringLiteral("status"), task.status},
                                  {QStringLiteral("grade"), task.grade},
                                  {QStringLiteral("gradable"), terminal},
                                  {QStringLiteral("finishedAtMs"), double(task.finishedAtMs)},
                                  {QStringLiteral("costUsd"), task.costUsd},
                                  {QStringLiteral("repo"), task.repo}});
    }
    return {{QStringLiteral("teammates"), rows},
            {QStringLiteral("tasks"), recent},
            {QStringLiteral("totalTasks"), int(tasks.size())}};
}

QString boundedOutput(const QString &text, int maxChars)
{
    const QString trimmed = text.trimmed();
    if (trimmed.size() <= maxChars)
        return trimmed;
    return trimmed.right(maxChars);
}

QJsonObject roomMessage(const QString &id, const QString &sender, const QString &text,
                        qint64 timestampMs, bool self, const QString &threadRootId)
{
    return {{QStringLiteral("id"), id.left(128)},
            {QStringLiteral("sender"), sender.left(80)},
            {QStringLiteral("text"), text.left(4000)},
            {QStringLiteral("t"), double(timestampMs)},
            {QStringLiteral("self"), self},
            {QStringLiteral("threadRootId"), threadRootId.left(128)}};
}

} // namespace forkmesh::localcontrol::teammates
