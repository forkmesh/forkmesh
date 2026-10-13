#include "AddCoreNodeModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QtGlobal>

#include <algorithm>

namespace forkmesh::fleet {

namespace {

QJsonObject parseObject(const QByteArray &json)
{
    QByteArray text = json.trimmed();
    const int brace = text.lastIndexOf("\n{");
    if (brace >= 0)
        text = text.mid(brace + 1);
    const QJsonDocument document = QJsonDocument::fromJson(text);
    return document.isObject() ? document.object() : QJsonObject();
}

AddCoreStep::Status statusFromText(const QString &text)
{
    if (text == QLatin1String("running"))
        return AddCoreStep::Status::Running;
    if (text == QLatin1String("done"))
        return AddCoreStep::Status::Done;
    if (text == QLatin1String("failed"))
        return AddCoreStep::Status::Failed;
    return AddCoreStep::Status::Pending;
}

QList<AddCoreStep> stepsFromArray(const QJsonArray &array)
{
    QList<AddCoreStep> steps;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        AddCoreStep step;
        step.id = object.value(QStringLiteral("id")).toString();
        step.title = object.value(QStringLiteral("title")).toString();
        step.estimateSeconds = object.value(QStringLiteral("estimate")).toInt();
        if (!step.id.isEmpty())
            steps.append(step);
    }
    return steps;
}

} // namespace

AddCoreOptions parseAddCoreOptions(const QByteArray &json)
{
    AddCoreOptions options;
    const QJsonObject object = parseObject(json);
    if (object.isEmpty()) {
        options.error = QStringLiteral("the fleet tool returned no options");
        return options;
    }
    options.valid = true;
    const QString provider = object.value(QStringLiteral("provider")).toString();
    if (!provider.isEmpty())
        options.provider = provider;
    for (const QJsonValue &value : object.value(QStringLiteral("providers")).toArray())
        options.providers.append(value.toString());
    if (options.providers.isEmpty())
        options.providers.append(options.provider);
    options.priceNote = object.value(QStringLiteral("price_note")).toString();
    options.name = object.value(QStringLiteral("name")).toString();
    options.region = object.value(QStringLiteral("region")).toString();
    options.plan = object.value(QStringLiteral("plan")).toString();
    options.rev = object.value(QStringLiteral("rev")).toString();
    options.voter = object.value(QStringLiteral("voter")).toBool();
    options.voterReason = object.value(QStringLiteral("voter_reason")).toString();
    options.estimateSeconds = object.value(QStringLiteral("estimate_seconds")).toInt();
    options.error = object.value(QStringLiteral("error")).toString();
    options.plansError = object.value(QStringLiteral("plans_error")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("regions")).toArray())
        options.regions.append(value.toString());
    for (const QJsonValue &value : object.value(QStringLiteral("plans")).toArray()) {
        const QJsonObject item = value.toObject();
        AddCorePlan plan;
        plan.id = item.value(QStringLiteral("id")).toString();
        plan.ramMb = item.value(QStringLiteral("ram")).toInt();
        plan.vcpus = item.value(QStringLiteral("vcpu_count")).toInt();
        plan.monthlyCost = item.value(QStringLiteral("monthly_cost")).toDouble();
        for (const QJsonValue &location : item.value(QStringLiteral("locations")).toArray())
            plan.locations.append(location.toString());
        if (!plan.id.isEmpty())
            options.plans.append(plan);
    }
    if (!options.region.isEmpty() && !options.regions.contains(options.region))
        options.regions.prepend(options.region);
    const QJsonObject unfinished = object.value(QStringLiteral("unfinished")).toObject();
    options.unfinishedName = unfinished.value(QStringLiteral("name")).toString();
    const QJsonObject failed = unfinished.value(QStringLiteral("failed")).toObject();
    options.unfinishedFailedStep = failed.value(QStringLiteral("step")).toString();
    options.unfinishedError = failed.value(QStringLiteral("error")).toString();
    return options;
}

