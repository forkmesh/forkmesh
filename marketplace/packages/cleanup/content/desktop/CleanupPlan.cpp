#include "CleanupPlan.h"

#include "SafeDelete.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace forkmesh::cleanup {
namespace {

QString clean(const QString &path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(QDir(path).absolutePath());
}

bool symlinkBetween(const QString &member, const QString &parent)
{
    if (QFileInfo(parent).isSymLink())
        return true;
    QString current = QFileInfo(member).absolutePath();
    while (current.size() > parent.size() && pathWithin(current, parent)) {
        if (QFileInfo(current).isSymLink())
            return true;
        const QString up = QFileInfo(current).absolutePath();
        if (up == current)
            break;
        current = up;
    }
    return false;
}

} // namespace

QString planRefusal(const Item &item, const Choice &choice)
{
    if (item.status == Status::Protected)
        return QStringLiteral("protected");
    if (item.status == Status::InUse)
        return QStringLiteral("in use");
    if (item.kind == Kind::Report)
        return QStringLiteral("report only");
    if (!item.deletable())
        return QStringLiteral("nothing here can be deleted");
    if (item.needsConfirm() && !choice.confirmed)
        return QStringLiteral("not confirmed");
    if (item.kind == Kind::Worktree && item.dirtyFiles > 0 && !choice.discardChanges)
        return QStringLiteral("has uncommitted changes; tick Discard to remove it");
    return QString();
}

Plan buildPlan(const QList<Item> &selected, const QHash<QString, Choice> &choices)
{
    Plan plan;
    for (const Item &item : selected) {
        const Choice choice = choices.value(item.id);
        const QString why = planRefusal(item, choice);
        if (!why.isEmpty()) {
            plan.skipped.append({item, why});
            continue;
        }
        Step step{item, choice};
        if (item.kind == Kind::Worktree)
            plan.worktrees.append(step);
        else if (item.kind == Kind::ActionSandbox)
            plan.sandboxes.append(step);
        else
            plan.files.append(step);
        plan.bytes += item.bytes;
    }
    return plan;
}

QString describe(const Plan &plan)
{
    QStringList lines;
    lines << QStringLiteral("%1 item%2, %3 reclaimed")
                 .arg(plan.count())
                 .arg(plan.count() == 1 ? QString() : QStringLiteral("s"), sizeText(plan.bytes));
    for (const Step &step : plan.worktrees) {
        QString line = QStringLiteral("worktree %1").arg(step.item.path);
        if (step.choice.discardChanges && step.item.dirtyFiles > 0)
            line += QStringLiteral(" (discarding %1 uncommitted files)").arg(step.item.dirtyFiles);
        if (step.choice.retireBranch && !step.item.branch.isEmpty())
            line += QStringLiteral(", branch %1 moved to refs/forkmesh/trash/cleanup/").arg(step.item.branch);
        lines << line;
    }
    for (const Step &step : plan.sandboxes)
        lines << QStringLiteral("Actions sandbox %1").arg(step.item.path);
    for (const Step &step : plan.files) {
        if (step.item.members.size() == 1 && step.item.members.first().path == step.item.path)
            lines << step.item.path;
        else
            lines << QStringLiteral("%1 entr%2 in %3")
                         .arg(step.item.members.size())
                         .arg(step.item.members.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"),
                              step.item.path);
    }
    for (const Skip &skip : plan.skipped)
        lines << QStringLiteral("kept %1: %2").arg(skip.item.title, skip.why);
    return lines.join(QLatin1Char('\n'));
}

QString memberRefusal(const Item &item, const Member &member, const Recheck &recheck)
{
    const QString parent = clean(item.parent);
    const QString path = clean(member.path);
    if (parent.isEmpty())
        return QStringLiteral("no expected parent folder");
    if (path == parent || !pathWithin(path, parent))
        return QStringLiteral("%1 is not inside %2").arg(path, parent);
    if (symlinkBetween(path, parent))
        return QStringLiteral("a link sits between %1 and %2").arg(parent, path);
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return QStringLiteral("already gone");
    const QStringList users = recheck.processes.usersOf(path);
    if (!users.isEmpty())
        return QStringLiteral("now open in %1").arg(users.mid(0, 3).join(QStringLiteral(", ")));
    for (const QString &uuid : uuidsIn(info.fileName())) {
        if (recheck.live.contains(uuid))
            return QStringLiteral("its session is live");
    }
    if (!info.isSymLink()) {
        const Usage usage = measure(path);
        if (usage.newestMs > 0 &&
            recheck.nowMs - usage.newestMs < qint64(recheck.liveMinutes) * 60 * 1000)
            return QStringLiteral("changed in the last %1 minutes").arg(recheck.liveMinutes);
    }
    return QString();
}

