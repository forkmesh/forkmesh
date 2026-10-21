#include "DiscussionStore.h"
#include "SafeDelete.h"

#include "EventSigning.h"
#include "ForkMeshIdentity.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace {

constexpr int kGitTimeoutMs = 15000;

bool runGit(const QString &dir, const QStringList &args, QByteArray *output = nullptr,
            QString *errText = nullptr, const QByteArray *input = nullptr)
{
    QProcess process;
    process.start("git", QStringList{"-C", dir} + args);
    if (input) {
        process.write(*input);
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(kGitTimeoutMs)) {
        if (errText)
            *errText = QStringLiteral("git timed out");
        return false;
    }
    if (output)
        *output = process.readAllStandardOutput();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (errText)
            *errText =
                QString::fromUtf8(process.readAllStandardError()).trimmed().left(300);
        return false;
    }
    return true;
}

QString stripEdgeNewlines(QString text)
{
    while (text.startsWith('\n') || text.startsWith('\r'))
        text.remove(0, 1);
    while (text.endsWith('\n') || text.endsWith('\r'))
        text.chop(1);
    return text;
}

struct FrontMatter {
    QHash<QString, QString> values;
    QString body;
    QString get(const QString &key) const { return values.value(key); }
    qint64 num(const QString &key) const { return values.value(key).toLongLong(); }
};