QString stepDisplayText(const AddCoreStep &step)
{
    QString mark;
    switch (step.status) {
    case AddCoreStep::Status::Pending:
        mark = QStringLiteral("○");
        break;
    case AddCoreStep::Status::Running:
        mark = QStringLiteral("◔");
        break;
    case AddCoreStep::Status::Done:
        mark = QStringLiteral("✓");
        break;
    case AddCoreStep::Status::Failed:
        mark = QStringLiteral("✗");
        break;
    }
    QString text = QStringLiteral("%1  %2").arg(mark, step.title);
    if (step.status == AddCoreStep::Status::Done && step.seconds > 0.0)
        text += QStringLiteral("  (%1)").arg(formatDuration(int(step.seconds + 0.5)));
    else if (step.status != AddCoreStep::Status::Done && step.estimateSeconds > 0)
        text += step.estimateSeconds < 60
                    ? QStringLiteral("  (~%1 s)").arg(step.estimateSeconds)
                    : QStringLiteral("  (%1)").arg(formatDuration(step.estimateSeconds));
    if (step.status == AddCoreStep::Status::Failed && !step.error.isEmpty())
        text += QStringLiteral("\n      ") + step.error;
    return text;
}

QList<AddCorePlan> plansInRegion(const AddCoreOptions &options, const QString &region)
{
    QList<AddCorePlan> out;
    for (const AddCorePlan &plan : options.plans) {
        if (plan.locations.contains(region, Qt::CaseInsensitive))
            out.append(plan);
    }
    return out;
}

const AddCorePlan *findPlan(const AddCoreOptions &options, const QString &planId)
{
    for (const AddCorePlan &plan : options.plans) {
        if (plan.id == planId)
            return &plan;
    }
    return nullptr;
}

QString formatMonthlyCost(double dollars)
{
    if (dollars <= 0.0)
        return QStringLiteral("price unknown");
    return QStringLiteral("$%1/month").arg(QString::number(dollars, 'f', 2));
}

QString formatDuration(int seconds)
{
    if (seconds <= 0)
        return QStringLiteral("unknown");
    if (seconds < 60)
        return QStringLiteral("%1 s").arg(seconds);
    const int minutes = (seconds + 30) / 60;
    return QStringLiteral("about %1 min").arg(minutes);
}

QString planLabel(const AddCorePlan &plan)
{
    QStringList parts{plan.id};
    if (plan.vcpus > 0)
        parts.append(QStringLiteral("%1 vCPU").arg(plan.vcpus));
    if (plan.ramMb > 0) {
        parts.append(plan.ramMb % 1024 == 0
                         ? QStringLiteral("%1 GB").arg(plan.ramMb / 1024)
                         : QStringLiteral("%1 MB").arg(plan.ramMb));
    }
    parts.append(formatMonthlyCost(plan.monthlyCost));
    return parts.join(QStringLiteral(" · "));
}

QString confirmationText(const AddCoreOptions &options, const QString &region,
                         const QString &plan)
{
    const QString name = options.name.isEmpty() ? QStringLiteral("the new core")
                                                : options.name;
    const QString label = options.name.isEmpty()
                              ? QStringLiteral("fmnext-core-N")
                              : QStringLiteral("fmnext-core-%1").arg(options.name.mid(1));
    const AddCorePlan *chosen = findPlan(options, plan);
    const QString cost = chosen ? formatMonthlyCost(chosen->monthlyCost)
                                : QStringLiteral("price unknown");
    QStringList lines;
    lines << QStringLiteral("This will:");
    const QString cloud = providerDisplayName(options.provider);
    lines << QStringLiteral("• create a new %5 VM %1 in %2 (%3, %4), billed to "
                            "the ForkMesh %5 account;")
                 .arg(label, region, plan.isEmpty() ? QStringLiteral("cheapest plan") : plan,
                      cost, cloud);
    if (options.provider == QLatin1String("aws"))
        lines << QStringLiteral("• on AWS: tags Name=%1 and forkmesh-next=1, Debian 13, "
                                "IMDSv2 only, an encrypted gp3 disk and a security group "
                                "that mirrors the node firewall; no reverse DNS;")
                     .arg(label);
    lines << QStringLiteral("• add %1 to every core's WireGuard mesh, one core at a "
                            "time (each is drained from DNS for about a minute while it "
                            "is updated);")
                 .arg(name);
    lines << QStringLiteral("• join the rqlite cluster as a %1: %2;")
                 .arg(options.voter ? QStringLiteral("voter") : QStringLiteral("non-voter"),
                      options.voterReason);
    lines << QStringLiteral("• install release %1 (the one every core serves), sync "
                            "every served repository from the other cores and take the "
                            "IDS baseline;")
                 .arg(options.rev.isEmpty() ? QStringLiteral("(current)") : options.rev);
    lines << QStringLiteral("• put %1 into DNS (app and world) only after its health "
                            "probes pass;")
                 .arg(name);
    lines << QStringLiteral("• commit deploy/fleet.toml to fleet/add-core-%1 for you "
                            "to merge (nothing is pushed).")
                 .arg(name);
    lines << QString();
    lines << QStringLiteral("Estimated time: %1. A failure leaves %2 out of DNS, and Retry "
                            "resumes from the step that failed.")
                 .arg(formatDuration(options.estimateSeconds), name);
    return lines.join(QLatin1Char('\n'));
}

