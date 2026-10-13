#include "ProjectStore.h"

#include "ForkMeshIdentity.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace {

constexpr int kGitTimeoutMs = 10000;

QString projectsRootRel() { return QStringLiteral(".forkmesh/projects"); }

QString projectJsonFileName(int number)
{
    return QStringLiteral("project-%1.json").arg(number);
}

bool runGit(const QString &dir, const QStringList &args, QByteArray *output = nullptr,
            QString *errText = nullptr, int timeoutMs = kGitTimeoutMs)
{
    QProcess process;
    process.start("git", QStringList{"-C", dir} + args);
    if (!process.waitForFinished(timeoutMs)) {
        if (errText)
            *errText = QStringLiteral("git timed out");
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errText)
            *errText =
                QString::fromUtf8(process.readAllStandardError()).trimmed().left(200);
        return false;
    }
    return true;
}

bool runGitInput(const QString &dir, const QStringList &args, const QByteArray &input,
                 QByteArray *output, QString *errText = nullptr,
                 int timeoutMs = kGitTimeoutMs)
{
    QProcess process;
    process.start("git", QStringList{"-C", dir} + args);
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        if (errText)
            *errText = QStringLiteral("git timed out");
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errText)
            *errText =
                QString::fromUtf8(process.readAllStandardError()).trimmed().left(200);
        return false;
    }
    return true;
}

QJsonArray fromIntList(const QList<int> &list)
{
    QJsonArray array;
    for (int item : list)
        array.append(item);
    return array;
}

QList<int> toIntList(const QJsonArray &array)
{
    QList<int> out;
    for (const QJsonValue &value : array)
        out.append(value.toInt());
    return out;
}

QList<int> sortedIssueList(QList<int> issues)
{
    std::sort(issues.begin(), issues.end());
    issues.erase(std::unique(issues.begin(), issues.end()), issues.end());
    return issues;
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool writeTextFile(const QString &path, const QString &text, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Could not write %1").arg(path);
        return false;
    }
    file.write(text.toUtf8());
    return true;
}

} // namespace

// ---- ProjectEvent (JSON is the wire format) ---------------------------------

QJsonObject ProjectEvent::toJson() const
{
    QJsonObject obj{{"type", type}, {"id", id}, {"author", author},
                    {"authorName", authorName},
                    {"ts", double(ts)}};
    if (type == "open" || type == "title")
        obj.insert("title", title);
    if (type == "open" || type == "edit")
        obj.insert("body", body);
    if (type == "status")
        obj.insert("status", status);
    if (type == "dates") {
        obj.insert("startDate", double(startDate));
        obj.insert("endDate", double(endDate));
    }
    if (type == "milestone")
        obj.insert("milestone", milestone);
    if (type == "issues")
        obj.insert("issues", fromIntList(issues));
    if (type == "delete")
        obj.insert("target", target);
    obj.insert("sig", sig);
    return obj;
}

ProjectEvent ProjectEvent::fromJson(const QJsonObject &obj)
{
    ProjectEvent ev;
    ev.type = obj.value("type").toString();
    ev.id = obj.value("id").toString();
    ev.author = obj.value("author").toString();
    ev.authorName = obj.value("authorName").toString();
    ev.ts = qint64(obj.value("ts").toDouble());
    ev.title = obj.value("title").toString();
    ev.body = obj.value("body").toString();
    ev.status = obj.value("status").toString();
    ev.startDate = qint64(obj.value("startDate").toDouble());
    ev.endDate = qint64(obj.value("endDate").toDouble());
    ev.milestone = obj.value("milestone").toString();
    ev.issues = toIntList(obj.value("issues").toArray());
    ev.target = obj.value("target").toString();
    ev.sig = obj.value("sig").toString();
    return ev;
}