FrontMatter parseFrontMatter(const QByteArray &bytes)
{
    FrontMatter fm;
    const QString text = QString::fromUtf8(bytes);
    const QStringList lines = text.split('\n');
    if (lines.isEmpty() || lines.first().trimmed() != "---") {
        fm.body = text;
        return fm;
    }

    int i = 1;
    for (; i < lines.size() && lines.at(i).trimmed() != "---"; ++i) {
        const QString &line = lines.at(i);
        const int sep = line.indexOf(": ");
        if (sep >= 0)
            fm.values.insert(line.left(sep), line.mid(sep + 2));
        else if (line.endsWith(':'))
            fm.values.insert(line.left(line.size() - 1), QString());
    }
    fm.body = stripEdgeNewlines(lines.mid(i + 1).join('\n'));
    return fm;
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

const QRegularExpression &commentFileRe()
{
    static const QRegularExpression re(QStringLiteral("^\\d{4}-comment\\.md$"));
    return re;
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

void appendEventFields(QStringList &lines, const DiscussionEvent &ev)
{
    lines << "type: " + ev.type;
    lines << "id: " + ev.id;
    lines << "author: " + ev.author;
    lines << "authorName: " + ev.authorName;
    lines << "ts: " + QString::number(ev.ts);
    if (ev.type == "open") {
        lines << "title: " + ev.title;
        lines << "category: " + ev.category;
    }
    if (ev.type == "answer") {
        lines << "target: " + ev.target;
        lines << "targetAuthor: " + ev.targetAuthor;
    }
    lines << "sig: " + ev.sig;
}

DiscussionEvent eventFromFrontMatter(const FrontMatter &fm)
{
    DiscussionEvent ev;
    ev.type = fm.get("type");
    ev.id = fm.get("id");
    ev.author = fm.get("author");
    ev.authorName = fm.get("authorName");
    ev.ts = fm.num("ts");
    ev.title = fm.get("title");
    ev.category = fm.get("category");
    ev.target = fm.get("target");
    ev.targetAuthor = fm.get("targetAuthor");
    ev.sig = fm.get("sig");
    ev.body = fm.body;
    return ev;
}

} // namespace

QJsonObject DiscussionEvent::toJson() const
{
    QJsonObject obj{{"type", type},       {"id", id},
                    {"author", author},   {"authorName", authorName},
                    {"ts", double(ts)},   {"body", body},
                    {"sig", sig}};
    if (type == "open") {
        obj.insert("title", title);
        obj.insert("category", category);
    }
    if (type == "answer" || type == "delete")
        obj.insert("target", target);
    if (type == "status")
        obj.insert("status", status);
    return obj;
}

DiscussionEvent DiscussionEvent::fromJson(const QJsonObject &obj)
{
    DiscussionEvent ev;
    ev.type = obj.value("type").toString();
    ev.id = obj.value("id").toString();
    ev.author = obj.value("author").toString();
    ev.authorName = obj.value("authorName").toString();
    ev.ts = qint64(obj.value("ts").toDouble());
    ev.title = obj.value("title").toString();
    ev.category = obj.value("category").toString();
    ev.target = obj.value("target").toString();
    ev.status = obj.value("status").toString();
    ev.body = obj.value("body").toString();
    ev.sig = obj.value("sig").toString();
    return ev;
}

qint64 Discussion::updatedAt() const
{
    qint64 latest = createdAt;
    for (const DiscussionEvent &ev : events)
        latest = std::max(latest, ev.ts);
    return latest;
}

int Discussion::commentCount() const
{
    int count = 0;
    for (const DiscussionEvent &ev : events)
        if (ev.type == "comment")
            ++count;
    return count;
}

bool Discussion::answerable() const
{
    return DiscussionStore::normalizedCategory(category) ==
           DiscussionStore::answerCategory();
}

QString Discussion::answerId() const
{
    if (!answerable() || answer.target.isEmpty())
        return QString();
    for (const DiscussionEvent &ev : events)
        if (ev.type == QLatin1String("comment") && ev.id == answer.target)
            return ev.id;
    return QString();
}

DiscussionStore::DiscussionStore(QString workTreePath, QString mirrorPath,
                                 const ForkMeshIdentity *identity,
                                 QString authorName)
    : m_workTree(std::move(workTreePath)), m_mirror(std::move(mirrorPath)),
      m_identity(identity), m_authorName(std::move(authorName))
{
}

bool DiscussionStore::canWrite() const
{
    if (m_workTree.isEmpty() || !m_identity || !m_identity->isValid())
        return false;
    return QFileInfo::exists(m_workTree + "/.git");
}

QString DiscussionStore::discussionsDir() const
{
    return m_workTree + "/.forkmesh/discussions";
}

QString DiscussionStore::discussionDir(int number) const
{
    return discussionsDir() + "/" + QString::number(number);
}

QStringList DiscussionStore::categories()
{
    return {"Announcements", "Ideas", "Q&A", "Show and tell", "Maintainer notes"};
}

QString DiscussionStore::answerCategory()
{
    return QStringLiteral("Q&A");
}

QString DiscussionStore::normalizedCategory(const QString &category)
{
    const QString trimmed = category.simplified();
    for (const QString &known : categories()) {
        if (QString::compare(trimmed, known, Qt::CaseInsensitive) == 0)
            return known;
    }

    const QString folded = trimmed.toCaseFolded();
    if (folded == "announcement")
        return QStringLiteral("Announcements");
    if (folded == "idea")
        return QStringLiteral("Ideas");
    if (folded == "qa" || folded == "q and a" || folded == "q-and-a")
        return QStringLiteral("Q&A");
    if (folded == "show-and-tell" || folded == "show & tell")
        return QStringLiteral("Show and tell");
    if (folded == "maintainer-notes" || folded == "maintainer note")
        return QStringLiteral("Maintainer notes");
    return QStringLiteral("Ideas");
}

QString DiscussionStore::contentForSigning(const DiscussionEvent &ev)
{
    const QChar nul(QChar::Null);
    if (ev.type == "open")
        return ev.title + nul + ev.body + nul + ev.category;
    if (ev.type == "comment")
        return ev.body;
    if (ev.type == "answer")
        return ev.target;
    // Owner-only events a browser queues through the relay (the website's
    // Close/Archive and reply delete): the new status, or the reply's id.
    if (ev.type == "status")
        return ev.status;
    if (ev.type == "delete")
        return ev.target;
    return QString();
}

void DiscussionStore::setSigningRepository(const QString &owner, const QString &name)
{
    m_signingRepoKey = forkmesh::eventsigning::repoKey(owner, name);
}

QByteArray DiscussionStore::canonicalString(int number, const DiscussionEvent &ev,
                                            const QString &repoKey)
{
    const QByteArray contentHash =
        QCryptographicHash::hash(contentForSigning(ev).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    QByteArray canonical = forkmesh::eventsigning::prefix("discussion-event", repoKey);
    canonical += ev.type.toUtf8() + "\n";
    canonical += QByteArray::number(number) + "\n";
    canonical += ev.author.toUtf8() + "\n";
    canonical += QByteArray::number(ev.ts) + "\n";
    canonical += contentHash;
    return canonical;
}

DiscussionEvent DiscussionStore::makeSignedEvent(int number, DiscussionEvent ev) const
{
    if (ev.id.isEmpty())
        ev.id = ev.type == "open" && number > 0 ? QStringLiteral("open-%1").arg(number)
                                                 : newId();
    if (ev.type == "open")
        ev.category = normalizedCategory(ev.category);
    ev.author = m_identity ? m_identity->publicKey() : QString();
    if (ev.authorName.isEmpty())
        ev.authorName = m_authorName;
    if (ev.ts == 0)
        ev.ts = QDateTime::currentMSecsSinceEpoch();
    ev.sig = m_identity ? m_identity->signData(canonicalString(number, ev, m_signingRepoKey))
                        : QString();
    return ev;
}

QList<Discussion> DiscussionStore::loadAll(QString *error) const
{
    if (!canWrite())
        return loadFromMirror(error);

    QList<Discussion> discussions;
    const QStringList entries =
        QDir(discussionsDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                         QDir::Name);
    for (const QString &entry : entries) {
        bool numeric = false;
        const int number = entry.toInt(&numeric);
        if (!numeric)
            continue;
        Discussion discussion;
        if (readDiscussionFile(number, discussion))
            discussions.append(discussion);
    }
    std::sort(discussions.begin(), discussions.end(),
              [](const Discussion &a, const Discussion &b) {
                  return a.number < b.number;
              });
    return discussions;
}

bool DiscussionStore::readDiscussionFile(int number, Discussion &out) const
{
    QFile file(discussionDir(number) + "/discussion.md");
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const FrontMatter fm = parseFrontMatter(file.readAll());

    out = Discussion();
    out.number = number;
    out.title = fm.get("title");
    out.category = fm.get("category");
    out.status = fm.get("status");
    if (out.status != QLatin1String("closed") &&
        out.status != QLatin1String("archived"))
        out.status = QStringLiteral("open");
    out.createdAt = fm.num("createdAt");
    out.author = fm.get("author");
    out.authorName = fm.get("authorName");

    DiscussionEvent open = eventFromFrontMatter(fm);
    open.type = "open";
    open.title = out.title;
    open.category = out.category;
    out.events.append(open);

    QStringList eventFiles =
        QDir(discussionDir(number)).entryList(QDir::Files, QDir::Name);
    eventFiles.sort();
    for (const QString &name : eventFiles) {
        if (!commentFileRe().match(name).hasMatch())
            continue;
        QFile ef(discussionDir(number) + "/" + name);
        if (ef.open(QIODevice::ReadOnly))
            out.events.append(eventFromFrontMatter(parseFrontMatter(ef.readAll())));
    }
    QFile answerFile(discussionDir(number) + "/answer.md");
    if (answerFile.open(QIODevice::ReadOnly))
        out.answer = eventFromFrontMatter(parseFrontMatter(answerFile.readAll()));
    return true;
}

bool DiscussionStore::writeAnswerFile(int number, const DiscussionEvent &answer,
                                      QString *error) const
{
    QStringList lines;
    lines << "---";
    appendEventFields(lines, answer);
    lines << "---";
    lines << "";
    return writeTextFile(discussionDir(number) + "/answer.md",
                         lines.join('\n') + "\n", error);
}

bool DiscussionStore::mayMarkAnswer(const Discussion &discussion) const
{
    if (!discussion.answerable() || !m_identity || !m_identity->isValid())
        return false;
    return canWrite() || discussion.author == m_identity->publicKey();
}

bool DiscussionStore::applyAnswer(const Discussion &discussion,
                                  DiscussionEvent answer, QString *error) const
{
    if (!discussion.answerable()) {
        if (error)
            *error = QStringLiteral("Only Q&A discussions have answers.");
        return false;
    }
    answer.targetAuthor.clear();
    if (!answer.target.isEmpty()) {
        for (const DiscussionEvent &ev : discussion.events)
            if (ev.type == QLatin1String("comment") && ev.id == answer.target)
                answer.targetAuthor = ev.author;
        if (answer.targetAuthor.isEmpty()) {
            if (error)
                *error = QStringLiteral("That comment is not in this discussion.");
            return false;
        }
    }
    if (!writeAnswerFile(discussion.number, answer, error))
        return false;
    return commit(answer.target.isEmpty()
                      ? QStringLiteral("discussion #%1: clear answer")
                            .arg(discussion.number)
                      : QStringLiteral("discussion #%1: answer").arg(discussion.number),
                  error);
}

bool DiscussionStore::markAnswer(int number, const QString &commentId,
                                 QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    Discussion discussion;
    if (!readDiscussionFile(number, discussion)) {
        if (error)
            *error = QStringLiteral("Discussion #%1 not found.").arg(number);
        return false;
    }
    DiscussionEvent ev;
    ev.type = "answer";
    ev.target = commentId;
    return applyAnswer(discussion, makeSignedEvent(number, ev), error);
}

bool DiscussionStore::writeDiscussionFile(const Discussion &discussion,
                                          QString *error) const
{
    QDir().mkpath(discussionDir(discussion.number));
    if (discussion.events.isEmpty())
        return false;
    const DiscussionEvent &open = discussion.events.first();

    QStringList lines;
    lines << "---";
    lines << "schema: forkmesh-discussion-v1";
    lines << "number: " + QString::number(discussion.number);
    lines << "title: " + open.title;
    lines << "category: " + open.category;
    lines << "status: " + (discussion.status == QLatin1String("closed") ||
                              discussion.status == QLatin1String("archived")
                          ? discussion.status
                          : QStringLiteral("open"));
    lines << "createdAt: " + QString::number(discussion.createdAt);
    lines << "author: " + open.author;
    lines << "authorName: " + open.authorName;
    lines << "type: open";
    lines << "id: " + open.id;
    lines << "ts: " + QString::number(open.ts);
    lines << "sig: " + open.sig;
    lines << "---";
    lines << "";
    if (!writeTextFile(discussionDir(discussion.number) + "/discussion.md",
                       lines.join('\n') + "\n" + open.body + "\n", error))
        return false;

    int index = 2;
    for (int i = 1; i < discussion.events.size(); ++i) {
        const DiscussionEvent &ev = discussion.events.at(i);
        if (ev.type != "comment")
            continue;
        QStringList evLines;
        evLines << "---";
        appendEventFields(evLines, ev);
        evLines << "---";
        evLines << "";
        const QString name = QStringLiteral("%1-comment.md")
                                 .arg(index, 4, 10, QChar('0'));
        if (!writeTextFile(discussionDir(discussion.number) + "/" + name,
                           evLines.join('\n') + "\n" + ev.body + "\n", error))
            return false;
        ++index;
    }
    return true;
}

int DiscussionStore::nextNumber() const
{
    int max = 0;
    const QStringList entries =
        QDir(discussionsDir()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        bool numeric = false;
        const int n = entry.toInt(&numeric);
        if (numeric && n > max)
            max = n;
    }
    return max + 1;
}

bool DiscussionStore::commit(const QString &message, QString *error) const
{
    QString err;
    if (!runGit(m_workTree, {"add", "-A", "--", ".forkmesh/discussions"}, nullptr,
                &err)) {
        if (error)
            *error = "git add failed: " + err;
        return false;
    }
    if (!runGit(m_workTree, {"commit", "-m", message, "--", ".forkmesh/discussions"},
                nullptr, &err)) {
        if (err.contains("nothing to commit") || err.isEmpty())
            return true;
        if (error)
            *error = "git commit failed: " + err;
        return false;
    }
    return true;
}

int DiscussionStore::createDiscussion(const QString &title, const QString &body,
                                      const QString &category, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return -1;
    }
    const QString cleanTitle = title.trimmed();
    if (cleanTitle.isEmpty()) {
        if (error)
            *error = QStringLiteral("Discussion title is required.");
        return -1;
    }

    const int number = nextNumber();
    DiscussionEvent ev;
    ev.type = "open";
    ev.title = cleanTitle;
    ev.category = normalizedCategory(category);
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedEvent(number, ev);

    Discussion discussion;
    discussion.number = number;
    discussion.title = ev.title;
    discussion.category = ev.category;
    discussion.createdAt = ev.ts;
    discussion.author = ev.author;
    discussion.authorName = ev.authorName;
    discussion.events.append(ev);

    if (!writeDiscussionFile(discussion, error))
        return -1;
    if (!commit(QStringLiteral("discussion #%1: %2").arg(number).arg(ev.title), error))
        return -1;
    return number;
}

bool DiscussionStore::addComment(int number, const QString &body, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }

    Discussion discussion;
    if (!readDiscussionFile(number, discussion)) {
        if (error)
            *error = QStringLiteral("Discussion #%1 not found.").arg(number);
        return false;
    }
    if (discussion.status != QLatin1String("open")) {
        if (error)
            *error = QStringLiteral("Discussion #%1 is %2.")
                         .arg(number)
                         .arg(discussion.status);
        return false;
    }

    DiscussionEvent ev;
    ev.type = "comment";
    ev.body = stripEdgeNewlines(body);
    ev = makeSignedEvent(number, ev);
    discussion.events.append(ev);
    if (!writeDiscussionFile(discussion, error))
        return false;
    return commit(QStringLiteral("discussion #%1: comment").arg(number), error);
}