QString providerDisplayName(const QString &provider)
{
    const QString value = provider.trimmed().toLower();
    if (value.isEmpty() || value == QLatin1String("vultr"))
        return QStringLiteral("Vultr");
    if (value == QLatin1String("aws"))
        return QStringLiteral("AWS");
    return provider;
}

QStringList addCoreOptionsArguments(const QString &script, const QString &provider)
{
    QStringList arguments{script, QStringLiteral("add-core-options"), QStringLiteral("--json")};
    if (!provider.isEmpty() && provider != QLatin1String("vultr"))
        arguments << QStringLiteral("--provider") << provider;
    return arguments;
}

QStringList addCoreArguments(const QString &script, const QString &region,
                             const QString &plan, const QString &name, bool resume,
                             const QString &provider)
{
    QStringList arguments{script, QStringLiteral("add-core"), QStringLiteral("--execute")};
    if (resume) {
        if (!name.isEmpty())
            arguments << QStringLiteral("--name") << name;
        return arguments;
    }
    if (!provider.isEmpty() && provider != QLatin1String("vultr"))
        arguments << QStringLiteral("--provider") << provider;
    if (!region.isEmpty())
        arguments << QStringLiteral("--region") << region;
    if (!plan.isEmpty())
        arguments << QStringLiteral("--plan") << plan;
    return arguments;
}

bool isFleetInstanceHost(const QString &name, const QString &address,
                         const QStringList &fleetAddresses)
{
    if (name.trimmed().startsWith(QLatin1String("fmnext-"), Qt::CaseInsensitive))
        return true;
    const QString trimmed = address.trimmed();
    return !trimmed.isEmpty() && fleetAddresses.contains(trimmed);
}

QStringList systemdRunArguments(const QString &unit, const QString &logPath,
                                const QString &workingDirectory, const QString &program,
                                const QStringList &arguments,
                                const QList<QPair<QString, QString>> &environment)
{
    QStringList out{QStringLiteral("--user"), QStringLiteral("--collect"),
                    QStringLiteral("--quiet"), QStringLiteral("--unit=") + unit,
                    QStringLiteral("--working-directory=") + workingDirectory,
                    QStringLiteral("--property=StandardOutput=append:") + logPath,
                    QStringLiteral("--property=StandardError=append:") + logPath,
                    QStringLiteral("--setenv=PYTHONUNBUFFERED=1")};
    for (const auto &pair : environment) {
        if (!pair.first.isEmpty() && !pair.second.isEmpty())
            out.append(QStringLiteral("--setenv=%1=%2").arg(pair.first, pair.second));
    }
    out.append(program);
    out.append(arguments);
    return out;
}

QString addCoreUnitName(const QString &stamp)
{
    QString clean;
    for (const QChar c : stamp) {
        if (c.isLetterOrNumber() || c == QLatin1Char('-'))
            clean.append(c);
    }
    return QStringLiteral("fmnext-add-core-") + clean;
}