QJsonObject Project::toJson() const
{
    QJsonArray eventsArray;
    qint64 updatedAt = createdAt;
    for (const ProjectEvent &ev : events) {
        eventsArray.append(ev.toJson());
        if (ev.ts > updatedAt)
            updatedAt = ev.ts;
    }
    return {{"schema", "forkmesh-project-v1"}, {"number", number}, {"title", title},
            {"body", body}, {"status", status},
            {"startDate", double(startDate)}, {"endDate", double(endDate)},
            {"milestone", milestone}, {"issues", fromIntList(issues)},
            {"createdAt", double(createdAt)}, {"updatedAt", double(updatedAt)},
            {"author", author}, {"authorName", authorName},
            {"events", eventsArray}};
}

Project Project::fromJson(const QJsonObject &obj)
{
    Project project;
    project.number = obj.value("number").toInt();
    project.title = obj.value("title").toString();
    project.body = obj.value("body").toString();
    project.status = obj.value("status").toString("open");
    project.startDate = qint64(obj.value("startDate").toDouble());
    project.endDate = qint64(obj.value("endDate").toDouble());
    project.milestone = obj.value("milestone").toString();
    project.issues = toIntList(obj.value("issues").toArray());
    project.createdAt = qint64(obj.value("createdAt").toDouble());
    project.author = obj.value("author").toString();
    project.authorName = obj.value("authorName").toString();
    for (const QJsonValue &value : obj.value("events").toArray())
        project.events.append(ProjectEvent::fromJson(value.toObject()));
    return project;
}

bool Project::isDeleted() const
{
    for (const ProjectEvent &ev : events)
        if (ev.type == "delete" && ev.target == "self")
            return true;
    return false;
}


ProjectStore::ProjectStore(QString workTreePath, QString mirrorPath,
                           const ForkMeshIdentity *identity, QString authorName)
    : m_workTree(std::move(workTreePath)), m_mirror(std::move(mirrorPath)),
      m_identity(identity), m_authorName(std::move(authorName))
{
}

bool ProjectStore::canWrite() const
{
    if (m_workTree.isEmpty() || !m_identity || !m_identity->isValid())
        return false;
    return QFileInfo::exists(m_workTree + "/.git");
}

QString ProjectStore::projectsDir() const
{
    return QDir(m_workTree).filePath(projectsRootRel());
}

QString ProjectStore::projectDir(int number) const
{
    return QDir(projectsDir()).filePath(QString::number(number));
}

QString ProjectStore::projectFilePath(int number) const
{
    return QDir(projectDir(number)).filePath(projectJsonFileName(number));
}

// ---- Signing -----------------------------------------------------------------

QString ProjectStore::contentForSigning(const ProjectEvent &ev)
{
    const QChar nul(QChar::Null);
    if (ev.type == "open")
        return ev.title + nul + ev.body;
    if (ev.type == "title")
        return ev.title;
    if (ev.type == "edit")
        return ev.body;
    if (ev.type == "status")
        return ev.status;
    if (ev.type == "dates")
        return QString::number(ev.startDate) + nul + QString::number(ev.endDate);
    if (ev.type == "milestone")
        return ev.milestone;
    if (ev.type == "issues") {
        QStringList parts;
        for (int n : sortedIssueList(ev.issues))
            parts << QString::number(n);
        return parts.join(",");
    }
    if (ev.type == "delete")
        return ev.target;
    return QString();
}