bool DiscussionStore::deleteComment(int number, const QString &eventId,
                                    QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    if (eventId.isEmpty()) {
        if (error)
            *error = QStringLiteral("Missing comment id.");
        return false;
    }

    Discussion discussion;
    if (!readDiscussionFile(number, discussion)) {
        if (error)
            *error = QStringLiteral("Discussion #%1 not found.").arg(number);
        return false;
    }

    int removed = -1;
    for (int i = 1; i < discussion.events.size(); ++i) {
        const DiscussionEvent &ev = discussion.events.at(i);
        if (ev.type == "comment" && ev.id == eventId) {
            removed = i;
            break;
        }
    }
    if (removed < 0) {
        if (error)
            *error = QStringLiteral("Comment not found.");
        return false;
    }
    discussion.events.removeAt(removed);

    QDir dir(discussionDir(number));
    const QStringList names = dir.entryList(QDir::Files, QDir::Name);
    for (const QString &name : names) {
        if (commentFileRe().match(name).hasMatch())
            dir.remove(name);
    }

    if (!writeDiscussionFile(discussion, error))
        return false;
    return commit(QStringLiteral("discussion #%1: delete comment").arg(number),
                  error);
}

bool DiscussionStore::setStatus(int number, const QString &status, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    if (status != QLatin1String("open") && status != QLatin1String("closed") &&
        status != QLatin1String("archived")) {
        if (error)
            *error = QStringLiteral("Unsupported discussion status.");
        return false;
    }

    Discussion discussion;
    if (!readDiscussionFile(number, discussion)) {
        if (error)
            *error = QStringLiteral("Discussion #%1 not found.").arg(number);
        return false;
    }
    if (discussion.status == status)
        return true;
    discussion.status = status;
    if (!writeDiscussionFile(discussion, error))
        return false;
    return commit(QStringLiteral("discussion #%1: %2").arg(number).arg(status),
                  error);
}

