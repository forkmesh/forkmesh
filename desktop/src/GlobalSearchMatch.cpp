#include "GlobalSearchMatch.h"

#include <QRegularExpression>

namespace forkmesh::search {

namespace {

const Category kCategories[] = {
    {"repo", "Repositories", "repo", "#d29922"},
    {"issue", "Issues", "issue-opened", "#3fb950"},
    {"pull", "Pull requests", "git-pull-request", "#a371f7"},
    {"discussion", "Discussions", "comment", "#58a6ff"},
    {"project", "Projects", "workflow", "#a371f7"},
    {"milestone", "Milestones", "graph", "#3fb950"},
    {"agent", "Agent sessions", "sparkle", "#e3b341"},
    {"branch", "Branches", "git-branch", "#58a6ff"},
    {"worktree", "Worktrees", "file-directory", "#d29922"},
    {"tag", "Tags", "tag", "#8b949e"},
    {"action", "Workflow runs", "workflow", "#3fb950"},
    {"log", "Logs", "list-unordered", "#8b949e"},
    {"commit", "Commits", "git-commit", "#8b949e"},
    {"file", "Files", "file", "#58a6ff"},
    {"code", "Code", "code", "#58a6ff"},
    {"worktree-code", "Code in worktrees", "code", "#d29922"},
};
constexpr int kCategoryCount = int(sizeof(kCategories) / sizeof(kCategories[0]));

QString commentNote(const QString &body, const QString &query)
{
    return QStringLiteral("comment: ") + snippetAround(body, query);
}

} // namespace

int categoryCount()
{
    return kCategoryCount;
}

const Category &categoryAt(int index)
{
    static const Category unknown{"", "Other", "search", "#8b949e"};
    if (index < 0 || index >= kCategoryCount)
        return unknown;
    return kCategories[index];
}

int categoryIndexOf(const QString &kind)
{
    for (int i = 0; i < kCategoryCount; ++i)
        if (kind == QLatin1String(kCategories[i].kind))
            return i;
    return kCategoryCount;
}

bool containsFold(const QString &haystack, const QString &needle)
{
    return !haystack.isEmpty() && !needle.isEmpty() &&
           haystack.contains(needle, Qt::CaseInsensitive);
}

QString snippetAround(const QString &haystack, const QString &needle)
{
    QString flat = haystack;
    flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
    flat = flat.simplified();
    const int at = flat.indexOf(needle, 0, Qt::CaseInsensitive);
    if (at < 0)
        return flat.left(120);
    const int start = qMax(0, at - 40);
    QString out = flat.mid(start, 140);
    if (start > 0)
        out.prepend(QStringLiteral("…"));
    if (start + 140 < flat.size())
        out.append(QStringLiteral("…"));
    return out;
}

StatusBadge statusBadge(const QString &kind, const QString &status)
{
    const bool open = status == QLatin1String("open");
    const bool closed = status == QLatin1String("closed");
    if (kind == QLatin1String("issue")) {
        if (open)
            return {QStringLiteral("Open"), QStringLiteral("issue-opened"),
                    QStringLiteral("#3fb950")};
        if (closed)
            return {QStringLiteral("Closed"), QStringLiteral("check-circle"),
                    QStringLiteral("#a371f7")};
    } else if (kind == QLatin1String("pull")) {
        if (open)
            return {QStringLiteral("Open"), QStringLiteral("git-pull-request"),
                    QStringLiteral("#3fb950")};
        if (status == QLatin1String("merged"))
            return {QStringLiteral("Merged"), QStringLiteral("git-merge"),
                    QStringLiteral("#a371f7")};
        if (closed)
            return {QStringLiteral("Closed"), QStringLiteral("circle-slash"),
                    QStringLiteral("#f85149")};
    } else if (kind == QLatin1String("discussion")) {
        if (open)
            return {QStringLiteral("Open"), QStringLiteral("comment"),
                    QStringLiteral("#3fb950")};
        if (closed)
            return {QStringLiteral("Closed"), QStringLiteral("check-circle"),
                    QStringLiteral("#a371f7")};
        if (status == QLatin1String("archived"))
            return {QStringLiteral("Archived"), QStringLiteral("comment"),
                    QStringLiteral("#8b949e")};
    } else if (kind == QLatin1String("project") ||
               kind == QLatin1String("milestone")) {
        const QString icon = kind == QLatin1String("project")
                                 ? QStringLiteral("workflow")
                                 : QStringLiteral("graph");
        if (open)
            return {QStringLiteral("Open"), icon, QStringLiteral("#3fb950")};
        if (closed)
            return {QStringLiteral("Closed"), icon, QStringLiteral("#a371f7")};
    }
    return {};
}

QVector<HighlightRun> highlightRuns(const QString &text, const QString &query)
{
    QVector<HighlightRun> runs;
    const QString needle = query.trimmed();
    int from = 0;
    while (!needle.isEmpty()) {
        const int at = text.indexOf(needle, from, Qt::CaseInsensitive);
        if (at < 0)
            break;
        if (at > from)
            runs.append({text.mid(from, at - from), false});
        runs.append({text.mid(at, needle.size()), true});
        from = at + needle.size();
    }
    if (from < text.size())
        runs.append({text.mid(from), false});
    return runs;
}

bool matchTitleBody(const QString &query, const QString &title,
                    const QString &body, QString *where)
{
    if (containsFold(title, query)) {
        *where = QStringLiteral("title");
        return true;
    }
    if (containsFold(body, query)) {
        *where = snippetAround(body, query);
        return true;
    }
    return false;
}

bool matchIssue(const Issue &issue, const QString &query, QString *where)
{
    if (containsFold(issue.title, query)) {
        *where = QStringLiteral("title");
        return true;
    }
    for (const IssueEvent &event : issue.events) {
        if (!containsFold(event.body, query))
            continue;
        *where = event.type == QLatin1String("open")
                     ? snippetAround(event.body, query)
                     : commentNote(event.body, query);
        return true;
    }
    return false;
}

bool matchPull(const PullRequest &pull, const QString &query, QString *where)
{
    if (matchTitleBody(query, pull.title, pull.description, where))
        return true;
    for (const PullEvent &event : pull.events) {
        if (!containsFold(event.body, query))
            continue;
        *where = commentNote(event.body, query);
        return true;
    }
    return false;
}

bool matchDiscussion(const Discussion &discussion, const QString &query,
                     QString *where)
{
    if (containsFold(discussion.title, query)) {
        *where = QStringLiteral("title");
        return true;
    }
    for (const DiscussionEvent &event : discussion.events) {
        if (!containsFold(event.body, query))
            continue;
        *where = event.type == QLatin1String("open")
                     ? snippetAround(event.body, query)
                     : commentNote(event.body, query);
        return true;
    }
    return false;
}

bool matchProject(const Project &project, const QString &query, QString *where)
{
    if (matchTitleBody(query, project.title, project.body, where))
        return true;
    for (const ProjectEvent &event : project.events) {
        if (!containsFold(event.body, query))
            continue;
        *where = commentNote(event.body, query);
        return true;
    }
    return false;
}

QVector<RecordHit> matchRepositoryRecords(
    const QString &query, const QList<Issue> &issues,
    const QList<IssueMilestone> &milestones,
    const QList<PullRequest> &pulls,
    const QList<Discussion> &discussions,
    const QList<Project> &projects, int limitPerKind)
{
    QVector<RecordHit> hits;
    if (query.trimmed().isEmpty() || limitPerKind <= 0)
        return hits;

    int count = 0;
    for (const Issue &issue : issues) {
        if (count >= limitPerKind)
            break;
        if (issue.isDeleted())
            continue;
        QString where;
        const QString identity =
            QStringLiteral("#%1 %2").arg(issue.number).arg(issue.title);
        if (!matchIssue(issue, query, &where) &&
            !containsFold(identity, query))
            continue;
        if (where.isEmpty())
            where = QStringLiteral("issue number");
        RecordHit hit;
        hit.kind = QStringLiteral("issue");
        hit.number = issue.number;
        hit.primary = identity;
        hit.detail = where;
        hit.status = issue.status;
        hits.append(std::move(hit));
        ++count;
    }

    count = 0;
    for (const IssueMilestone &milestone : milestones) {
        if (count >= limitPerKind)
            break;
        QString where;
        if (!matchTitleBody(query, milestone.title, milestone.description,
                            &where))
            continue;
        RecordHit hit;
        hit.kind = QStringLiteral("milestone");
        hit.path = milestone.title;
        hit.primary = milestone.title;
        hit.detail = where;
        hit.status = milestone.status;
        hits.append(std::move(hit));
        ++count;
    }

    count = 0;
    for (const PullRequest &pull : pulls) {
        if (count >= limitPerKind)
            break;
        QString where;
        const QString identity =
            QStringLiteral("#%1 %2").arg(pull.number).arg(pull.title);
        if (!matchPull(pull, query, &where) &&
            !containsFold(identity, query))
            continue;
        if (where.isEmpty())
            where = QStringLiteral("pull request number");
        RecordHit hit;
        hit.kind = QStringLiteral("pull");
        hit.number = pull.number;
        hit.primary = identity;
        hit.detail = where;
        hit.status = pullDisplayStatus(pull);
        hits.append(std::move(hit));
        ++count;
    }

    count = 0;
    for (const Discussion &discussion : discussions) {
        if (count >= limitPerKind)
            break;
        QString where;
        const QString identity = QStringLiteral("#%1 %2")
                                     .arg(discussion.number)
                                     .arg(discussion.title);
        if (!matchDiscussion(discussion, query, &where) &&
            !containsFold(identity, query))
            continue;
        if (where.isEmpty())
            where = QStringLiteral("discussion number");
        RecordHit hit;
        hit.kind = QStringLiteral("discussion");
        hit.number = discussion.number;
        hit.primary = identity;
        hit.detail = discussion.category.isEmpty()
                         ? where
                         : discussion.category + QStringLiteral(" · ") + where;
        hit.status = discussion.status;
        hits.append(std::move(hit));
        ++count;
    }

    count = 0;
    for (const Project &project : projects) {
        if (count >= limitPerKind)
            break;
        if (project.isDeleted())
            continue;
        QString where;
        const QString identity =
            QStringLiteral("#%1 %2").arg(project.number).arg(project.title);
        if (!matchProject(project, query, &where) &&
            !containsFold(identity, query))
            continue;
        if (where.isEmpty())
            where = QStringLiteral("project number");
        RecordHit hit;
        hit.kind = QStringLiteral("project");
        hit.number = project.number;
        hit.primary = identity;
        hit.detail = where;
        hit.status = project.status;
        hits.append(std::move(hit));
        ++count;
    }

    return hits;
}

QVector<WorktreeRecord> parseWorktreePorcelain(const QStringList &lines)
{
    QVector<WorktreeRecord> records;
    WorktreeRecord current;
    bool open = false;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("worktree "))) {
            if (open)
                records.append(current); // a new record starts; emit the last
            current = WorktreeRecord();
            current.path = line.mid(9);
            open = true;
        } else if (!open) {
            continue; // stray attribute before any "worktree " line
        } else if (line.startsWith(QLatin1String("HEAD "))) {
            current.head = line.mid(5);
        } else if (line.startsWith(QLatin1String("branch "))) {
            current.branch = line.mid(7);
            if (current.branch.startsWith(QLatin1String("refs/heads/")))
                current.branch = current.branch.mid(11);
        } else if (line == QLatin1String("detached")) {
            current.detached = true;
        }
    }
    if (open)
        records.append(current);
    return records;
}