QByteArray ProjectStore::canonicalString(int number, const ProjectEvent &ev)
{
    const QByteArray contentHash =
        QCryptographicHash::hash(contentForSigning(ev).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    QByteArray canonical = "forkmesh-project-event-v1\n";
    canonical += ev.type.toUtf8() + "\n";
    canonical += QByteArray::number(number) + "\n";
    canonical += ev.author.toUtf8() + "\n";
    canonical += QByteArray::number(ev.ts) + "\n";
    canonical += contentHash;
    return canonical;
}

ProjectEvent ProjectStore::makeSignedEvent(int number, ProjectEvent ev) const
{
    if (ev.id.isEmpty())
        ev.id = newId();
    ev.author = m_identity ? m_identity->publicKey() : QString();
    if (ev.authorName.isEmpty())
        ev.authorName = m_authorName;
    if (ev.ts == 0)
        ev.ts = QDateTime::currentMSecsSinceEpoch();
    ev.sig = m_identity ? m_identity->signData(canonicalString(number, ev)) : QString();
    return ev;
}


QList<Project> ProjectStore::loadAll(QString *error) const
{
    if (!canWrite())
        return loadFromMirror(error);

    QList<Project> projects;
    const QStringList entries =
        QDir(projectsDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &entry : entries) {
        bool numeric = false;
        const int number = entry.toInt(&numeric);
        if (!numeric)
            continue;
        Project project;
        if (readProjectFile(number, project) && !project.isDeleted())
            projects.append(project);
    }

    std::sort(projects.begin(), projects.end(),
              [](const Project &a, const Project &b) { return a.number < b.number; });
    return projects;
}

bool ProjectStore::readProjectFile(int number, Project &out) const
{
    QFile jsonFile(projectFilePath(number));
    if (!jsonFile.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    out = Project::fromJson(doc.object());
    if (out.number <= 0)
        out.number = number;
    recomputeMetadata(out);
    return true;
}


QString ProjectStore::mirrorRef() const
{
    if (m_mirrorRefResolved)
        return m_cachedMirrorRef;
    m_mirrorRefResolved = true;

    QByteArray output;
    if (runGit(m_mirror, {"rev-parse", "--verify", "-q", "HEAD"}, &output) &&
        !output.trimmed().isEmpty()) {
        m_cachedMirrorRef = QStringLiteral("HEAD");
    } else if (runGit(m_mirror,
                      {"for-each-ref", "--format=%(refname)", "--count=1",
                       "refs/heads/"},
                      &output)) {
        m_cachedMirrorRef = QString::fromUtf8(output).trimmed();
    }
    return m_cachedMirrorRef;
}

QList<Project> ProjectStore::loadFromMirror(QString *error) const
{
    QList<Project> projects;
    if (m_mirror.isEmpty()) {
        if (error)
            *error = QStringLiteral("No local mirror to read projects from.");
        return projects;
    }
    const QString ref = mirrorRef();
    if (ref.isEmpty())
        return projects;

    QByteArray listing;
    if (!runGit(m_mirror, {"ls-tree", "-r", ref, projectsRootRel() + "/"}, &listing))
        return projects;

    QMap<int, QString> projectJsonOids;
    QSet<QString> wantedOids;
    const QString prefix = projectsRootRel() + QLatin1Char('/');
    for (const QString &line :
         QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts)) {
        const int tab = line.indexOf('\t');
        if (tab < 0)
            continue;
        const QStringList meta = line.left(tab).split(' ', Qt::SkipEmptyParts);
        if (meta.size() < 3 || meta.at(1) != QStringLiteral("blob"))
            continue;
        const QString path = line.mid(tab + 1);
        if (!path.startsWith(prefix))
            continue;
        const QString rel = path.mid(prefix.size());
        const int slash = rel.indexOf('/');
        if (slash < 0)
            continue;
        bool numeric = false;
        const int number = rel.left(slash).toInt(&numeric);
        if (!numeric)
            continue;
        if (rel.mid(slash + 1) == projectJsonFileName(number)) {
            const QString oid = meta.at(2);
            projectJsonOids.insert(number, oid);
            wantedOids.insert(oid);
        }
    }
    if (wantedOids.isEmpty())
        return projects;

    QByteArray batchInput;
    for (const QString &oid : std::as_const(wantedOids))
        batchInput += oid.toUtf8() + '\n';
    QByteArray batch;
    if (!runGitInput(m_mirror, {"cat-file", "--batch"}, batchInput, &batch))
        return projects;

    QHash<QString, QByteArray> contentByOid;
    contentByOid.reserve(wantedOids.size());
    for (int pos = 0; pos < batch.size();) {
        const int nl = batch.indexOf('\n', pos);
        if (nl < 0)
            break;
        const QList<QByteArray> header = batch.mid(pos, nl - pos).split(' ');
        pos = nl + 1;
        if (header.size() < 3) // "<oid> missing" or malformed — no body follows
            continue;
        bool sizeOk = false;
        const int size = header.at(2).toInt(&sizeOk);
        if (!sizeOk || pos + size > batch.size())
            break;
        contentByOid.insert(QString::fromUtf8(header.at(0)), batch.mid(pos, size));
        pos += size + 1; // skip body and its trailing newline
    }

    for (auto it = projectJsonOids.constBegin(); it != projectJsonOids.constEnd();
         ++it) {
        if (!contentByOid.contains(it.value()))
            continue;
        QJsonParseError parseError;
        const QJsonDocument doc =
            QJsonDocument::fromJson(contentByOid.value(it.value()), &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        Project project = Project::fromJson(doc.object());
        if (project.number <= 0)
            project.number = it.key();
        recomputeMetadata(project);
        if (!project.isDeleted())
            projects.append(project);
    }

    std::sort(projects.begin(), projects.end(),
              [](const Project &a, const Project &b) { return a.number < b.number; });
    return projects;
}


bool ProjectStore::writeProjectFile(const Project &project, QString *error) const
{
    QDir().mkpath(projectDir(project.number));
    if (project.events.isEmpty()) {
        if (error)
            *error = QStringLiteral("Project #%1 has no events.").arg(project.number);
        return false;
    }
    Project stored = project;
    recomputeMetadata(stored);
    const QByteArray bytes =
        QJsonDocument(stored.toJson()).toJson(QJsonDocument::Indented);
    if (!writeTextFile(projectFilePath(project.number), QString::fromUtf8(bytes),
                       error))
        return false;
    return true;
}

void ProjectStore::recomputeMetadata(Project &project) const
{
    for (const ProjectEvent &ev : project.events) {
        if (ev.type == "open") {
            project.title = ev.title;
            project.body = ev.body;
        } else if (ev.type == "title" && !ev.title.isEmpty())
            project.title = ev.title;
        else if (ev.type == "edit")
            project.body = ev.body;
        else if (ev.type == "status")
            project.status = ev.status;
        else if (ev.type == "dates") {
            project.startDate = ev.startDate;
            project.endDate = ev.endDate;
        } else if (ev.type == "milestone")
            project.milestone = ev.milestone;
        else if (ev.type == "issues")
            project.issues = sortedIssueList(ev.issues);
    }
}

bool ProjectStore::commit(const QString &message, QString *error) const
{
    QString err;
    QStringList paths{projectsRootRel()};
    if (!runGit(m_workTree, QStringList{"add", "-A", "--"} + paths, nullptr, &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    QByteArray out;
    if (!runGit(m_workTree, QStringList{"commit", "-m", message, "--"} + paths, &out,
                &err)) {
        if (err.contains("nothing to commit") || err.isEmpty())
            return true;
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    return true;
}

int ProjectStore::nextNumber() const
{
    int max = 0;
    const QStringList entries =
        QDir(projectsDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        const int n = entry.toInt(&numeric);
        if (numeric && n > max)
            max = n;
    }
    return max + 1;
}


int ProjectStore::createProject(const QString &title, const QString &body,
                                qint64 startDate, qint64 endDate,
                                const QString &milestone, const QList<int> &issues,
                                QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return -1;
    }
    const int number = nextNumber();
    QDir().mkpath(projectDir(number));

    ProjectEvent ev;
    ev.type = "open";
    ev.id = QStringLiteral("open-%1").arg(number);
    ev.title = title;
    ev.body = body;
    ev = makeSignedEvent(number, ev);

    Project project;
    project.number = number;
    project.title = title;
    project.body = body;
    project.status = "open";
    project.createdAt = ev.ts;
    project.author = ev.author;
    project.authorName = ev.authorName;
    project.events.append(ev);

    if (startDate != 0 || endDate != 0) {
        ProjectEvent dates;
        dates.type = "dates";
        dates.startDate = startDate;
        dates.endDate = endDate;
        project.events.append(makeSignedEvent(number, dates));
    }
    if (!milestone.isEmpty()) {
        ProjectEvent ms;
        ms.type = "milestone";
        ms.milestone = milestone;
        project.events.append(makeSignedEvent(number, ms));
    }
    if (!issues.isEmpty()) {
        ProjectEvent linked;
        linked.type = "issues";
        linked.issues = sortedIssueList(issues);
        project.events.append(makeSignedEvent(number, linked));
    }

    if (!writeProjectFile(project, error))
        return -1;
    if (!commit(QStringLiteral("projects: #%1 %2").arg(number).arg(title), error))
        return -1;
    return number;
}

bool ProjectStore::setTitle(int number, const QString &newTitle, QString *error)
{
    if (!canWrite())
        return false;
    Project project;
    if (!readProjectFile(number, project))
        return false;
    ProjectEvent ev;
    ev.type = "title";
    ev.title = newTitle.trimmed();
    ev = makeSignedEvent(number, ev);
    project.events.append(ev);
    if (!writeProjectFile(project, error))
        return false;
    return commit(QStringLiteral("projects: #%1 retitle").arg(number), error);
}

bool ProjectStore::setBody(int number, const QString &newBody, QString *error)
{
    if (!canWrite())
        return false;
    Project project;
    if (!readProjectFile(number, project))
        return false;
    ProjectEvent ev;
    ev.type = "edit";
    ev.body = newBody;
    ev = makeSignedEvent(number, ev);
    project.events.append(ev);
    if (!writeProjectFile(project, error))
        return false;
    return commit(QStringLiteral("projects: #%1 edit").arg(number), error);
}

bool ProjectStore::setStatus(int number, const QString &status, QString *error)
{
    if (!canWrite())
        return false;
    Project project;
    if (!readProjectFile(number, project))
        return false;
    ProjectEvent ev;
    ev.type = "status";
    ev.status = status;
    ev = makeSignedEvent(number, ev);
    project.events.append(ev);
    if (!writeProjectFile(project, error))
        return false;
    return commit(QStringLiteral("projects: #%1 %2").arg(number).arg(status), error);
}

bool ProjectStore::setDates(int number, qint64 startDate, qint64 endDate,
                            QString *error)
{
    if (!canWrite())
        return false;
    Project project;
    if (!readProjectFile(number, project))
        return false;
    ProjectEvent ev;
    ev.type = "dates";
    ev.startDate = startDate;
    ev.endDate = endDate;
    ev = makeSignedEvent(number, ev);
    project.events.append(ev);
    if (!writeProjectFile(project, error))
        return false;
    return commit(QStringLiteral("projects: #%1 dates").arg(number), error);
}

bool ProjectStore::setMilestone(int number, const QString &milestone, QString *error)
{
    if (!canWrite())
        return false;
    Project project;
    if (!readProjectFile(number, project))
        return false;
    ProjectEvent ev;
    ev.type = "milestone";
    ev.milestone = milestone;
    ev = makeSignedEvent(number, ev);
    project.events.append(ev);
    if (!writeProjectFile(project, error))
        return false;
    return commit(QStringLiteral("projects: #%1 milestone").arg(number), error);
}

bool ProjectStore::setIssues(int number, const QList<int> &issues, QString *error)
{
    if (!canWrite())
        return false;
    Project project;
    if (!readProjectFile(number, project))
        return false;
    ProjectEvent ev;
    ev.type = "issues";
    ev.issues = sortedIssueList(issues);
    ev = makeSignedEvent(number, ev);
    project.events.append(ev);
    if (!writeProjectFile(project, error))
        return false;
    return commit(QStringLiteral("projects: #%1 issues").arg(number), error);
}

bool ProjectStore::tombstoneProject(int number, QString *error)
{
    if (!canWrite())
        return false;
    Project project;
    if (!readProjectFile(number, project))
        return false;
    if (project.isDeleted())
        return true; // already tombstoned; nothing to do
    ProjectEvent ev;
    ev.type = "delete";
    ev.target = "self";
    ev = makeSignedEvent(number, ev);
    project.events.append(ev);
    if (!writeProjectFile(project, error))
        return false;
    return commit(QStringLiteral("projects: #%1 delete").arg(number), error);
}