bool DiscussionStore::deleteDiscussion(int number, QString *error)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }
    const QString path = discussionDir(number);
    if (!QDir(path).exists()) {
        if (error)
            *error = QStringLiteral("Discussion #%1 not found.").arg(number);
        return false;
    }
    if (!forkmesh::safedelete::removeTree(path, discussionsDir())) {
        if (error)
            *error = QStringLiteral("Could not remove discussion #%1.").arg(number);
        return false;
    }
    return commit(QStringLiteral("discussion #%1: delete").arg(number), error);
}

bool DiscussionStore::applyRemoteEvent(int number, const DiscussionEvent &ev,
                                       const QString &titleIfNew, QString *error,
                                       bool ownerApproved)
{
    if (!canWrite()) {
        if (error)
            *error = QStringLiteral("This repository is read-only on this node.");
        return false;
    }

    if (ev.type == "open") {
        const QList<Discussion> all = loadAll();
        for (const Discussion &discussion : all) {
            for (const DiscussionEvent &existing : discussion.events) {
                if (existing.type != "open")
                    continue;
                const bool sameSig = !ev.sig.isEmpty() && existing.sig == ev.sig;
                const bool sameAuthorTs = ev.sig.isEmpty() &&
                                          existing.author == ev.author &&
                                          existing.ts == ev.ts &&
                                          existing.title == ev.title;
                if (sameSig || sameAuthorTs)
                    return true;
            }
        }

        Discussion probe;
        if (number <= 0 || readDiscussionFile(number, probe))
            number = nextNumber();

        Discussion discussion;
        discussion.number = number;
        discussion.title = ev.title;
        discussion.category = ev.category;
        discussion.createdAt = ev.ts;
        discussion.author = ev.author;
        discussion.authorName = ev.authorName;
        discussion.events.append(ev);
        if (!writeDiscussionFile(discussion, error))
            return false;
        return commit(QStringLiteral("discussion #%1: opened (from %2)")
                          .arg(number)
                          .arg(ev.authorName.isEmpty() ? ev.author.left(8)
                                                       : ev.authorName),
                      error);
    }

    if (ev.type == "answer") {
        Discussion discussion;
        if (!readDiscussionFile(number, discussion)) {
            if (error)
                *error = QStringLiteral("Discussion #%1 not found.").arg(number);
            return false;
        }
        if (ev.author.isEmpty() ||
            (!ownerApproved && ev.author != discussion.author)) {
            if (error)
                *error = QStringLiteral("Only the discussion's author can mark its answer.");
            return false;
        }
        if (!discussion.answer.sig.isEmpty() && discussion.answer.sig == ev.sig)
            return true;
        const auto ms = [](qint64 ts) { return ts > 0 && ts < 10000000000LL ? ts * 1000 : ts; };
        if (ms(discussion.answer.ts) > ms(ev.ts))
            return true;
        return applyAnswer(discussion, ev, error);
    }

    if (ev.type == "status" || ev.type == "delete") {
        // Repository-owned state: the relay and the mirror gateway queue these
        // only with the owner's session, and mark them ownerApproved. Without
        // that they are refused here exactly like on a managed mirror
        // (writeDiscussionModeration in server/intake.go).
        if (!ownerApproved) {
            if (error)
                *error = QStringLiteral("Only the repository owner can moderate a discussion.");
            return false;
        }
        Discussion discussion;
        const bool wholeDiscussion =
            ev.type == "delete" && ev.target == QLatin1String("#discussion");
        if (!readDiscussionFile(number, discussion)) {
            if (wholeDiscussion)
                return true; // already deleted: consume the redelivery
            if (error)
                *error = QStringLiteral("Discussion #%1 not found.").arg(number);
            return false;
        }
        if (wholeDiscussion)
            return deleteDiscussion(number, error);
        if (ev.type == "status")
            return setStatus(number, ev.status, error);
        for (int i = 1; i < discussion.events.size(); ++i)
            if (discussion.events.at(i).type == "comment" &&
                discussion.events.at(i).id == ev.target)
                return deleteComment(number, ev.target, error);
        return true; // already removed: consume the redelivery
    }

    if (ev.type != "comment") {
        if (error)
            *error = QStringLiteral("Unsupported discussion event type.");
        return false;
    }

    Discussion discussion;
    const bool exists = readDiscussionFile(number, discussion);
    if (exists && discussion.status != QLatin1String("open")) {
        // Closed and archived discussions are read-only (addComment refuses
        // the same reply locally). Consume it so the inbox does not retry it.
        return true;
    }
    if (!exists) {
        discussion.number = number;
        discussion.title = titleIfNew;
        discussion.category = QStringLiteral("Ideas");
        discussion.createdAt = ev.ts;
        discussion.author = ev.author;
        discussion.authorName = ev.authorName;

        DiscussionEvent placeholder;
        placeholder.type = "open";
        placeholder.id = QStringLiteral("open-%1").arg(number);
        placeholder.title = titleIfNew;
        placeholder.category = discussion.category;
        placeholder.author = ev.author;
        placeholder.authorName = ev.authorName;
        placeholder.ts = ev.ts;
        discussion.events.append(placeholder);
    }

    if (!ev.id.isEmpty()) {
        for (const DiscussionEvent &existing : std::as_const(discussion.events))
            if (existing.id == ev.id)
                return true;
    }
    discussion.events.append(ev);
    if (!writeDiscussionFile(discussion, error))
        return false;
    return commit(QStringLiteral("discussion #%1: comment (from %2)")
                      .arg(number)
                      .arg(ev.authorName.isEmpty() ? ev.author.left(8)
                                                   : ev.authorName),
                  error);
}