UnitState parseUnitState(const QByteArray &systemctlShow)
{
    UnitState state;
    QString active;
    QString load;
    for (const QByteArray &raw : systemctlShow.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals <= 0)
            continue;
        const QString key = line.left(equals);
        const QString value = line.mid(equals + 1);
        if (key == QLatin1String("ActiveState"))
            active = value;
        else if (key == QLatin1String("LoadState"))
            load = value;
        else if (key == QLatin1String("ExecMainStatus"))
            state.exitStatus = value.toInt();
    }
    state.known = !active.isEmpty();
    state.running = active == QLatin1String("active") || active == QLatin1String("activating") ||
                    active == QLatin1String("deactivating") ||
                    active == QLatin1String("reloading");
    if (load == QLatin1String("not-found") && !state.running)
        state.exitStatus = 0;
    return state;
}

void AddCoreRun::reset()
{
    const QList<AddCoreStep> steps = m_steps;
    const QString prefix = m_prefix;
    const QString verb = m_verb;
    const QString doneSummary = m_doneSummary;
    *this = AddCoreRun();
    m_steps = steps;
    m_prefix = prefix;
    m_verb = verb;
    m_doneSummary = doneSummary;
    for (AddCoreStep &item : m_steps) {
        item.status = AddCoreStep::Status::Pending;
        item.seconds = 0.0;
        item.error.clear();
    }
}

void AddCoreRun::setDefaultSteps(const QList<AddCoreStep> &steps)
{
    if (m_state != State::Idle || steps.isEmpty())
        return;
    m_steps = steps;
}

QList<AddCoreStep> AddCoreRun::stepsFromJson(const QByteArray &json)
{
    return stepsFromArray(parseObject(json).value(QStringLiteral("steps")).toArray());
}