QString worktreeRefusal(const Item &item, const Recheck &recheck)
{
    if (item.kind != Kind::Worktree)
        return QStringLiteral("not a worktree");
    if (!QFileInfo(item.path).isDir())
        return QStringLiteral("already gone");
    const QStringList users = recheck.processes.usersOf(item.path);
    if (!users.isEmpty())
        return QStringLiteral("now open in %1").arg(users.mid(0, 3).join(QStringLiteral(", ")));
    return QString();
}

QList<Outcome> deleteFiles(const QList<Step> &steps, const Recheck &recheck,
                           const CancelFlag &cancel)
{
    QList<Outcome> outcomes;
    for (const Step &step : steps) {
        for (const Member &member : step.item.members) {
            if (cancel && cancel->load())
                return outcomes;
            Outcome outcome;
            outcome.path = clean(member.path);
            outcome.bytes = member.bytes;
            outcome.reason = step.item.reasonText();
            outcome.error = memberRefusal(step.item, member, recheck);
            if (outcome.error.isEmpty()) {
                QString why;
                outcome.removed = forkmesh::safedelete::removeTree(outcome.path,
                                                                   clean(step.item.parent), &why);
                if (!outcome.removed)
                    outcome.error = why.isEmpty() ? QStringLiteral("could not remove it") : why;
            }
            outcomes.append(outcome);
        }
    }
    return outcomes;
}

QList<Outcome> deleteSandboxes(const QList<Step> &steps, const Recheck &recheck,
                               const SandboxRemover &remove)
{
    QList<Outcome> outcomes;
    for (const Step &step : steps) {
        Outcome outcome;
        outcome.path = clean(step.item.path);
        outcome.bytes = step.item.bytes;
        outcome.reason = step.item.reasonText();
        const QStringList users = recheck.processes.usersOf(outcome.path);
        if (!users.isEmpty())
            outcome.error = QStringLiteral("now open in %1").arg(users.mid(0, 3).join(QStringLiteral(", ")));
        else if (!remove)
            outcome.error = QStringLiteral("the desktop is not available to remove it");
        else
            outcome.error = remove(clean(step.item.parent), outcome.path);
        outcome.removed = outcome.error.isEmpty();
        outcomes.append(outcome);
    }
    return outcomes;
}

qint64 freedBytes(const QList<Outcome> &outcomes)
{
    qint64 total = 0;
    for (const Outcome &outcome : outcomes) {
        if (outcome.removed)
            total += outcome.bytes;
    }
    return total;
}

QString logLine(const Outcome &outcome, qint64 nowMs)
{
    QStringList fields{
        QDateTime::fromMSecsSinceEpoch(nowMs).toString(Qt::ISODate),
        outcome.removed ? QStringLiteral("removed") : QStringLiteral("kept"),
        QString::number(outcome.bytes),
        outcome.path,
        outcome.reason,
    };
    if (!outcome.error.isEmpty())
        fields << outcome.error;
    for (QString &field : fields)
        field.replace(QLatin1Char('\t'), QLatin1Char(' ')).replace(QLatin1Char('\n'), QLatin1Char(' '));
    return fields.join(QLatin1Char('\t'));
}

bool appendLog(const QString &file, const QList<Outcome> &outcomes, qint64 nowMs)
{
    if (outcomes.isEmpty())
        return true;
    QDir().mkpath(QFileInfo(file).absolutePath());
    QFile log(file);
    if (!log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return false;
    log.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    for (const Outcome &outcome : outcomes)
        log.write(logLine(outcome, nowMs).toUtf8() + '\n');
    return true;
}

} // namespace forkmesh::cleanup