QString DiscussionStore::mirrorRef() const
{
    QByteArray output;
    if (runGit(m_mirror, {"rev-parse", "--verify", "-q", "HEAD"}, &output) &&
        !output.trimmed().isEmpty())
        return QStringLiteral("HEAD");
    if (runGit(m_mirror,
               {"for-each-ref", "--format=%(refname)", "--count=1", "refs/heads/"},
               &output)) {
        const QString ref = QString::fromUtf8(output).trimmed();
        if (!ref.isEmpty())
            return ref;
    }
    return QString();
}

QByteArray DiscussionStore::showFromMirror(const QString &repoRelPath, bool *ok) const
{
    const QString ref = mirrorRef();
    QByteArray output;
    const bool good = !ref.isEmpty() &&
                      runGit(m_mirror, {"show", ref + ":" + repoRelPath}, &output);
    if (ok)
        *ok = good;
    return good ? output : QByteArray();
}

QList<Discussion> DiscussionStore::loadFromMirror(QString *error) const
{
    QList<Discussion> discussions;
    if (m_mirror.isEmpty()) {
        if (error)
            *error = QStringLiteral("No local mirror to read discussions from.");
        return discussions;
    }
    const QString ref = mirrorRef();
    if (ref.isEmpty())
        return discussions;

    // One recursive listing and one `cat-file --batch` read the whole tree,
    // instead of an ls-tree plus a `git show` per discussion and per comment.
    QByteArray listing;
    if (!runGit(m_mirror, {"ls-tree", "-r", ref, ".forkmesh/discussions/"}, &listing))
        return discussions;

    struct MirrorFiles {
        QString discussionOid;
        QString answerOid;
        QMap<QString, QString> commentOids; // file name -> blob oid, name-sorted
    };
    QMap<QString, MirrorFiles> filesByDir; // numeric directory name -> files
    QByteArray batchInput;
    const QString prefix = QStringLiteral(".forkmesh/discussions/");
    for (const QString &line :
         QString::fromUtf8(listing).split('\n', Qt::SkipEmptyParts)) {
        const int tab = line.indexOf('\t');
        if (tab < 0)
            continue;
        const QStringList meta = line.left(tab).split(' ', Qt::SkipEmptyParts);
        if (meta.size() < 3 || meta.at(1) != QLatin1String("blob"))
            continue;
        const QString path = line.mid(tab + 1);
        if (!path.startsWith(prefix))
            continue;
        const QString rel = path.mid(prefix.size());
        const int slash = rel.indexOf('/');
        if (slash < 0)
            continue;
        const QString fname = rel.mid(slash + 1);
        if (fname.contains('/'))
            continue;
        const QString base = rel.left(slash);
        bool numeric = false;
        base.toInt(&numeric);
        if (!numeric)
            continue;
        const QString oid = meta.at(2);
        if (fname == QLatin1String("discussion.md"))
            filesByDir[base].discussionOid = oid;
        else if (fname == QLatin1String("answer.md"))
            filesByDir[base].answerOid = oid;
        else if (commentFileRe().match(fname).hasMatch())
            filesByDir[base].commentOids.insert(fname, oid);
        else
            continue;
        batchInput += oid.toUtf8() + '\n';
    }
    if (batchInput.isEmpty())
        return discussions;

    QByteArray batch;
    if (!runGit(m_mirror, {"cat-file", "--batch"}, &batch, nullptr, &batchInput))
        return discussions;
    QHash<QString, QByteArray> contentByOid;
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

    for (auto it = filesByDir.constBegin(); it != filesByDir.constEnd(); ++it) {
        const MirrorFiles &files = it.value();
        if (files.discussionOid.isEmpty() || !contentByOid.contains(files.discussionOid))
            continue;
        const FrontMatter fm = parseFrontMatter(contentByOid.value(files.discussionOid));
        Discussion discussion;
        discussion.number = it.key().toInt();
        discussion.title = fm.get("title");
        discussion.category = fm.get("category");
        discussion.status = fm.get("status");
        if (discussion.status != QLatin1String("closed") &&
            discussion.status != QLatin1String("archived"))
            discussion.status = QStringLiteral("open");
        discussion.createdAt = fm.num("createdAt");
        discussion.author = fm.get("author");
        discussion.authorName = fm.get("authorName");
        DiscussionEvent open = eventFromFrontMatter(fm);
        open.type = "open";
        open.title = discussion.title;
        open.category = discussion.category;
        discussion.events.append(open);

        for (const QString &oid : files.commentOids) {
            if (contentByOid.contains(oid))
                discussion.events.append(
                    eventFromFrontMatter(parseFrontMatter(contentByOid.value(oid))));
        }
        if (!files.answerOid.isEmpty() && contentByOid.contains(files.answerOid))
            discussion.answer = eventFromFrontMatter(
                parseFrontMatter(contentByOid.value(files.answerOid)));
        discussions.append(discussion);
    }
    std::sort(discussions.begin(), discussions.end(),
              [](const Discussion &a, const Discussion &b) {
                  return a.number < b.number;
              });
    return discussions;
}