AddCoreStep *AddCoreRun::step(const QString &id)
{
    for (AddCoreStep &item : m_steps) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

QString AddCoreRun::consume(const QString &chunk)
{
    m_pending += chunk;
    QString log;
    int newline = m_pending.indexOf(QLatin1Char('\n'));
    while (newline >= 0) {
        QString line = m_pending.left(newline);
        m_pending.remove(0, newline + 1);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (!handleEvent(line))
            log += line + QLatin1Char('\n');
        newline = m_pending.indexOf(QLatin1Char('\n'));
    }
    return log;
}

QString AddCoreRun::flush()
{
    if (m_pending.isEmpty())
        return {};
    const QString line = m_pending;
    m_pending.clear();
    return handleEvent(line) ? QString() : line + QLatin1Char('\n');
}

bool AddCoreRun::handleEvent(const QString &line)
{
    if (m_prefix.isEmpty() || !line.startsWith(m_prefix))
        return false;
    const QJsonObject event =
        QJsonDocument::fromJson(line.mid(m_prefix.size()).toUtf8()).object();
    if (event.isEmpty())
        return false;
    const QString kind = event.value(QStringLiteral("event")).toString();
    if (kind == QLatin1String("start")) {
        const QList<AddCoreStep> steps =
            stepsFromArray(event.value(QStringLiteral("steps")).toArray());
        if (!steps.isEmpty())
            m_steps = steps;
        m_name = event.value(QStringLiteral("name")).toString();
        m_state = State::Running;
        m_resultSeen = false;
        m_summary.clear();
        m_failedStep.clear();
        m_error.clear();
        m_notes.clear();
        return true;
    }
    if (kind == QLatin1String("step")) {
        AddCoreStep *item = step(event.value(QStringLiteral("step")).toString());
        if (!item)
            return true;
        item->status = statusFromText(event.value(QStringLiteral("status")).toString());
        if (event.contains(QStringLiteral("title")))
            item->title = event.value(QStringLiteral("title")).toString();
        if (event.contains(QStringLiteral("seconds")))
            item->seconds = event.value(QStringLiteral("seconds")).toDouble();
        item->error = event.value(QStringLiteral("error")).toString();
        if (item->status == AddCoreStep::Status::Running)
            m_state = State::Running;
        return true;
    }
    if (kind == QLatin1String("result")) {
        m_resultSeen = true;
        if (!event.value(QStringLiteral("name")).toString().isEmpty())
            m_name = event.value(QStringLiteral("name")).toString();
        if (event.value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
            m_state = State::Succeeded;
            m_summary = event.value(QStringLiteral("summary")).toString();
            m_branch = event.value(QStringLiteral("branch")).toString();
            m_commit = event.value(QStringLiteral("commit")).toString();
        } else {
            m_state = State::Failed;
            m_failedStep = event.value(QStringLiteral("step")).toString();
            m_error = event.value(QStringLiteral("error")).toString();
            m_notes.clear();
            for (const QJsonValue &note : event.value(QStringLiteral("notes")).toArray())
                m_notes.append(note.toString());
        }
        return true;
    }
    return true;
}

void AddCoreRun::markProcessEnded(int exitCode, bool crashed)
{
    if (m_resultSeen && !crashed)
        return;
    if (m_state == State::Succeeded && !crashed && exitCode == 0)
        return;
    m_state = State::Failed;
    for (AddCoreStep &item : m_steps) {
        if (item.status == AddCoreStep::Status::Running) {
            item.status = AddCoreStep::Status::Failed;
            if (m_failedStep.isEmpty())
                m_failedStep = item.id;
        }
    }
    if (m_error.isEmpty()) {
        m_error = crashed ? QStringLiteral("the fleet tool stopped unexpectedly")
                          : QStringLiteral("the fleet tool exited with code %1 before "
                                           "reporting a result")
                                .arg(exitCode);
    }
}

bool AddCoreRun::loadStatus(const QByteArray &json)
{
    const QJsonObject object = parseObject(json);
    if (object.isEmpty() || object.value(QStringLiteral("name")).toString().isEmpty())
        return false;
    m_name = object.value(QStringLiteral("name")).toString();
    m_branch = object.value(QStringLiteral("branch")).toString();
    m_commit = object.value(QStringLiteral("commit")).toString();
    const QJsonObject statuses = object.value(QStringLiteral("steps")).toObject();
    for (AddCoreStep &item : m_steps)
        item.status = statusFromText(statuses.value(item.id).toString());
    const QJsonObject failed = object.value(QStringLiteral("failed")).toObject();
    if (object.value(QStringLiteral("finished")).toBool()) {
        m_state = State::Succeeded;
        m_summary = m_doneSummary.arg(m_name);
        return true;
    }
    m_state = State::Failed;
    m_failedStep = failed.value(QStringLiteral("step")).toString();
    m_error = failed.value(QStringLiteral("error")).toString();
    for (AddCoreStep &item : m_steps) {
        if (item.status == AddCoreStep::Status::Running) {
            item.status = AddCoreStep::Status::Failed;
            if (m_failedStep.isEmpty())
                m_failedStep = item.id;
        }
        if (item.id == m_failedStep)
            item.error = m_error;
    }
    if (m_error.isEmpty())
        m_error = QStringLiteral("the last run stopped before it finished");
    return true;
}

int AddCoreRun::doneCount() const
{
    return int(std::count_if(m_steps.cbegin(), m_steps.cend(), [](const AddCoreStep &item) {
        return item.status == AddCoreStep::Status::Done;
    }));
}

int AddCoreRun::remainingEstimateSeconds() const
{
    int total = 0;
    for (const AddCoreStep &item : m_steps) {
        if (item.status != AddCoreStep::Status::Done)
            total += item.estimateSeconds;
    }
    return total;
}

QString AddCoreRun::headline() const
{
    const QString name = m_name.isEmpty() ? QStringLiteral("the new core") : m_name;
    switch (m_state) {
    case State::Idle:
        return {};
    case State::Succeeded:
        return m_summary.isEmpty() ? m_doneSummary.arg(name) : m_summary;
    case State::Failed: {
        QString title = m_failedStep;
        for (const AddCoreStep &item : m_steps) {
            if (item.id == m_failedStep && !item.title.isEmpty())
                title = item.title;
        }
        return QStringLiteral("%1 %2 failed%3: %4")
            .arg(m_verb, name, title.isEmpty() ? QString() : QStringLiteral(" at “%1”").arg(title),
                 m_error);
    }
    case State::Running:
        for (int index = 0; index < m_steps.size(); ++index) {
            if (m_steps.at(index).status == AddCoreStep::Status::Running) {
                return QStringLiteral("%1 %2: step %3 of %4, %5 (%6 left)")
                    .arg(m_verb, name)
                    .arg(index + 1)
                    .arg(m_steps.size())
                    .arg(m_steps.at(index).title, formatDuration(remainingEstimateSeconds()));
            }
        }
        return QStringLiteral("%1 %2…").arg(m_verb, name);
    }
    return {};
}

} // namespace forkmesh::fleet