bool isLedgerBranch(const QString &branch)
{
    QString name = branch;
    for (const QLatin1String prefix :
         {QLatin1String("refs/heads/"), QLatin1String("refs/remotes/")})
        if (name.startsWith(prefix))
            name = name.mid(prefix.size());
    // A remote-tracking name carries its remote first: origin/forkmesh/pulls.
    const int slash = name.indexOf(QLatin1Char('/'));
    const QString withoutRemote = slash < 0 ? QString() : name.mid(slash + 1);
    for (const QString &candidate : {name, withoutRemote}) {
        if (candidate == QLatin1String("forkmesh/pulls") ||
            candidate.startsWith(QLatin1String("forkmesh/pull/")))
            return true;
    }
    return false;
}

QStringList allBranchRevisionArgs(const QString &ref)
{
    QStringList args{QStringLiteral("--source"),
                     QStringLiteral("--exclude=forkmesh/pulls"),
                     QStringLiteral("--exclude=forkmesh/pull/*"),
                     QStringLiteral("--branches"),
                     QStringLiteral("--exclude=*/forkmesh/pulls"),
                     QStringLiteral("--exclude=*/forkmesh/pull/*"),
                     QStringLiteral("--remotes")};
    if (!ref.isEmpty())
        args.append(ref);
    return args;
}

CodeRow parseGrepRow(const QString &row, const QString &ref)
{
    CodeRow out;
    QString line = row;
    const QString prefix = ref + QLatin1Char(':');
    if (!ref.isEmpty() && line.startsWith(prefix))
        line = line.mid(prefix.size());
    static const QRegularExpression rowRe(QStringLiteral("^(.+?):(\\d+):(.*)$"));
    const QRegularExpressionMatch match = rowRe.match(line);
    if (!match.hasMatch())
        return out;
    out.path = match.captured(1);
    out.line = match.captured(2).toInt();
    out.text = match.captured(3).trimmed();
    out.valid = true;
    return out;
}

} // namespace forkmesh::search
