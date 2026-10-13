#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "FlowLayout.h"
#include "NetworkReplyError.h"
#include "SettingsWrite.h"
#include "TaskMarkdown.h"

#include <QActionGroup>
#include <QBuffer>
#include <QButtonGroup>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QFormLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QInputDialog>
#include <QJsonDocument>
#include <QMessageBox>
#include <QMenu>
#include <QNetworkReply>
#include <QPainter>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>

using namespace forkmesh::ui;

namespace {

constexpr int kOrganizationTaskPageSize = 100;
const QString kOrganizationTaskBackoffKey = QStringLiteral("_backoffMs");
constexpr int kOrganizationTaskDeleteMaxRetries = 5;
QHash<QString, int> g_organizationTaskDeleteRetries;
constexpr int kOrganizationTaskLoadAheadRows = 10;
constexpr qint64 kOrganizationTaskHistoryStaleMs = 30 * 1000;

QString taskText(const QJsonObject &task, const QString &field)
{
    return task.value(field).toString().trimmed();
}

QString taskBlockedText(const QJsonObject &payload)
{
    QStringList reasons;
    for (const QJsonValue &value :
         payload.value(QStringLiteral("blockedBy")).toArray()) {
        const QString code = value.toString();
        if (code == QLatin1String("agent_running"))
            reasons.append(QStringLiteral("an agent job is running on it"));
        else if (code == QLatin1String("qa_in_progress"))
            reasons.append(QStringLiteral("it is waiting for QA review"));
        else if (!code.isEmpty())
            reasons.append(code);
    }
    return reasons.join(QStringLiteral(" and "));
}

QString taskErrorText(const QJsonObject &payload, const QString &fallback)
{
    QString error = payload.value(QStringLiteral("error")).toString().trimmed();
    if (error.isEmpty())
        return fallback;
    error.replace(QLatin1Char('_'), QLatin1Char(' '));
    const QString blocked = taskBlockedText(payload);
    if (!blocked.isEmpty())
        error += QStringLiteral(" (%1)").arg(blocked);
    return error;
}

QString taskRequestContextText(const QJsonObject &payload)
{
    const QJsonObject context =
        payload.value(QStringLiteral("request")).toObject();
    static const QList<QPair<QString, QString>> fields = {
        {QStringLiteral("user"), QStringLiteral("user")},
        {QStringLiteral("via"), QStringLiteral("via")},
        {QStringLiteral("node"), QStringLiteral("node")},
        {QStringLiteral("bot"), QStringLiteral("bot")},
        {QStringLiteral("agent"), QStringLiteral("agent")},
        {QStringLiteral("ip"), QStringLiteral("ip")},
        {QStringLiteral("userAgent"), QStringLiteral("user agent")},
    };
    QStringList shown;
    for (const auto &[key, label] : fields) {
        const QString value = context.value(key).toString().trimmed();
        if (!value.isEmpty())
            shown.append(label + QLatin1Char(' ') + value);
    }
    return shown.join(QStringLiteral(", "));
}

// The canonical prefix this desktop signs with its account key for one task
// request when it holds no account session token. Empty when the relay accepts
// no key-signed form of the request: timers and QA verdicts deliberately
// still require a real session. `resource` receives the task id
// for a proof that names one. Must stay in lockstep with
// _org_task_signed_session in the worker's entry.py.
QString organizationTaskProof(const QByteArray &method, const QString &path,
                              const QJsonObject &body, QString *resource)
{
    static const QRegularExpression completeRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/complete/?$"));
    static const QRegularExpression undoRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/undo/?$"));
    static const QRegularExpression attachmentRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/attachments/"
                       "([a-f0-9]{32})/?$"));
    static const QRegularExpression itemRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/?$"));
    static const QRegularExpression agentStatusRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/agent-status/?$"));
    static const QRegularExpression pingRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/(remind|checkin)/?$"));
    static const QRegularExpression replyRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/replies/?$"));
    static const QRegularExpression historyRe(
        QStringLiteral("^/api/tasks/([a-f0-9]{32})/history/?$"));
    const bool collection = path == QLatin1String("/api/tasks") ||
                            path == QLatin1String("/api/tasks/");
    if (method == QByteArrayLiteral("GET"))
    {
        if (collection)
            return kOrgTaskListProof;
        const QRegularExpressionMatch attachment = attachmentRe.match(path);
        if (attachment.hasMatch()) {
            *resource = attachment.captured(1) + QLatin1Char('\n') +
                        attachment.captured(2);
            return kOrgTaskAttachmentProof;
        }
        const QRegularExpressionMatch history = historyRe.match(path);
        if (history.hasMatch()) {
            *resource = history.captured(1);
            return kOrgTaskHistoryProof;
        }
        return QString();
    }
    if (method == QByteArrayLiteral("DELETE")) {
        const QRegularExpressionMatch item = itemRe.match(path);
        if (!item.hasMatch())
            return QString();
        const QString digest = body.isEmpty()
                                   ? QString()
                                   : forkmesh::taskwork::updateDigest(body);
        if (!body.isEmpty() && digest.isEmpty())
            return QString();
        *resource = digest.isEmpty()
                        ? item.captured(1)
                        : item.captured(1) + QLatin1Char('\n') + digest;
        return kOrgTaskDeleteProof;
    }
    if (method == QByteArrayLiteral("PATCH")) {
        const QRegularExpressionMatch item = itemRe.match(path);
        QStringList fields = body.keys();
        fields.removeAll(QStringLiteral("sessionToken"));
        const QString digest = forkmesh::taskwork::updateDigest(body);
        if (!item.hasMatch() || digest.isEmpty() ||
            fields == QStringList{QStringLiteral("priority")} ||
            fields == QStringList{QStringLiteral("progress")})
            return QString();
        *resource = item.captured(1) + QLatin1Char('\n') + digest;
        return kOrgTaskUpdateProof;
    }
    if (method != QByteArrayLiteral("POST"))
        return QString();
    if (collection)
        return kOrgTaskOpenProof;
    const QRegularExpressionMatch complete = completeRe.match(path);
    if (complete.hasMatch()) {
        *resource = complete.captured(1);
        return kOrgTaskCompleteProof;
    }
    const QRegularExpressionMatch agentStatus = agentStatusRe.match(path);
    if (agentStatus.hasMatch()) {
        *resource = agentStatus.captured(1);
        return kOrgTaskAgentStatusProof;
    }
    const QRegularExpressionMatch reply = replyRe.match(path);
    if (reply.hasMatch()) {
        const QString digest = forkmesh::taskwork::updateDigest(body);
        if (digest.isEmpty())
            return QString();
        *resource = reply.captured(1) + QLatin1Char('\n') + digest;
        return kOrgTaskReplyProof;
    }
    const QRegularExpressionMatch ping = pingRe.match(path);
    if (ping.hasMatch()) {
        const QString digest = body.isEmpty()
                                   ? QString()
                                   : forkmesh::taskwork::updateDigest(body);
        if (!body.isEmpty() && digest.isEmpty())
            return QString();
        *resource = digest.isEmpty()
                        ? ping.captured(1)
                        : ping.captured(1) + QLatin1Char('\n') + digest;
        return ping.captured(2) == QLatin1String("remind")
                   ? kOrgTaskRemindProof
                   : kOrgTaskCheckinProof;
    }
    const QRegularExpressionMatch undo = undoRe.match(path);
    if (!undo.hasMatch())
        return QString();
    *resource = undo.captured(1);
    return kOrgTaskUndoProof;
}

bool organizationTaskIsClosed(const QJsonObject &task)
{
    return task.value(QStringLiteral("completedAt")).toDouble() > 0 ||
           taskText(task, QStringLiteral("status")) == QLatin1String("done");
}

bool organizationTaskAwaitsQa(const QJsonObject &task)
{
    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
    return qa.value(QStringLiteral("requestedAt")).toDouble() > 0 &&
           qa.value(QStringLiteral("reviewedAt")).toDouble() <= 0;
}

int openOrganizationTaskCount(const QJsonArray &tasks)
{
    int open = 0;
    for (const QJsonValue &value : tasks) {
        if (!organizationTaskIsClosed(value.toObject()))
            ++open;
    }
    return open;
}

QString taskTimestamp(qint64 milliseconds)
{
    if (milliseconds <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    return QDateTime::fromMSecsSinceEpoch(milliseconds)
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString taskCreatedAgo(qint64 milliseconds)
{
    const QString age = formatShortRelativeTime(milliseconds / 1000);
    if (age.isEmpty())
        return QString::fromUtf8("\xE2\x80\x94");
    return age == QLatin1String("now") ? age
                                        : age + QStringLiteral(" ago");
}

QString taskAssigneeLabel(const QJsonObject &task)
{
    const QString kind = taskText(task, QStringLiteral("assigneeKind"));
    if (kind == QLatin1String("agent"))
        return QStringLiteral("Bot");
    if (kind == QLatin1String("unassigned"))
        return QStringLiteral("Unassigned");
    const QString assignee = taskText(task, QStringLiteral("assignee"));
    return assignee.isEmpty() ? QStringLiteral("Unassigned") : assignee;
}

bool organizationTaskAssignedTo(const QJsonObject &task,
                                const QString &account)
{
    const QString actor = account.trimmed();
    return !actor.isEmpty() &&
           taskText(task, QStringLiteral("assigneeKind")) ==
               QLatin1String("user") &&
           taskText(task, QStringLiteral("assignee"))
                   .compare(actor, Qt::CaseInsensitive) == 0;
}

constexpr int kTaskStatusRole = Qt::UserRole + 1;
constexpr int kTaskDepartmentRole = Qt::UserRole + 2;
constexpr int kTaskRepositoryRole = Qt::UserRole + 3;
constexpr int kTaskAssigneeKindRole = Qt::UserRole + 4;
constexpr int kTaskAssigneeRole = Qt::UserRole + 5;
constexpr int kTaskQaRole = Qt::UserRole + 6;
constexpr int kTaskQaRequestedRole = Qt::UserRole + 7;
constexpr int kTaskPriorityRole = Qt::UserRole + 8;
constexpr int kTaskCreatedAtRole = Qt::UserRole + 9;
constexpr int kTaskProgressRole = Qt::UserRole + 10;
constexpr int kTaskCreatorRole = Qt::UserRole + 11;
constexpr int kTaskBranchRole = Qt::UserRole + 12;
constexpr int kTaskAgentFaceRole = Qt::UserRole + 13;
constexpr int kTaskAgentWorkingRole = Qt::UserRole + 14;
constexpr int kTaskAgentAngleRole = Qt::UserRole + 15;
constexpr int kTaskThumbnailsRole = Qt::UserRole + 16;
constexpr int kTaskAttachmentCountRole = Qt::UserRole + 17;
constexpr int kTaskCreatorPictureRole = Qt::UserRole + 18;
constexpr int kTaskAssigneePictureRole = Qt::UserRole + 19;

constexpr int kTaskRowHeight = 52;
constexpr int kTaskCompactRowHeight = 32;
constexpr int kTaskMetaGlyphSize = 14;
constexpr int kTaskPriorityWidth = 26;
constexpr int kTaskMetaGap = 14;
constexpr int kTaskAssigneeBadge = 13;
constexpr int kTaskAssigneeOverhang = 2;
constexpr int kTaskAgentBadge = 16;
constexpr int kTaskRowThumbSide = 34;
constexpr int kTaskRowThumbGap = 4;
constexpr int kTaskRowThumbMax = 3;
constexpr int kTaskAgentOrbit = 30;
constexpr int kTaskAgentSpinTickMs = 110;
constexpr qreal kTaskAgentSpinStepDegrees = 20.0;

QString taskStatusKey(const QJsonObject &task)
{
    if (task.value(QStringLiteral("completedAt")).toDouble() > 0 ||
        taskText(task, QStringLiteral("status")) == QLatin1String("done"))
        return QStringLiteral("done");
    if (taskText(task, QStringLiteral("status")) == QLatin1String("active"))
        return QStringLiteral("active");
    if (taskText(task, QStringLiteral("assigneeKind")) ==
            QLatin1String("agent") &&
        !taskText(task, QStringLiteral("agentSessionId")).isEmpty())
        return QStringLiteral("queued");
    return QStringLiteral("ready");
}

QString taskStatusLabel(const QString &key)
{
    if (key == QLatin1String("done"))
        return QStringLiteral("Done");
    if (key == QLatin1String("active"))
        return QStringLiteral("In progress");
    if (key == QLatin1String("queued"))
        return QStringLiteral("Queued");
    return QStringLiteral("Ready");
}

QString taskStatusIcon(const QString &key)
{
    if (key == QLatin1String("done"))
        return QStringLiteral("check-circle");
    if (key == QLatin1String("active"))
        return QStringLiteral("sync");
    if (key == QLatin1String("queued"))
        return QStringLiteral("list-unordered");
    return QStringLiteral("issue-opened");
}

// The catalog endpoint does not promise an order. Keep all ordering here so
// paging always happens after the user's chosen sort and the first page is
// predictably the newest work rather than whichever slice the relay returned.
qint64 taskSortTimestamp(const QJsonObject &task, const QString &field)
{
    const qint64 timestamp = qint64(task.value(field).toDouble());
    if (timestamp > 0 || field == QLatin1String("createdAt"))
        return timestamp;
    return qint64(task.value(QStringLiteral("createdAt")).toDouble());
}

int compareTaskText(const QString &left, const QString &right,
                    bool descending = false)
{
    if (left.isEmpty() != right.isEmpty())
        return left.isEmpty() ? 1 : -1; // missing values always belong last
    const int compared = left.compare(right, Qt::CaseInsensitive);
    return descending ? -compared : compared;
}

int compareTaskNumber(qint64 left, qint64 right, bool descending,
                      bool missingIsZero = true)
{
    if (missingIsZero && (left <= 0) != (right <= 0))
        return left <= 0 ? 1 : -1; // undated/unprioritized tasks stay last
    if (left == right)
        return 0;
    const int compared = left < right ? -1 : 1;
    return descending ? -compared : compared;
}

int taskStatusSortRank(const QJsonObject &task)
{
    const QString status = taskStatusKey(task);
    if (status == QLatin1String("active"))
        return 0;
    if (status == QLatin1String("queued"))
        return 1;
    if (status == QLatin1String("ready"))
        return 2;
    return 3; // done
}

bool organizationTaskComesBefore(const QJsonObject &left,
                                 const QJsonObject &right,
                                 const QString &sortKey)
{
    int compared = 0;
    if (sortKey == QLatin1String("created-asc")) {
        compared = compareTaskNumber(
            taskSortTimestamp(left, QStringLiteral("createdAt")),
            taskSortTimestamp(right, QStringLiteral("createdAt")), false);
    } else if (sortKey == QLatin1String("updated-desc") ||
               sortKey == QLatin1String("updated-asc")) {
        compared = compareTaskNumber(
            taskSortTimestamp(left, QStringLiteral("updatedAt")),
            taskSortTimestamp(right, QStringLiteral("updatedAt")),
            sortKey.endsWith(QLatin1String("desc")));
    } else if (sortKey == QLatin1String("priority-asc") ||
               sortKey == QLatin1String("priority-desc")) {
        compared = compareTaskNumber(
            left.value(QStringLiteral("priority")).toInt(),
            right.value(QStringLiteral("priority")).toInt(),
            sortKey.endsWith(QLatin1String("desc")));
    } else if (sortKey == QLatin1String("title-asc") ||
               sortKey == QLatin1String("title-desc")) {
        compared = compareTaskText(
            taskText(left, QStringLiteral("title")),
            taskText(right, QStringLiteral("title")),
            sortKey.endsWith(QLatin1String("desc")));
    } else if (sortKey == QLatin1String("status")) {
        compared = taskStatusSortRank(left) - taskStatusSortRank(right);
    } else if (sortKey == QLatin1String("assignee")) {
        compared = compareTaskText(taskAssigneeLabel(left),
                                   taskAssigneeLabel(right));
    } else if (sortKey == QLatin1String("department")) {
        compared = compareTaskText(
            taskText(left, QStringLiteral("department")),
            taskText(right, QStringLiteral("department")));
    } else if (sortKey == QLatin1String("repository")) {
        compared = compareTaskText(
            taskText(left, QStringLiteral("repository")),
            taskText(right, QStringLiteral("repository")));
    } else {
        // Newest-created is deliberately the default, including for an unknown
        // key left behind by a future/older build.
        compared = compareTaskNumber(
            taskSortTimestamp(left, QStringLiteral("createdAt")),
            taskSortTimestamp(right, QStringLiteral("createdAt")), true);
    }
    if (compared != 0)
        return compared < 0;

    // Stable, useful tie-breaks: recently created work first, then title and
    // id. This keeps every sort deterministic without depending on relay order.
    compared = compareTaskNumber(
        taskSortTimestamp(left, QStringLiteral("createdAt")),
        taskSortTimestamp(right, QStringLiteral("createdAt")), true);
    if (compared != 0)
        return compared < 0;
    compared = compareTaskText(taskText(left, QStringLiteral("title")),
                               taskText(right, QStringLiteral("title")));
    if (compared != 0)
        return compared < 0;
    return taskText(left, QStringLiteral("id")) <
           taskText(right, QStringLiteral("id"));
}

QColor taskStatusColor(const QString &key, bool dark)
{
    if (key == QLatin1String("done"))
        return QColor(dark ? "#3fb950" : "#1a7f37");
    if (key == QLatin1String("active"))
        return Theme::nextOr(Theme::Role::AccentFg, QColor(Theme::kRunning));
    if (key == QLatin1String("queued"))
        return QColor(dark ? "#e3b341" : "#9a6700");
    return Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
}

QColor taskAccentColor(const QString &seed)
{
    if (seed.isEmpty())
        return QColor(Theme::kTextTertiary);
    uint hash = 2166136261u;
    for (const QChar &ch : seed)
        hash = (hash ^ uint(ch.unicode())) * 16777619u;
    return QColor(
        Theme::kSenderPalette[hash % uint(Theme::kSenderPaletteSize)]);
}

QString taskDepartmentIcon(const QString &department)
{
    static const QHash<QString, QString> known{
        {QStringLiteral("engineering"), QStringLiteral("code")},
        {QStringLiteral("product-design"), QStringLiteral("pencil")},
        {QStringLiteral("design"), QStringLiteral("pencil")},
        {QStringLiteral("product"), QStringLiteral("package")},
        {QStringLiteral("marketing"), QStringLiteral("broadcast")},
        {QStringLiteral("community"), QStringLiteral("people")},
        {QStringLiteral("support"), QStringLiteral("comment")},
        {QStringLiteral("security"), QStringLiteral("lock")},
        {QStringLiteral("infrastructure"), QStringLiteral("server")},
        {QStringLiteral("operations"), QStringLiteral("workflow")},
        {QStringLiteral("quality-assurance"), QStringLiteral("shield-check")},
        {QStringLiteral("research"), QStringLiteral("graph")},
        {QStringLiteral("finance"), QStringLiteral("credit-card")},
        {QStringLiteral("general"), QStringLiteral("tag")},
    };
    return known.value(department.toLower(), QStringLiteral("tag"));
}

QPixmap taskIdenticon(const QString &seed, int side, qreal radiusRatio)
{
    static QHash<QString, QPixmap> cache;
    const QString key = seed + QLatin1Char('|') + QString::number(side) +
                        QLatin1Char('|') + QString::number(radiusRatio) +
                        QLatin1Char('|') +
                        QString::number(iconDevicePixelRatio());
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();
    const QPixmap pixmap =
        roundedAvatar(forkMeshAvatarPng(seed.toLower()), side, radiusRatio);
    cache.insert(key, pixmap);
    return pixmap;
}

QPixmap taskPersonFace(const QVariant &picture, const QString &seed, int side,
                       qreal radiusRatio)
{
    const QPixmap source = picture.value<QPixmap>();
    if (source.isNull())
        return seed.isEmpty() ? QPixmap()
                              : taskIdenticon(seed, side, radiusRatio);
    static QHash<QString, QPixmap> cache;
    const QString key = QString::number(source.cacheKey()) +
                        QLatin1Char('|') + QString::number(side) +
                        QLatin1Char('|') + QString::number(radiusRatio) +
                        QLatin1Char('|') +
                        QString::number(iconDevicePixelRatio());
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();
    if (cache.size() > 512)
        cache.clear();
    const QPixmap pixmap = roundedAvatar(source, side, radiusRatio);
    cache.insert(key, pixmap);
    return pixmap;
}

// A repository that keeps a conventional logo at its root is recognisable at a
// glance, so the task surfaces show that file instead of the generated
// identicon. The names, and their order, are the ones the website reads for a
// native repository logo (_committed_repository_logo_url in the worker).
struct TaskRepositoryLogo {
    QString path;
    qint64 modifiedMs = 0;
};

QString repositoryRootLogoPath(const QString &workTree)
{
    if (workTree.isEmpty())
        return QString();
    static const QString names[] = {
        QStringLiteral("logo.png"), QStringLiteral("logo.webp"),
        QStringLiteral("logo.jpg"), QStringLiteral("logo.jpeg")};
    const QDir dir(workTree);
    for (const QString &name : names) {
        const QString path = dir.filePath(name);
        if (QFileInfo(path).isFile())
            return path;
    }
    return QString();
}

// Resolved once per render by publishTaskRepositoryLogos(), never from the
// paint path: a delegate must not touch the filesystem per row.
QHash<QString, TaskRepositoryLogo> &taskRepositoryLogos()
{
    static QHash<QString, TaskRepositoryLogo> logos;
    return logos;
}

TaskRepositoryLogo taskRepositoryLogo(const QString &repository)
{
    return taskRepositoryLogos().value(repository.trimmed().toLower());
}

QPixmap taskRepositoryLogoPixmap(const TaskRepositoryLogo &logo, int side,
                                 qreal radiusRatio)
{
    if (logo.path.isEmpty())
        return QPixmap();
    static QHash<QString, QPixmap> cache;
    const qreal dpr = iconDevicePixelRatio();
    // The stamp is part of the key, so a replaced logo redraws instead of
    // serving the old mark for the rest of the session.
    const QString key = logo.path + QLatin1Char('|') +
                        QString::number(logo.modifiedMs) + QLatin1Char('|') +
                        QString::number(side) + QLatin1Char('|') +
                        QString::number(radiusRatio) + QLatin1Char('|') +
                        QString::number(dpr);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();
    const int px = qMax(1, qRound(side * dpr));
    QImageReader reader(logo.path);
    reader.setAutoTransform(true);
    const QSize native = reader.size();
    if (native.isValid() && !native.isEmpty() &&
        (native.width() > px || native.height() > px)) {
        // Decode straight to the size we draw: project logos are routinely
        // more than a thousand pixels square.
        reader.setScaledSize(native.scaled(px, px, Qt::KeepAspectRatio));
    }
    const QImage image = reader.read();
    // Stays null when the file is missing or unreadable, and the identicon
    // takes the slot back.
    QPixmap mark;
    if (!image.isNull()) {
        QPixmap scaled = QPixmap::fromImage(image);
        if (scaled.width() > px || scaled.height() > px)
            scaled = scaled.scaled(px, px, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
        // Contained, not cropped: a wordmark keeps its shape where the
        // expanding fit an identicon uses would cut its ends off.
        mark = crispIconPixmap(side, dpr);
        QPainter painter(&mark);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        const qreal radius = side * radiusRatio;
        clip.addRoundedRect(0, 0, side, side, radius, radius);
        painter.setClipPath(clip);
        painter.drawPixmap(QPointF((side - scaled.width() / dpr) / 2.0,
                                   (side - scaled.height() / dpr) / 2.0),
                           scaled);
    }
    cache.insert(key, mark);
    return mark;
}

// Republished whenever the task list renders, so a repository cloned - or a
// logo committed - during the session needs no restart.
void publishTaskRepositoryLogos(const QList<RepositoryRecord> &repositories)
{
    QHash<QString, TaskRepositoryLogo> logos;
    QSet<QString> ambiguous;
    for (const RepositoryRecord &repo : repositories) {
        const QString path = repositoryRootLogoPath(repo.localPath);
        if (path.isEmpty())
            continue;
        TaskRepositoryLogo logo;
        logo.path = path;
        logo.modifiedMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        logos.insert((repo.owner + QLatin1Char('/') + repo.name).toLower(),
                     logo);
        // Tasks that name a repository without its owner still resolve, as
        // long as exactly one repository with a logo answers to that name.
        const QString bare = repo.name.toLower();
        if (bare.isEmpty() || ambiguous.contains(bare))
            continue;
        if (logos.contains(bare) && logos.value(bare).path != path) {
            ambiguous.insert(bare);
            logos.remove(bare);
            continue;
        }
        logos.insert(bare, logo);
    }
    taskRepositoryLogos() = logos;
}

// One description of the mark that stands for a task field. The list rows, the
// hover card and the detail pane all read their icons from here, so the three
// surfaces can never drift into three different vocabularies.
struct TaskGlyph {
    QString icon;        // octicon name, drawn when no identicon renders
    QColor color;        // tint for the octicon
    QString avatarSeed;  // identicon seed; preferred over the octicon
    qreal avatarRadius = 0.5;
    TaskRepositoryLogo logo;  // the project's own logo; preferred over both
};

QColor taskMutedColor(bool dark)
{
    return Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
}

TaskGlyph taskStatusGlyph(const QString &statusKey, bool dark)
{
    return {taskStatusIcon(statusKey), taskStatusColor(statusKey, dark), {}, 0.5};
}

TaskGlyph taskPriorityGlyph(bool dark)
{
    return {QStringLiteral("alert"),
            Theme::nextOr(Theme::Role::FgMuted,
                          QColor(dark ? "#e3b341" : "#9a6700")),
            {}, 0.5};
}

TaskGlyph taskDepartmentGlyph(const QString &department)
{
    return {taskDepartmentIcon(department), taskAccentColor(department), {}, 0.5};
}

TaskGlyph taskRepositoryGlyph(const QString &repository)
{
    return {QStringLiteral("repo"), taskAccentColor(repository), repository,
            0.28, taskRepositoryLogo(repository)};
}

// People are drawn as their identicon wherever one renders, with the plain
// person octicon as the fallback — the same rule the list rows follow.
TaskGlyph taskPersonGlyph(const QString &person, bool dark)
{
    const QString seed = person.trimmed();
    if (seed.isEmpty())
        return {QStringLiteral("person"), taskMutedColor(dark), {}, 0.5};
    return {QStringLiteral("person"), taskAccentColor(seed), seed, 0.5};
}

TaskGlyph taskAssigneeGlyph(const QString &assigneeKind, const QString &assignee,
                            bool dark)
{
    if (assigneeKind == QLatin1String("agent"))
        return {QStringLiteral("sparkle"),
                Theme::nextOr(Theme::Role::FgMuted, QColor(Theme::kGenie)), {},
                0.5};
    return taskPersonGlyph(assignee, dark);
}

TaskGlyph taskDestinationGlyph(const QString &destination, bool dark)
{
    if (destination == QLatin1String("agent"))
        return {QStringLiteral("sparkle"),
                Theme::nextOr(Theme::Role::FgMuted, QColor(Theme::kGenie)), {},
                0.5};
    if (destination == QLatin1String("qa"))
        return {QStringLiteral("shield-check"), taskMutedColor(dark), {}, 0.5};
    return {QStringLiteral("paper-airplane"), taskMutedColor(dark), {}, 0.5};
}

// `decided` reports whether QA has actually said anything yet: the list rows
// leave their slot empty until it has, while the text surfaces always print a
// glyph so their label columns stay aligned.
TaskGlyph taskQaGlyph(const QString &status, bool requested, bool dark,
                      bool *decided)
{
    if (decided)
        *decided = true;
    if (status == QLatin1String("passed"))
        return {QStringLiteral("shield-check"),
                QColor(dark ? "#3fb950" : "#1a7f37"), {}, 0.5};
    if (status == QLatin1String("failed"))
        return {QStringLiteral("circle-slash"),
                QColor(dark ? "#f85149" : "#cf222e"), {}, 0.5};
    if (requested)
        return {QStringLiteral("eye"), QColor(dark ? "#e3b341" : "#9a6700"), {},
                0.5};
    if (decided)
        *decided = false;
    return {QStringLiteral("shield-check"), taskMutedColor(dark), {}, 0.5};
}

TaskGlyph taskTimeGlyph(bool dark)
{
    return {QStringLiteral("history"), taskMutedColor(dark), {}, 0.5};
}

QString taskPixmapMarkup(const QPixmap &pixmap, int size)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG");
    return QStringLiteral(
               "<img src='data:image/png;base64,%1' width='%2' height='%2'>")
        .arg(QString::fromLatin1(png.toBase64()))
        .arg(size);
}

// Rich text can only carry a glyph as an inline image, so every octicon and
// identicon the two text surfaces show is encoded once and cached: a full page
// of rows asks for the same handful of marks over and over.
QString taskGlyphMarkup(const TaskGlyph &glyph, int size)
{
    static QHash<QString, QString> cache;
    const QString key = glyph.icon + QLatin1Char('|') +
                        QString::number(glyph.color.rgba(), 16) +
                        QLatin1Char('|') + glyph.avatarSeed + QLatin1Char('|') +
                        QString::number(glyph.avatarRadius) + QLatin1Char('|') +
                        glyph.logo.path + QLatin1Char('|') +
                        QString::number(glyph.logo.modifiedMs) +
                        QLatin1Char('|') + QString::number(size) +
                        QLatin1Char('|') +
                        QString::number(iconDevicePixelRatio());
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();
    QString markup;
    const QPixmap logo =
        taskRepositoryLogoPixmap(glyph.logo, size, glyph.avatarRadius);
    if (!logo.isNull())
        markup = taskPixmapMarkup(logo, size);
    if (markup.isEmpty() && !glyph.avatarSeed.isEmpty()) {
        const QPixmap avatar =
            taskIdenticon(glyph.avatarSeed, size, glyph.avatarRadius);
        if (!avatar.isNull())
            markup = taskPixmapMarkup(avatar, size);
    }
    if (markup.isEmpty())
        markup = octiconMarkup(glyph.icon, size, glyph.color);
    cache.insert(key, markup);
    return markup;
}

QString taskTooltipRow(const TaskGlyph &glyph, const QString &label,
                       const QString &value)
{
    const QString shown = value.trimmed().isEmpty()
                              ? QString::fromUtf8("\xE2\x80\x94")
                              : value.trimmed();
    return QStringLiteral(
               "<tr>"
               "<td style='padding:2px 7px 2px 0'>%1</td>"
               "<td style='padding:2px 10px 2px 0;color:#57606a'>%2</td>"
               "<td style='padding:2px 0;color:#1f2328'>%3</td>"
               "</tr>")
        .arg(taskGlyphMarkup(glyph, 14), label.toHtmlEscaped(),
             shown.toHtmlEscaped());
}

QString taskRowTooltipHtml(const QJsonObject &task, const QString &statusKey,
                           const QString &department,
                           const QString &repository,
                           const QString &qaStatus, bool qaRequested)
{
    // The card paints its own light background, so its glyphs are always
    // tinted for a light surface whatever the app theme is.
    constexpr bool dark = false;
    const QString title = taskText(task, QStringLiteral("title"));
    const qint64 createdAt =
        qint64(task.value(QStringLiteral("createdAt")).toDouble());
    const QString created = createdAt > 0
                                ? QString::fromUtf8("%1 \xC2\xB7 %2")
                                      .arg(taskCreatedAgo(createdAt),
                                           taskTimestamp(createdAt))
                                : QString::fromUtf8("\xE2\x80\x94");
    QString html = QStringLiteral(
        "<table cellspacing='0' cellpadding='0' bgcolor='#f6f8fa' "
        "style='background-color:#f6f8fa;color:#1f2328;"
        "border:1px solid #d0d7de;padding:8px'>"
        "<tr><td colspan='3' style='padding:0 0 6px 0;color:#1f2328;"
        "font-weight:700'>%1</td></tr>")
                       .arg(title.toHtmlEscaped());
    html += taskTooltipRow(taskStatusGlyph(statusKey, dark),
                           QStringLiteral("Status"),
                           taskStatusLabel(statusKey));
    html += taskTooltipRow(taskPriorityGlyph(dark), QStringLiteral("Priority"),
                           QString::number(
                               task.value(QStringLiteral("priority")).toInt()));
    html += taskTooltipRow(taskDepartmentGlyph(department),
                           QStringLiteral("Department"), department);
    html += taskTooltipRow(taskRepositoryGlyph(repository),
                           QStringLiteral("Repository"), repository);
    html += taskTooltipRow(
        taskAssigneeGlyph(taskText(task, QStringLiteral("assigneeKind")),
                          taskText(task, QStringLiteral("assignee")), dark),
        QStringLiteral("Assignee"), taskAssigneeLabel(task));
    html += taskTooltipRow(taskQaGlyph(qaStatus, qaRequested, dark, nullptr),
                           QStringLiteral("QA"),
                           qaStatus.isEmpty() ? QStringLiteral("unknown")
                                              : qaStatus);
    html += taskTooltipRow(taskTimeGlyph(dark), QStringLiteral("Created"),
                           created);
    return html + QStringLiteral("</table>");
}

int taskRowHeight(forkmesh::ui::ListViewMode mode)
{
    return mode == forkmesh::ui::ListViewMode::Full ? kTaskRowHeight
                                                     : kTaskCompactRowHeight;
}

class OrganizationTaskRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void setMode(forkmesh::ui::ListViewMode mode) { m_mode = mode; }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), taskRowHeight(m_mode)));
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const bool dark = currentThemeIsDark();
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(
                selected ? Theme::nextOr(Theme::Role::SurfaceActive,
                                         QColor(dark ? "#1f6feb" : "#0969da"))
                         : Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa")));
            painter->drawRoundedRect(option.rect.adjusted(2, 2, -2, -2), 6, 6);
        }

        const QString statusKey = index.data(kTaskStatusRole).toString();
        const QString department = index.data(kTaskDepartmentRole).toString();
        const QString repository = index.data(kTaskRepositoryRole).toString();
        const QString assigneeKind =
            index.data(kTaskAssigneeKindRole).toString();
        const QString assignee = index.data(kTaskAssigneeRole).toString();
        const QString qa = index.data(kTaskQaRole).toString();
        const bool qaRequested = index.data(kTaskQaRequestedRole).toBool();
        const QString createdAgo =
            taskCreatedAgo(index.data(kTaskCreatedAtRole).toLongLong());
        const bool done = statusKey == QLatin1String("done");

        const QColor muted(selected ? Theme::nextOr(Theme::Role::FgSecondary,
                                                    QColor("#c8e1ff"))
                                    : Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76")));
        const QColor rowBg =
            selected ? Theme::nextOr(Theme::Role::SurfaceActive,
                                     QColor(dark ? "#1f6feb" : "#0969da"))
            : hovered ? Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa"))
                      : option.palette.base().color();
        const QColor titleColor(
            selected ? Theme::nextOr(Theme::Role::Fg, QColor("#ffffff"))
            : done   ? muted
                     : Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328")));
        if (m_mode == forkmesh::ui::ListViewMode::Zen) {
            paintZen(painter, option, index, done, titleColor);
            painter->restore();
            return;
        }
        if (m_mode == forkmesh::ui::ListViewMode::Compact) {
            paintCompact(painter, option, index, dark, selected, done, muted,
                         titleColor);
            painter->restore();
            return;
        }
        const QRect inner = option.rect.adjusted(0, 6, 0, -6);
        const int lineHeight = inner.height() / 2;
        const QRect titleLine(inner.left(), inner.top(), inner.width(),
                              lineHeight);
        const QRect metaLine(inner.left(), inner.top() + lineHeight,
                             inner.width(), inner.height() - lineHeight);
        int right = option.rect.right() - 8;

        const QVariantList thumbnails =
            index.data(kTaskThumbnailsRole).toList();
        if (!thumbnails.isEmpty()) {
            const int side =
                qMin(kTaskRowThumbSide, option.rect.height() - 12);
            const int top =
                option.rect.top() + (option.rect.height() - side) / 2;
            int tileX = right - thumbnails.size() * (side + kTaskRowThumbGap) +
                        kTaskRowThumbGap + 1;
            right = tileX - 10;
            const int hidden =
                index.data(kTaskAttachmentCountRole).toInt() -
                thumbnails.size();
            const QColor tileBorder(selected ? Theme::nextOr(Theme::Role::BorderStrong,
                                                             QColor("#c8e1ff"))
                                             : Theme::remapColor(QColor(dark ? "#30363d"
                                                           : "#d0d7de")));
            for (int i = 0; i < thumbnails.size(); ++i) {
                const QRect tile(tileX, top, side, side);
                const QPixmap pixmap = thumbnails.at(i).value<QPixmap>();
                QPainterPath clip;
                clip.addRoundedRect(QRectF(tile), 4, 4);
                painter->save();
                painter->setClipPath(clip);
                if (pixmap.isNull()) {
                    painter->fillRect(tile,
                                      Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa")));
                    const int glyph = qMin(16, side - 8);
                    painter->drawPixmap(
                        tile.left() + (side - glyph) / 2,
                        tile.top() + (side - glyph) / 2,
                        tintedOcticonPixmap(
                            QStringLiteral("paperclip"),
                            Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76")), glyph));
                } else {
                    painter->drawPixmap(tile, pixmap);
                }
                if (hidden > 0 && i == thumbnails.size() - 1) {
                    painter->fillRect(tile, QColor(0, 0, 0, 140));
                    QFont moreFont = option.font;
                    moreFont.setBold(true);
                    painter->setFont(moreFont);
                    painter->setPen(QColor("#ffffff"));
                    painter->drawText(tile, Qt::AlignCenter,
                                      QStringLiteral("+%1").arg(hidden));
                }
                painter->restore();
                painter->setPen(QPen(tileBorder, 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(
                    QRectF(tile).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
                tileX += side + kTaskRowThumbGap;
            }
        }

        const QRect rail(option.rect.left() + 4, option.rect.top() + 9, 3,
                         option.rect.height() - 18);
        const int progress =
            qBound(0, index.data(kTaskProgressRole).toInt(), 100);
        painter->setPen(QPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")), 3,
                             Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(rail.center().x(), rail.top(), rail.center().x(),
                          rail.bottom());
        if (progress > 0) {
            const int progressTop =
                rail.bottom() - qRound(rail.height() * progress / 100.0);
            painter->setPen(QPen(Theme::nextOr(Theme::Role::AccentFg,
                                               QColor(done ? "#8250df" : "#2f81f7")),
                                 3, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(rail.center().x(), progressTop,
                              rail.center().x(), rail.bottom());
        }

        const int avatarD = qMax(24, qMin(32, option.rect.height() - 18));
        const QRect avatar(rail.right() + 8,
                           option.rect.top() +
                               (option.rect.height() - avatarD) / 2,
                           avatarD, avatarD);
        const QString creator = index.data(kTaskCreatorRole).toString();
        const QPixmap creatorFace =
            creator.isEmpty() ? QPixmap()
                              : taskPersonFace(
                                    index.data(kTaskCreatorPictureRole),
                                    creator, avatarD, 0.5);
        if (!creatorFace.isNull())
            painter->drawPixmap(avatar, creatorFace);
        else
            painter->drawPixmap(
                avatar.topLeft() + QPoint((avatarD - 18) / 2, (avatarD - 18) / 2),
                tintedOcticonPixmap(QStringLiteral("person"), muted, 18));

        const QRect badge(
            avatar.right() - kTaskAssigneeBadge + kTaskAssigneeOverhang,
            avatar.bottom() - kTaskAssigneeBadge + kTaskAssigneeOverhang,
            kTaskAssigneeBadge, kTaskAssigneeBadge);
        const QPixmap agentFace =
            index.data(kTaskAgentFaceRole).value<QPixmap>();
        const QRect agentBadge(
            avatar.right() - kTaskAgentBadge + kTaskAssigneeOverhang,
            avatar.bottom() - kTaskAgentBadge + kTaskAssigneeOverhang,
            kTaskAgentBadge, kTaskAgentBadge);
        const bool agentAssigned = assigneeKind == QLatin1String("agent") ||
                                   !agentFace.isNull();
        const bool personAssigned =
            !agentAssigned && !assignee.isEmpty() &&
            assigneeKind != QLatin1String("unassigned") &&
            assignee.compare(creator, Qt::CaseInsensitive) != 0;
        if (!agentFace.isNull()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(rowBg);
            painter->drawEllipse(agentBadge.adjusted(-1, -1, 1, 1));
            painter->drawPixmap(agentBadge, agentFace);
            if (index.data(kTaskAgentWorkingRole).toBool())
                painter->drawPixmap(
                    QPoint(agentBadge.center().x() - kTaskAgentOrbit / 2,
                           agentBadge.center().y() - kTaskAgentOrbit / 2),
                    agentRunningFireballPixmap(
                        kTaskAgentOrbit,
                        index.data(kTaskAgentAngleRole).toReal()));
        } else if (agentAssigned || personAssigned) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(rowBg);
            painter->drawEllipse(badge.adjusted(-1, -1, 1, 1));
            if (agentAssigned) {
                painter->drawPixmap(
                    badge.topLeft() + QPoint(1, 1),
                    tintedOcticonPixmap(QStringLiteral("sparkle"),
                                        Theme::nextOr(Theme::Role::FgSecondary,
                                                      QColor(Theme::kGenie)),
                                        kTaskAssigneeBadge - 2));
            } else {
                const QPixmap face = taskPersonFace(
                    index.data(kTaskAssigneePictureRole), assignee,
                    kTaskAssigneeBadge, 0.5);
                if (!face.isNull())
                    painter->drawPixmap(badge, face);
            }
        }

        QFont smallFont = option.font;
        smallFont.setPointSizeF(qMax(7.0, option.font.pointSizeF() - 1.5));

        const int left = avatar.right() + 6;
        painter->setPen(muted);
        painter->setFont(smallFont);
        painter->drawText(QRect(left, titleLine.top(), kTaskPriorityWidth,
                                titleLine.height()),
                          Qt::AlignVCenter | Qt::AlignRight,
                          index.data(kTaskPriorityRole).toString());
        const int x0 = left + kTaskPriorityWidth + 10;

        const QRect titleRect(x0, titleLine.top(), qMax(0, right - x0),
                              titleLine.height());
        QFont titleFont = option.font;
        titleFont.setStrikeOut(done);
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        painter->drawText(
            titleRect, Qt::AlignVCenter | Qt::AlignLeft,
            painter->fontMetrics().elidedText(index.data(Qt::DisplayRole)
                                                  .toString(),
                                              Qt::ElideRight,
                                              qMax(0, titleRect.width())));

        painter->setFont(smallFont);
        const QFontMetrics metrics = painter->fontMetrics();
        const int glyphTop =
            metaLine.top() + (metaLine.height() - kTaskMetaGlyphSize) / 2;
        int x = x0;
        auto drawGlyph = [&](const TaskGlyph &glyph) {
            const QPixmap logo = taskRepositoryLogoPixmap(
                glyph.logo, kTaskMetaGlyphSize, glyph.avatarRadius);
            if (!logo.isNull()) {
                painter->drawPixmap(x, glyphTop, logo);
                return;
            }
            if (!glyph.avatarSeed.isEmpty()) {
                const QPixmap avatar = taskIdenticon(
                    glyph.avatarSeed, kTaskMetaGlyphSize, glyph.avatarRadius);
                if (!avatar.isNull()) {
                    painter->drawPixmap(x, glyphTop, avatar);
                    return;
                }
            }
            if (glyph.icon.isEmpty())
                return;
            painter->drawPixmap(x, glyphTop,
                                tintedOcticonPixmap(glyph.icon,
                                                    selected ? Theme::nextOr(Theme::Role::FgSecondary,
                                                                             QColor("#ffffff"))
                                                             : glyph.color,
                                                    kTaskMetaGlyphSize));
        };
        const int charW = qMax(1, metrics.averageCharWidth());
        auto drawColumn = [&](const TaskGlyph &glyph, const QString &text,
                              int chars) {
            const int columnRight =
                chars > 0 ? qMin(right, x + kTaskMetaGlyphSize + 4 + chars * charW)
                          : right;
            if (x < right && !text.isEmpty()) {
                drawGlyph(glyph);
                const int textX = x + kTaskMetaGlyphSize + 4;
                const int room = columnRight - textX;
                if (room > 0) {
                    painter->setPen(muted);
                    painter->drawText(
                        QRect(textX, metaLine.top(), room, metaLine.height()),
                        Qt::AlignVCenter | Qt::AlignLeft,
                        metrics.elidedText(text, Qt::ElideRight, room));
                }
            }
            x = columnRight + kTaskMetaGap;
        };

        drawColumn(taskStatusGlyph(statusKey, dark),
                   taskStatusLabel(statusKey), 11);
        drawColumn(taskDepartmentGlyph(department), department, 12);
        drawColumn(taskRepositoryGlyph(repository), repository, 18);
        drawColumn({QStringLiteral("git-branch"), muted, {}, 0.5},
                   index.data(kTaskBranchRole).toString(), 20);
        bool qaDecided = false;
        const TaskGlyph qaGlyph =
            taskQaGlyph(qa, qaRequested, dark, &qaDecided);
        drawColumn(qaGlyph,
                   !qaDecided ? QString()
                   : qa == QLatin1String("passed") ? QStringLiteral("QA passed")
                   : qa == QLatin1String("failed") ? QStringLiteral("QA failed")
                                                   : QStringLiteral("In QA"),
                   9);
        drawColumn(taskTimeGlyph(dark), createdAgo, 0);
        painter->restore();
    }

private:
    static void paintZen(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index, bool done,
                         const QColor &titleColor)
    {
        QFont titleFont = option.font;
        titleFont.setStrikeOut(done);
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        const QRect titleRect = option.rect.adjusted(14, 0, -14, 0);
        painter->drawText(
            titleRect, Qt::AlignVCenter | Qt::AlignLeft,
            painter->fontMetrics().elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                qMax(0, titleRect.width())));
    }

    static void paintCompact(QPainter *painter,
                             const QStyleOptionViewItem &option,
                             const QModelIndex &index, bool dark,
                             bool selected, bool done, const QColor &muted,
                             const QColor &titleColor)
    {
        const QRect rect = option.rect;
        const int railX = rect.left() + 5;
        const int railTop = rect.top() + 7;
        const int railBottom = rect.bottom() - 7;
        const int progress =
            qBound(0, index.data(kTaskProgressRole).toInt(), 100);
        painter->setPen(QPen(QColor(dark ? "#30363d" : "#d0d7de"), 3,
                             Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(railX, railTop, railX, railBottom);
        if (progress > 0) {
            const int progressTop =
                railBottom - qRound((railBottom - railTop) * progress / 100.0);
            painter->setPen(QPen(QColor(done ? "#8250df" : "#2f81f7"), 3,
                                 Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(railX, progressTop, railX, railBottom);
        }

        QFont smallFont = option.font;
        smallFont.setPointSizeF(qMax(7.0, option.font.pointSizeF() - 1.5));
        painter->setFont(smallFont);
        const QFontMetrics metrics = painter->fontMetrics();
        const int charW = qMax(1, metrics.averageCharWidth());
        painter->setPen(muted);
        const int priorityLeft = railX + 6;
        painter->drawText(QRect(priorityLeft, rect.top(), kTaskPriorityWidth,
                                rect.height()),
                          Qt::AlignVCenter | Qt::AlignRight,
                          index.data(kTaskPriorityRole).toString());
        const int x0 = priorityLeft + kTaskPriorityWidth + 10;
        const int glyphTop =
            rect.top() + (rect.height() - kTaskMetaGlyphSize) / 2;
        const QString statusKey = index.data(kTaskStatusRole).toString();
        const QString assigneeKind =
            index.data(kTaskAssigneeKindRole).toString();
        const QString assignee = index.data(kTaskAssigneeRole).toString();
        const QPixmap agentFace =
            index.data(kTaskAgentFaceRole).value<QPixmap>();
        const bool agentAssigned =
            assigneeKind == QLatin1String("agent") || !agentFace.isNull();
        const QString assigneeLabel =
            agentAssigned ? QStringLiteral("Bot")
            : assigneeKind == QLatin1String("unassigned") ? QString()
                                                          : assignee;

        struct CompactColumn {
            TaskGlyph glyph;
            QString text;
            int chars;
            bool agent;
            bool person;
        };
        const QList<CompactColumn> columns{
            {taskStatusGlyph(statusKey, dark), taskStatusLabel(statusKey), 11,
             false, false},
            {taskAssigneeGlyph(assigneeKind, assignee, dark), assigneeLabel, 12,
             agentAssigned, !agentAssigned},
            {taskTimeGlyph(dark),
             taskCreatedAgo(index.data(kTaskCreatedAtRole).toLongLong()), 7,
             false, false},
        };
        const int minTitle = 120;
        int right = rect.right() - 8;
        if (index.data(kTaskAttachmentCountRole).toInt() > 0 &&
            right - kTaskMetaGlyphSize - kTaskMetaGap >= x0 + minTitle) {
            painter->drawPixmap(right - kTaskMetaGlyphSize, glyphTop,
                                tintedOcticonPixmap(
                                    QStringLiteral("paperclip"),
                                    selected ? QColor("#ffffff") : muted,
                                    kTaskMetaGlyphSize));
            right -= kTaskMetaGlyphSize + kTaskMetaGap;
        }
        int columnsLeft = right;
        int shown = 0;
        for (int i = columns.size() - 1; i >= 0; --i) {
            const int width =
                kTaskMetaGlyphSize + 4 + columns.at(i).chars * charW;
            if (columnsLeft - width - kTaskMetaGap < x0 + minTitle)
                break;
            columnsLeft -= width + kTaskMetaGap;
            ++shown;
        }
        int x = columnsLeft + kTaskMetaGap;
        for (int i = columns.size() - shown; i < columns.size(); ++i) {
            const CompactColumn &column = columns.at(i);
            const int width = kTaskMetaGlyphSize + 4 + column.chars * charW;
            if (!column.text.isEmpty()) {
                if (column.agent && !agentFace.isNull()) {
                    const QRect face(x, glyphTop, kTaskMetaGlyphSize,
                                     kTaskMetaGlyphSize);
                    painter->drawPixmap(face, agentFace);
                    if (index.data(kTaskAgentWorkingRole).toBool()) {
                        const int orbit = kTaskMetaGlyphSize + 8;
                        painter->drawPixmap(
                            QPoint(face.center().x() - orbit / 2,
                                   face.center().y() - orbit / 2),
                            agentRunningFireballPixmap(
                                orbit,
                                index.data(kTaskAgentAngleRole).toReal()));
                    }
                } else {
                    const TaskGlyph &glyph = column.glyph;
                    QPixmap mark = taskRepositoryLogoPixmap(
                        glyph.logo, kTaskMetaGlyphSize, glyph.avatarRadius);
                    if (mark.isNull() && !glyph.avatarSeed.isEmpty())
                        mark = taskPersonFace(
                            column.person ? index.data(kTaskAssigneePictureRole)
                                          : QVariant(),
                            glyph.avatarSeed, kTaskMetaGlyphSize,
                            glyph.avatarRadius);
                    if (mark.isNull() && !glyph.icon.isEmpty())
                        mark = tintedOcticonPixmap(
                            glyph.icon,
                            selected ? QColor("#ffffff") : glyph.color,
                            kTaskMetaGlyphSize);
                    if (!mark.isNull())
                        painter->drawPixmap(x, glyphTop, mark);
                }
                const int textX = x + kTaskMetaGlyphSize + 4;
                const int room = x + width - textX;
                painter->setPen(muted);
                painter->drawText(
                    QRect(textX, rect.top(), room, rect.height()),
                    Qt::AlignVCenter | Qt::AlignLeft,
                    metrics.elidedText(column.text, Qt::ElideRight, room));
            }
            x += width + kTaskMetaGap;
        }

        const QRect titleRect(x0, rect.top(),
                              qMax(0, columnsLeft - x0), rect.height());
        QFont titleFont = option.font;
        titleFont.setStrikeOut(done);
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        painter->drawText(
            titleRect, Qt::AlignVCenter | Qt::AlignLeft,
            painter->fontMetrics().elidedText(
                index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                qMax(0, titleRect.width())));
    }

    forkmesh::ui::ListViewMode m_mode = forkmesh::ui::ListViewMode::Full;
};

QJsonObject selectedOrganizationTask(
    QTableWidget *table, const QJsonArray &tasks)
{
    if (!table || table->currentRow() < 0)
        return {};
    QTableWidgetItem *item = table->item(table->currentRow(), 0);
    if (!item)
        return {};
    const int index = item->data(Qt::UserRole).toInt();
    return index >= 0 && index < tasks.size() ? tasks.at(index).toObject()
                                              : QJsonObject();
}

QString organizationTaskPromptText(const QJsonObject &task)
{
    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
    QStringList lines;
    lines << QStringLiteral("[task:%1] %2")
                 .arg(taskText(task, QStringLiteral("id")),
                      taskText(task, QStringLiteral("title")));
    const QString repository = taskText(task, QStringLiteral("repository"));
    if (!repository.isEmpty())
        lines << QStringLiteral("Repository: %1").arg(repository);
    const QString details = taskText(task, QStringLiteral("details"));
    if (!details.isEmpty())
        lines << QString() << details;
    const QString howToTest = taskText(qa, QStringLiteral("howToTest"));
    if (!howToTest.isEmpty())
        lines << QString() << QStringLiteral("How to test: %1").arg(howToTest);
    return lines.join(QLatin1Char('\n'));
}

QString organizationTaskIssueBody(const QJsonObject &task)
{
    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
    QStringList sections;
    const QString details = taskText(task, QStringLiteral("details"));
    if (!details.isEmpty())
        sections.append(details);

    const QString howToTest = taskText(qa, QStringLiteral("howToTest"));
    if (!howToTest.isEmpty())
        sections.append(QStringLiteral("## How to test\n%1").arg(howToTest));

    const QString completionNote =
        taskText(task, QStringLiteral("completionNote"));
    if (!completionNote.isEmpty())
        sections.append(
            QStringLiteral("## Completion note\n%1").arg(completionNote));

    const QString taskId = taskText(task, QStringLiteral("id"));
    if (!taskId.isEmpty())
        sections.append(
            QStringLiteral("---\nMoved from organization task `%1`.")
                .arg(taskId));
    return sections.join(QStringLiteral("\n\n"));
}

constexpr int kTaskInlineImageMaxSide = 1200;

QString taskImageMimeForPath(const QString &path)
{
    const QString lower = path.toLower();
    if (lower.endsWith(QStringLiteral(".png")))
        return QStringLiteral("image/png");
    if (lower.endsWith(QStringLiteral(".jpg")) ||
        lower.endsWith(QStringLiteral(".jpeg")))
        return QStringLiteral("image/jpeg");
    if (lower.endsWith(QStringLiteral(".gif")))
        return QStringLiteral("image/gif");
    if (lower.endsWith(QStringLiteral(".webp")))
        return QStringLiteral("image/webp");
    return {};
}

bool taskImageMimeAllowed(const QString &mime)
{
    return mime == QLatin1String("image/png") ||
           mime == QLatin1String("image/jpeg") ||
           mime == QLatin1String("image/gif") ||
           mime == QLatin1String("image/webp");
}

QByteArray taskImagePreviewBytes(const QString &path, QString *mime)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull())
        return {};
    QByteArray encoded;
    for (const int side : {1200, 960, 720, 540}) {
        QImage shown = image;
        if (shown.width() > side || shown.height() > side)
            shown = shown.scaled(side, side, Qt::KeepAspectRatio,
                                 Qt::SmoothTransformation);
        for (const int quality : {82, 72, 62, 52}) {
            encoded.clear();
            QBuffer buffer(&encoded);
            if (!buffer.open(QIODevice::WriteOnly))
                return {};
            QImageWriter writer(&buffer, QByteArrayLiteral("jpg"));
            writer.setQuality(quality);
            if (!writer.write(shown.convertToFormat(QImage::Format_RGB32)))
                return {};
            if (encoded.size() <= 256 * 1024)
                break;
        }
        if (encoded.size() <= 256 * 1024)
            break;
    }
    if (encoded.size() > 256 * 1024)
        return {};
    if (mime)
        *mime = QStringLiteral("image/jpeg");
    return encoded;
}

QJsonArray organizationTaskImageAttachments(const QStringList &paths,
                                            QString *error)
{
    constexpr qsizetype maxEach = 256 * 1024;
    constexpr qsizetype maxTotal = 1024 * 1024;
    if (paths.size() > 4) {
        if (error)
            *error = QStringLiteral(
                "A task can include up to four images.");
        return {};
    }
    QJsonArray attachments;
    qsizetype total = 0;
    for (const QString &path : paths) {
        if (isVideoAttachmentPath(path)) {
            if (error)
                *error = QStringLiteral(
                    "Organization tasks accept image attachments only.");
            return {};
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("Could not read task image %1.")
                             .arg(QFileInfo(path).fileName());
            return {};
        }
        QByteArray bytes = file.readAll();
        QString mime = taskImageMimeForPath(path);
        if (bytes.isEmpty() || QImage::fromData(bytes).isNull()) {
            if (error)
                *error = QStringLiteral("Could not decode task image %1.")
                             .arg(QFileInfo(path).fileName());
            return {};
        }
        if (bytes.size() > maxEach || !taskImageMimeAllowed(mime)) {
            QString previewMime;
            bytes = taskImagePreviewBytes(path, &previewMime);
            mime = previewMime;
        }
        if (bytes.isEmpty() || bytes.size() > maxEach) {
            if (error)
                *error = QStringLiteral(
                    "Task image %1 is larger than 256 KiB after resizing.")
                             .arg(QFileInfo(path).fileName());
            return {};
        }
        total += bytes.size();
        if (total > maxTotal) {
            if (error)
                *error = QStringLiteral(
                    "Task images must total 1 MiB or less.");
            return {};
        }
        QImage thumbnailImage = QImage::fromData(bytes);
        thumbnailImage = thumbnailImage.scaled(48, 48, Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation);
        QByteArray thumbnailBytes;
        QBuffer thumbnailBuffer(&thumbnailBytes);
        thumbnailBuffer.open(QIODevice::WriteOnly);
        QImageWriter thumbnailWriter(&thumbnailBuffer, QByteArrayLiteral("jpg"));
        thumbnailWriter.setQuality(72);
        thumbnailWriter.write(thumbnailImage.convertToFormat(QImage::Format_RGB32));
        attachments.append(QJsonObject{
            {QStringLiteral("name"), QFileInfo(path).fileName()},
            {QStringLiteral("mime"), mime},
            {QStringLiteral("size"), bytes.size()},
            {QStringLiteral("file"), QString::fromLatin1(bytes.toBase64())},
            {QStringLiteral("thumbnail"),
             QStringLiteral("data:image/jpeg;base64,") +
                 QString::fromLatin1(thumbnailBytes.toBase64())},
        });
    }
    return attachments;
}

QByteArray taskImageDataUriBytes(const QString &value, QString *mime)
{
    const QRegularExpressionMatch match = QRegularExpression(
        QStringLiteral("^data:(image/(?:png|jpeg|gif|webp));base64,"
                       "([A-Za-z0-9+/=]+)$"))
        .match(value.trimmed());
    if (!match.hasMatch())
        return {};
    if (mime)
        *mime = match.captured(1);
    return QByteArray::fromBase64(match.captured(2).toLatin1());
}

QString taskImageCacheKey(const QString &taskId, const QString &attachmentId)
{
    return taskId + QLatin1String("|attachment|") + attachmentId;
}

QString taskLocalImageCacheKey(const QString &taskId, const QString &path)
{
    return taskId + QLatin1String("|local|") +
           QString::number(qHash(path), 16);
}

QPixmap taskRowThumbnailPixmap(const QString &key, const QByteArray &bytes)
{
    static QHash<QString, QPixmap> cache;
    const qreal dpr = iconDevicePixelRatio();
    const QString cacheKey = key + QLatin1Char('|') + QString::number(dpr);
    const auto cached = cache.constFind(cacheKey);
    if (cached != cache.constEnd())
        return cached.value();
    QPixmap thumb;
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    const int target = qMax(1, qRound(kTaskRowThumbSide * dpr));
    if (full.isValid() && !full.isEmpty())
        reader.setScaledSize(
            full.scaled(target, target, Qt::KeepAspectRatioByExpanding));
    const QImage image = reader.read();
    if (!image.isNull()) {
        QPixmap square(target, target);
        square.fill(Qt::transparent);
        QPainter p(&square);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QImage filled =
            image.scaled(target, target, Qt::KeepAspectRatioByExpanding,
                         Qt::SmoothTransformation);
        p.drawImage(QPoint((target - filled.width()) / 2,
                           (target - filled.height()) / 2),
                    filled);
        p.end();
        square.setDevicePixelRatio(dpr);
        thumb = square;
    }
    if (cache.size() > 256)
        cache.clear();
    cache.insert(cacheKey, thumb);
    return thumb;
}

QVariantList taskRowThumbnails(const QJsonObject &task,
                               const QHash<QString, QByteArray> &images)
{
    const QString taskId = taskText(task, QStringLiteral("id"));
    QVariantList thumbnails;
    for (const QJsonValue &value :
         task.value(QStringLiteral("attachments")).toArray()) {
        if (thumbnails.size() >= kTaskRowThumbMax)
            break;
        const QJsonObject attachment = value.toObject();
        const QString attachmentId =
            taskText(attachment, QStringLiteral("id"));
        const QString cacheKey = taskImageCacheKey(taskId, attachmentId);
        const QByteArray inlineBytes = taskImageDataUriBytes(
            taskText(attachment, QStringLiteral("thumbnail")), nullptr);
        QPixmap thumb;
        if (!inlineBytes.isEmpty())
            thumb = taskRowThumbnailPixmap(
                cacheKey + QLatin1String("|thumbnail|") +
                    QString::number(qHash(inlineBytes), 16),
                inlineBytes);
        else if (!attachmentId.isEmpty() && images.contains(cacheKey))
            thumb = taskRowThumbnailPixmap(cacheKey + QLatin1String("|full"),
                                           images.value(cacheKey));
        thumbnails.append(QVariant::fromValue(thumb));
    }
    return thumbnails;
}

QString taskImageHref(const QString &cacheKey)
{
    return QStringLiteral("forkmesh-task-image:") +
           QString::fromLatin1(QUrl::toPercentEncoding(cacheKey));
}

// The @names in a comment or check-in note, in the same shape the relay pings
// on (its MENTION_RE), so the status line can only ever claim a ping the relay
// will actually send. The author's own name is dropped: a self-ping is
// discarded server-side too.
QStringList taskMentionedNames(const QString &copy, const QString &self)
{
    static const QRegularExpression pattern(
        QStringLiteral("(?<![A-Za-z0-9._%+-])@([a-z](?:[a-z0-9-]{0,61}"
                       "[a-z0-9])?)\\b"));
    QStringList found;
    auto matches = pattern.globalMatch(copy.toLower());
    while (matches.hasNext()) {
        const QString name = matches.next().captured(1);
        if (!name.isEmpty() && !found.contains(name) &&
            name.compare(self, Qt::CaseInsensitive) != 0)
            found.append(name);
    }
    return found;
}

// The timeline vocabulary the relay records, in the desktop's own words. An
// unknown kind from a newer relay falls back to the raw kind rather than being
// dropped, so a task never appears to have a gap in its history.
QString taskEventLabel(const QString &kind)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("created"), QStringLiteral("created this task")},
        {QStringLiteral("updated"), QStringLiteral("edited this task")},
        {QStringLiteral("started"), QStringLiteral("started work")},
        {QStringLiteral("stopped"), QStringLiteral("paused work")},
        {QStringLiteral("checkin"), QStringLiteral("checked in")},
        {QStringLiteral("replied"), QStringLiteral("commented")},
        {QStringLiteral("completed"), QStringLiteral("completed this task")},
        {QStringLiteral("reopened"), QStringLiteral("reopened this task")},
        {QStringLiteral("returned"),
         QStringLiteral("returned this task to the list")},
        {QStringLiteral("qa_requested"), QStringLiteral("sent this to QA")},
        {QStringLiteral("qa_reviewed"), QStringLiteral("reviewed this in QA")},
        {QStringLiteral("deleted"), QStringLiteral("deleted this task")},
        {QStringLiteral("reminded"), QStringLiteral("sent a reminder")},
        {QStringLiteral("reminder_seen"), QStringLiteral("saw the reminder")},
        {QStringLiteral("emailed"), QStringLiteral("set off an email")},
        {QStringLiteral("email_opened"), QStringLiteral("opened the email")},
        {QStringLiteral("email_clicked"),
         QStringLiteral("clicked a link in the email")},
        {QStringLiteral("email_viewed"),
         QStringLiteral("opened the task from the email")},
    };
    return labels.value(kind, kind);
}

QJsonArray taskHistoryWithReplies(const QJsonArray &history,
                                  const QJsonArray &replies)
{
    QJsonArray merged;
    for (const QJsonValue &value : history) {
        QJsonObject entry = value.toObject();
        if (taskText(entry, QStringLiteral("kind")) ==
            QLatin1String("replied")) {
            const QString actor = taskText(entry, QStringLiteral("actor"));
            const qint64 at =
                qint64(entry.value(QStringLiteral("createdAt")).toDouble());
            for (const QJsonValue &candidate : replies) {
                const QJsonObject reply = candidate.toObject();
                if (taskText(reply, QStringLiteral("author")) != actor ||
                    qint64(reply.value(QStringLiteral("createdAt"))
                               .toDouble()) != at)
                    continue;
                entry.insert(QStringLiteral("body"),
                             reply.value(QStringLiteral("body")));
                entry.insert(QStringLiteral("readBy"),
                             reply.value(QStringLiteral("readBy")).toArray());
                break;
            }
        }
        merged.append(entry);
    }
    return merged;
}

QString taskCommentReceiptHtml(const QJsonObject &entry)
{
    if (entry.value(QStringLiteral("pending")).toBool())
        return QStringLiteral("Sending\u2026");
    if (!entry.contains(QStringLiteral("readBy")))
        return QString();
    QStringList readers;
    for (const QJsonValue &value :
         entry.value(QStringLiteral("readBy")).toArray()) {
        const QString name =
            taskText(value.toObject(), QStringLiteral("name"));
        if (!name.isEmpty())
            readers.append(QStringLiteral("@%1").arg(name));
    }
    return readers.isEmpty()
               ? QStringLiteral("Not read yet")
               : QStringLiteral("\u2713 Read by %1")
                     .arg(readers.join(QStringLiteral(", ")));
}

QString taskHistoryHtml(const QJsonArray &history, bool dark)
{
    QString html = QStringLiteral(
                       "<h3 style='margin:14px 0 4px 0'>%1&nbsp;Activity</h3>")
                       .arg(taskGlyphMarkup(taskTimeGlyph(dark), 15));
    if (history.isEmpty()) {
        return html + QStringLiteral(
                          "<p style='margin:0;color:#8b949e'>No activity "
                          "recorded on this task yet.</p>");
    }
    html += QStringLiteral("<table cellspacing='0'>");
    for (const QJsonValue &value : history) {
        const QJsonObject entry = value.toObject();
        const QString actor = taskText(entry, QStringLiteral("actor"));
        const QString summary = taskText(entry, QStringLiteral("summary"));
        const QString body = taskText(entry, QStringLiteral("body"));
        if (taskText(entry, QStringLiteral("kind")) ==
                QLatin1String("replied") &&
            !body.isEmpty()) {
            const QString receipt = taskCommentReceiptHtml(entry);
            html +=
                QStringLiteral(
                    "<tr><td valign='top' style='padding:3px 12px 3px 0;"
                    "color:#8b949e;white-space:nowrap'>%1</td>"
                    "<td style='padding:3px 0'><b>%2</b> %3"
                    "<div style='margin:2px 0 0 0'>%4</div>%5</td></tr>")
                    .arg(taskTimestamp(qint64(
                        entry.value(QStringLiteral("createdAt")).toDouble())))
                    .arg(actor.isEmpty()
                             ? QStringLiteral("Someone")
                             : QStringLiteral("@%1").arg(
                                   actor.toHtmlEscaped()))
                    .arg(taskEventLabel(QStringLiteral("replied"))
                             .toHtmlEscaped())
                    .arg(forkmesh::taskmarkdown::toHtml(body))
                    .arg(receipt.isEmpty()
                             ? QString()
                             : QStringLiteral(
                                   "<div style='color:#8b949e;"
                                   "font-size:small'>%1</div>")
                                   .arg(receipt.toHtmlEscaped()));
            continue;
        }
        html +=
            QStringLiteral(
                "<tr><td valign='top' style='padding:3px 12px 3px 0;"
                "color:#8b949e;white-space:nowrap'>%1</td>"
                "<td style='padding:3px 0'><b>%2</b> %3%4</td></tr>")
                .arg(taskTimestamp(qint64(
                         entry.value(QStringLiteral("createdAt")).toDouble())))
                .arg(actor.isEmpty()
                         ? QStringLiteral("Someone")
                         : QStringLiteral("@%1").arg(actor.toHtmlEscaped()))
                .arg(taskEventLabel(taskText(entry, QStringLiteral("kind")))
                         .toHtmlEscaped())
                .arg(summary.isEmpty()
                         ? QString()
                         : QStringLiteral(" &mdash; %1")
                               .arg(summary.toHtmlEscaped()));
    }
    return html + QStringLiteral("</table>");
}

QString taskFieldsHtml(const QJsonObject &task, const QString &localAgentRun,
                       int localAgentSessionId)
{
    if (task.isEmpty())
        return QString();
    const bool dark = currentThemeIsDark();
    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
    // Every field carries the same mark the list row and the hover card give
    // it, so a task reads identically wherever it is shown.
    auto row = [](const TaskGlyph &glyph, const QString &label,
                  const QString &value, bool valueIsHtml = false) {
        const QString shown =
            value.trimmed().isEmpty() ? QString::fromUtf8("\xE2\x80\x94")
                                      : valueIsHtml ? value
                                                    : value.toHtmlEscaped();
        return QStringLiteral(
                   "<tr><td style='padding:3px 7px 3px 0'>%1</td>"
                   "<th align='left' style='padding:3px 12px 3px 0;"
                   "color:#8b949e'>%2</th><td style='padding:3px 0'>%3</td></tr>")
            .arg(taskGlyphMarkup(glyph, 14), label.toHtmlEscaped(), shown);
    };
    QString html = QStringLiteral("<table cellspacing='0'>");
    html += row(taskStatusGlyph(taskStatusKey(task), dark),
                QStringLiteral("Status"),
                taskText(task, QStringLiteral("status")));
    html += row(taskPriorityGlyph(dark), QStringLiteral("Priority"),
                QString::number(task.value(QStringLiteral("priority")).toInt()));
    const QString department = taskText(task, QStringLiteral("department"));
    html += row(taskDepartmentGlyph(department), QStringLiteral("Department"),
                department);
    html += row({QStringLiteral("people"), taskMutedColor(dark), {}, 0.5},
                QStringLiteral("Team"), taskText(task, QStringLiteral("team")));
    const QString destination = taskText(task, QStringLiteral("destination"));
    html += row(taskDestinationGlyph(destination, dark),
                QStringLiteral("Destination"), destination);
    const QString repository = taskText(task, QStringLiteral("repository"));
    html += row(taskRepositoryGlyph(repository), QStringLiteral("Repository"),
                repository);
    html += row(taskAssigneeGlyph(taskText(task, QStringLiteral("assigneeKind")),
                                  taskText(task, QStringLiteral("assignee")),
                                  dark),
                QStringLiteral("Assignee"), taskAssigneeLabel(task));
    const QString createdBy = taskText(task, QStringLiteral("createdBy"));
    html += row(taskPersonGlyph(createdBy, dark), QStringLiteral("Created by"),
                createdBy);
    html += row({QStringLiteral("link"), taskMutedColor(dark), {}, 0.5},
                QStringLiteral("Follows task"),
                taskText(task, QStringLiteral("parentTaskId")));
    QString agentSession =
        taskText(task, QStringLiteral("agentSessionId")).toHtmlEscaped();
    if (!localAgentRun.isEmpty()) {
        const QString local =
            localAgentSessionId > 0
                ? (QStringLiteral("<a href=\"%1%2\" style=\"color:") +
                   Theme::nextOrHex(Theme::Role::AccentFg,
                                    QStringLiteral("#58a6ff")) +
                   QStringLiteral(";text-decoration:none\">%3</a>"))
                      .arg(kAgentLinkScheme)
                      .arg(localAgentSessionId)
                      .arg(localAgentRun.toHtmlEscaped())
                : localAgentRun.toHtmlEscaped();
        agentSession = agentSession.isEmpty()
                           ? local
                           : agentSession + QString::fromUtf8(" \xC2\xB7 ") + local;
    }
    html += row({QStringLiteral("terminal"), taskMutedColor(dark), {}, 0.5},
                QStringLiteral("Agent session"), agentSession,
                /*valueIsHtml=*/true);
    const QString qaStatus = taskText(qa, QStringLiteral("status"));
    const bool qaRequested =
        qa.value(QStringLiteral("requestedAt")).toDouble() > 0;
    html += row(taskQaGlyph(qaStatus, qaRequested, dark, nullptr),
                QStringLiteral("QA status"), qaStatus);
    const QString qaReviewer = taskText(qa, QStringLiteral("reviewer"));
    html += row(taskPersonGlyph(qaReviewer, dark), QStringLiteral("QA reviewer"),
                qaReviewer);
    html += row(taskTimeGlyph(dark), QStringLiteral("QA requested"),
                taskTimestamp(
                    qint64(qa.value(QStringLiteral("requestedAt")).toDouble())));
    html += row(taskTimeGlyph(dark), QStringLiteral("Updated"),
                taskTimestamp(qint64(
                    task.value(QStringLiteral("updatedAt")).toDouble())));
    const QJsonObject reminder =
        task.value(QStringLiteral("lastReminder")).toObject();
    if (!reminder.isEmpty()) {
        const QString by = taskText(reminder, QStringLiteral("by"));
        const QString note = taskText(reminder, QStringLiteral("note"));
        QString value =
            taskTimestamp(qint64(reminder.value(QStringLiteral("at")).toDouble()));
        if (!by.isEmpty())
            value += QStringLiteral(" by @") + by;
        if (!note.isEmpty())
            value += QString::fromUtf8(" \xC2\xB7 ") + note;
        const QString to = taskText(reminder, QStringLiteral("to"));
        const QString who =
            to.isEmpty() ? QStringLiteral("the assignee")
                         : QStringLiteral("@") + to;
        const qint64 seenAt =
            qint64(reminder.value(QStringLiteral("seenAt")).toDouble());
        if (seenAt > 0)
            value += QStringLiteral(" \u00B7 Seen %1 \u2014 %2 %3")
                         .arg(taskTimestamp(seenAt), who,
                              taskText(reminder, QStringLiteral("seenVia")) ==
                                      QLatin1String("ping")
                                  ? QStringLiteral("opened the ping")
                                  : QStringLiteral("opened the task"));
        else if (reminder.value(QStringLiteral("delivered")).isBool() &&
                 !reminder.value(QStringLiteral("delivered")).toBool())
            value += QStringLiteral(
                " \u00B7 Not delivered \u2014 the ping could not be filed");
        else
            value += QStringLiteral(" \u00B7 Not seen yet by %1").arg(who);
        html += row({QStringLiteral("bell"), taskMutedColor(dark), {}, 0.5},
                    QStringLiteral("Last reminder"), value);
    }
    QStringList workItems;
    for (const forkmesh::taskwork::WorkItem &item :
         forkmesh::taskwork::fromJsonArray(
             task.value(QStringLiteral("workItems")).toArray()))
        workItems.append(
            (item.node.isEmpty() ? item.branch
                                 : item.branch + QStringLiteral(" @ ") + item.node)
                .toHtmlEscaped());
    html += row({QStringLiteral("git-branch"), taskMutedColor(dark), {}, 0.5},
                QStringLiteral("Work items"),
                workItems.join(QStringLiteral("<br>")), /*valueIsHtml=*/true);
    html += row({QStringLiteral("tag"), taskMutedColor(dark), {}, 0.5},
                QStringLiteral("Task ID"), taskText(task, QStringLiteral("id")));
    return html + QStringLiteral("</table>");
}

QString taskDetailHtml(const QJsonObject &task,
                       const QHash<QString, QByteArray> &imageData,
                       const QHash<QString, QString> &imageMime,
                       const QJsonArray &history)
{
    if (task.isEmpty()) {
        return QStringLiteral(
            "<p style='color:#8b949e'>Select a task to inspect its private "
            "details and available actions.</p>");
    }
    const bool dark = currentThemeIsDark();
    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
    QString html =
        QStringLiteral("<h2 style='margin:0 0 8px 0'>%1</h2>")
            .arg(taskText(task, QStringLiteral("title")).toHtmlEscaped());
    auto block = [&html](const TaskGlyph &glyph, const QString &heading,
                         const QString &value) {
        const QString rendered = forkmesh::taskmarkdown::toHtml(value);
        html += QStringLiteral(
                    "<h3 style='margin:14px 0 4px 0'>%1&nbsp;%2</h3>"
                    "<div style='margin:0'>%3</div>")
                    .arg(taskGlyphMarkup(glyph, 15), heading.toHtmlEscaped(),
                         (rendered.isEmpty()
                              ? QString::fromUtf8("\xE2\x80\x94")
                              : rendered));
    };
    const QString details = taskText(task, QStringLiteral("details"));
    block({QStringLiteral("note"), taskMutedColor(dark), {}, 0.5},
          QStringLiteral("Details"), promptTextWithoutAttachments(details));
    block({QStringLiteral("shield-check"), taskMutedColor(dark), {}, 0.5},
          QStringLiteral("How to test"),
          taskText(qa, QStringLiteral("howToTest")));
    block(taskStatusGlyph(QStringLiteral("done"), dark),
          QStringLiteral("Completion note"),
          taskText(task, QStringLiteral("completionNote")));

    const QJsonArray attachments =
        task.value(QStringLiteral("attachments")).toArray();
    QString attachmentHtml;
    for (const QJsonValue &value : attachments) {
        const QJsonObject attachment = value.toObject();
        const QString id = taskText(attachment, QStringLiteral("id"));
        const QString key = taskImageCacheKey(
            taskText(task, QStringLiteral("id")), id);
        const QByteArray bytes = imageData.value(key);
        const QString mime = imageMime.value(
            key, taskText(attachment, QStringLiteral("mime")));
        if (!bytes.isEmpty() && taskImageMimeAllowed(mime)) {
            attachmentHtml += QStringLiteral(
                "<a href=\"%1\"><img src=\"data:%2;base64,%3\" "
                "width=\"640\" style=\"max-width:100%;height:auto;"
                "border-radius:6px;margin:4px 8px 4px 0\"></a>")
                .arg(taskImageHref(key), mime,
                     QString::fromLatin1(bytes.toBase64()));
        } else {
            attachmentHtml += QStringLiteral(
                "<p style='color:#8b949e;margin:4px 0'>Loading image %1...</p>")
                .arg(taskText(attachment, QStringLiteral("name"))
                         .toHtmlEscaped());
        }
    }
    for (const QString &path : promptAttachmentPaths(details)) {
        if (isVideoAttachmentPath(path) || !QFileInfo::exists(path))
            continue;
        const QString key = taskLocalImageCacheKey(
            taskText(task, QStringLiteral("id")), path);
        const QByteArray bytes = imageData.value(key);
        const QString mime = imageMime.value(key, taskImageMimeForPath(path));
        if (bytes.isEmpty() || !taskImageMimeAllowed(mime))
            continue;
        attachmentHtml += QStringLiteral(
            "<a href=\"%1\"><img src=\"data:%2;base64,%3\" "
            "width=\"640\" style=\"max-width:100%;height:auto;"
            "border-radius:6px;margin:4px 8px 4px 0\"></a>")
            .arg(taskImageHref(key), mime,
                 QString::fromLatin1(bytes.toBase64()));
    }
    if (!attachmentHtml.isEmpty()) {
        html += QStringLiteral(
            "<h3 style='margin:14px 0 4px 0'>%1&nbsp;Attachments</h3>"
            "<div style='white-space:normal'>%2</div>")
            .arg(taskGlyphMarkup({QStringLiteral("paperclip"),
                                  taskMutedColor(dark), {}, 0.5}, 15),
                 attachmentHtml);
    }
    html += taskHistoryHtml(history, dark);
    return html;
}

struct TaskCheckboxToggle {
    QString field;
    QString text;
};

TaskCheckboxToggle taskCheckboxToggle(const QJsonObject &task, int index)
{
    const QString details = taskText(task, QStringLiteral("details"));
    const int detailBoxes = forkmesh::taskmarkdown::taskItemCount(
        promptTextWithoutAttachments(details));
    if (index < detailBoxes)
        return {QStringLiteral("details"),
                forkmesh::taskmarkdown::toggleTaskItem(details, index)};
    return {QStringLiteral("howToTest"),
            forkmesh::taskmarkdown::toggleTaskItem(
                taskText(task.value(QStringLiteral("qa")).toObject(),
                         QStringLiteral("howToTest")),
                index - detailBoxes)};
}

class TaskCheckboxClickFilter : public QObject
{
public:
    TaskCheckboxClickFilter(QTextBrowser *browser,
                            std::function<bool(int)> canToggle,
                            std::function<void(int)> toggle)
        : QObject(browser), m_browser(browser),
          m_canToggle(std::move(canToggle)), m_toggle(std::move(toggle))
    {
        m_browser->viewport()->setMouseTracking(true);
        m_browser->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseMove:
            setHover(toggleableIndexAt(static_cast<QMouseEvent *>(event)) >= 0);
            return m_hover;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            m_pressed = mouse->button() == Qt::LeftButton
                            ? toggleableIndexAt(mouse)
                            : -1;
            return m_pressed >= 0;
        }
        case QEvent::MouseButtonRelease: {
            const int pressed = std::exchange(m_pressed, -1);
            if (pressed < 0)
                return false;
            if (toggleableIndexAt(static_cast<QMouseEvent *>(event)) == pressed)
                m_toggle(pressed);
            return true;
        }
        case QEvent::Leave:
            setHover(false);
            return false;
        default:
            return false;
        }
    }

private:
    int toggleableIndexAt(const QMouseEvent *mouse) const
    {
        const QPointF position =
            mouse->position() +
            QPointF(m_browser->horizontalScrollBar()->value(),
                    m_browser->verticalScrollBar()->value());
        const int index = forkmesh::taskmarkdown::taskItemIndexAt(
            m_browser->document(), position);
        return index >= 0 && m_canToggle(index) ? index : -1;
    }

    void setHover(bool hover)
    {
        if (hover == m_hover)
            return;
        m_hover = hover;
        QWidget *viewport = m_browser->viewport();
        if (hover) {
            m_restoreCursor = viewport->cursor();
            viewport->setCursor(Qt::PointingHandCursor);
        } else {
            viewport->setCursor(m_restoreCursor);
        }
    }

    QTextBrowser *m_browser = nullptr;
    std::function<bool(int)> m_canToggle;
    std::function<void(int)> m_toggle;
    QCursor m_restoreCursor;
    bool m_hover = false;
    int m_pressed = -1;
};

} // namespace

QWidget *MainWindow::buildOrganizationTasksSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("organizationTasksSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 4, 12, 18);
    outer->setSpacing(6);

    auto *headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(8, 0, 0, 0);
    headingRow->setSpacing(4);

    m_organizationTaskDeleteClosedButton =
        new VerticalIconButton(QStringLiteral("Delete closed"),
                               VerticalIconButton::Action);
    m_organizationTaskDeleteClosedButton->setObjectName(
        QStringLiteral("organizationTaskDeleteClosedAction"));
    m_organizationTaskDeleteClosedButton->setToolTip(
        QStringLiteral("Move every closed task to the trash "
                       "(restorable for 30 days on the web Tasks page)"));
    setOcticon(m_organizationTaskDeleteClosedButton, "trash", 16);
    static_cast<VerticalIconButton *>(m_organizationTaskDeleteClosedButton)
        ->setAccentColor(
            QColor(currentThemeIsDark() ? "#f85149" : "#cf222e"));
    connect(m_organizationTaskDeleteClosedButton, &QPushButton::clicked, this,
            &MainWindow::deleteClosedOrganizationTasks);

    m_organizationTaskNewButton =
        new VerticalIconButton(QStringLiteral("New task"),
                               VerticalIconButton::Action);
    m_organizationTaskNewButton->setObjectName(
        QStringLiteral("organizationTaskNewAction"));
    setOcticon(m_organizationTaskNewButton, "plus", 16);
    connect(m_organizationTaskNewButton, &QPushButton::clicked, this,
            &MainWindow::createOrganizationTask);
    headingRow->addWidget(m_organizationTaskNewButton);

    auto *refresh = new VerticalIconButton(QStringLiteral("Refresh"),
                                           VerticalIconButton::Action);
    refresh->setObjectName(QStringLiteral("organizationTaskRefreshAction"));
    setOcticon(refresh, "sync", 16);
    refresh->setToolTip(QStringLiteral("Refresh the private task catalog"));
    connect(refresh, &QPushButton::clicked, this,
            &MainWindow::refreshOrganizationTasks);
    headingRow->addWidget(refresh);
    m_organizationTaskRefreshButton = refresh;

    headingRow->addWidget(m_organizationTaskDeleteClosedButton);

    m_organizationTaskViewButton =
        new VerticalIconButton(QStringLiteral("Compact"),
                               VerticalIconButton::Action);
    m_organizationTaskViewButton->setFixedWidth(
        m_organizationTaskViewButton->sizeHint().width());
    m_organizationTaskViewButton->setObjectName(
        QStringLiteral("organizationTaskViewAction"));
    m_organizationTaskViewButton->setAccessibleDescription(
        QStringLiteral("Change how the task list shows each task"));
    connect(m_organizationTaskViewButton, &QPushButton::clicked, this, [this] {
        setOrganizationTaskListView(
            forkmesh::ui::nextListViewMode(m_organizationTaskListView));
    });
    headingRow->addWidget(m_organizationTaskViewButton);

    m_organizationTaskSortButton =
        new VerticalIconButton(QStringLiteral("Newest"),
                               VerticalIconButton::Action);
    m_organizationTaskSortButton->setObjectName(
        QStringLiteral("organizationTaskSortAction"));
    setOcticon(m_organizationTaskSortButton, "sort-desc", 16);
    m_organizationTaskSortButton->setToolTip(
        QStringLiteral("Sort tasks (currently newest created first)"));
    auto *sortMenu = new QMenu(m_organizationTaskSortButton);
    auto *sortGroup = new QActionGroup(sortMenu);
    sortGroup->setExclusive(true);
    struct TaskSortChoice {
        const char *menuLabel;
        const char *buttonLabel;
        const char *key;
    };
    static const TaskSortChoice sortChoices[] = {
        {"Newest created", "Newest", "created-desc"},
        {"Oldest created", "Oldest", "created-asc"},
        {"Recently updated", "Updated", "updated-desc"},
        {"Least recently updated", "Least updated", "updated-asc"},
        {"Highest priority", "Priority \xE2\x86\x91", "priority-asc"},
        {"Lowest priority", "Priority \xE2\x86\x93", "priority-desc"},
        {"Title A\xE2\x80\x93Z", "Title A\xE2\x80\x93Z", "title-asc"},
        {"Title Z\xE2\x80\x93" "A", "Title Z\xE2\x80\x93" "A",
         "title-desc"},
        {"Workflow status", "Status", "status"},
        {"Assignee A\xE2\x80\x93Z", "Assignee", "assignee"},
        {"Department A\xE2\x80\x93Z", "Department", "department"},
        {"Repository A\xE2\x80\x93Z", "Repository", "repository"},
    };
    for (const TaskSortChoice &choice : sortChoices) {
        auto *action = sortMenu->addAction(QString::fromUtf8(choice.menuLabel));
        action->setCheckable(true);
        action->setData(QString::fromLatin1(choice.key));
        action->setProperty("buttonLabel",
                            QString::fromUtf8(choice.buttonLabel));
        sortGroup->addAction(action);
        if (m_organizationTaskSortKey == QLatin1String(choice.key))
            action->setChecked(true);
    }
    connect(sortMenu, &QMenu::triggered, this, [this](QAction *action) {
        const QString key = action ? action->data().toString() : QString();
        if (key.isEmpty())
            return;
        m_organizationTaskSortKey = key;
        m_organizationTaskShownRows = 0;
        const QString label = action->property("buttonLabel").toString();
        m_organizationTaskSortButton->setText(label);
        m_organizationTaskSortButton->setToolTip(
            QStringLiteral("Sort tasks (currently %1)")
                .arg(action->text().toLower()));
        renderOrganizationTaskRows();
    });
    m_organizationTaskSortButton->setMenu(sortMenu);
    headingRow->addWidget(m_organizationTaskSortButton);

    m_organizationTaskMineFilterButton =
        new VerticalIconButton(QString(), VerticalIconButton::Tab);
    m_organizationTaskMineFilterButton->setObjectName(
        QStringLiteral("organizationTaskMineFilterAction"));
    m_organizationTaskMineFilterButton->setAccessibleName(
        QStringLiteral("Mine"));
    m_organizationTaskMineFilterButton->setCheckable(true);
    m_organizationTaskMineFilterButton->setEnabled(false);
    m_organizationTaskMineFilterButton->setToolTip(
        QStringLiteral("Show only tasks assigned to me"));
    setOcticon(m_organizationTaskMineFilterButton, "person", 16);
    connect(m_organizationTaskMineFilterButton, &QPushButton::toggled, this,
            [this](bool checked) {
                if (checked)
                    setOrganizationTaskAssigneeFilter(QStringLiteral("all"));
                m_organizationTaskShownRows = 0;
                renderOrganizationTaskRows();
            });
    headingRow->addWidget(m_organizationTaskMineFilterButton);


    auto *statusGroup = new QButtonGroup(page);
    statusGroup->setExclusive(true);
    auto makeStatusTab = [statusGroup, headingRow](const QString &label,
                                                   const QString &icon,
                                                   int id, bool checked) {
        auto *button = new VerticalIconButton(QString(), VerticalIconButton::Tab);
        button->setObjectName(QStringLiteral("organizationTaskStatusTab"));
        button->setAccessibleName(label);
        button->setToolTip(QStringLiteral("%1 tasks").arg(label));
        button->setCheckable(true);
        button->setChecked(checked);
        setOcticon(button, icon, 16);
        statusGroup->addButton(button, id);
        headingRow->addWidget(button);
        return button;
    };
    m_organizationTaskOpenFilterButton = makeStatusTab(
        QStringLiteral("Open"), QStringLiteral("issue-opened"), 0, true);
    m_organizationTaskClosedFilterButton = makeStatusTab(
        QStringLiteral("Closed"), QStringLiteral("check-circle"), 1, false);
    m_organizationTaskAllFilterButton = makeStatusTab(
        QStringLiteral("All"), QStringLiteral("list-unordered"), 2, false);
    connect(statusGroup, &QButtonGroup::idClicked, this, [this](int id) {
        m_organizationTaskStatusFilter =
            id == 0 ? QStringLiteral("open")
                    : id == 1 ? QStringLiteral("closed")
                              : QStringLiteral("all");
        m_organizationTaskShownRows = 0;
        renderOrganizationTaskRows();
    });

    m_organizationTasksStatus = new QLabel;
    m_organizationTasksStatus->setObjectName(
        QStringLiteral("organizationTasksStatus"));
    m_organizationTasksStatus->setWordWrap(true);
    m_organizationTasksStatus->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_organizationTasksStatus->setSizePolicy(QSizePolicy::Ignored,
                                             QSizePolicy::Preferred);
    m_organizationTasksSummary = new QLabel;
    m_organizationTasksSummary->setObjectName(QStringLiteral("mutedLabel"));
    headingRow->addSpacing(12);
    headingRow->addWidget(m_organizationTasksStatus, 1);
    headingRow->addSpacing(8);
    headingRow->addWidget(m_organizationTasksSummary);
    outer->addLayout(headingRow);

    auto *peopleScroll = new QScrollArea;
    peopleScroll->setObjectName(QStringLiteral("organizationTaskPeopleScroll"));
    peopleScroll->setFrameShape(QFrame::NoFrame);
    peopleScroll->setWidgetResizable(true);
    peopleScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    peopleScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    peopleScroll->setFixedHeight(40);
    m_organizationTaskPeopleStrip = new QWidget;
    m_organizationTaskPeopleStrip->setObjectName(
        QStringLiteral("organizationTaskPeople"));
    m_organizationTaskPeopleStrip->setAccessibleName(
        QStringLiteral("Show the tasks of an organization member"));
    m_organizationTaskPeopleStrip->setStyleSheet(QStringLiteral(
        "QToolButton#organizationTaskPerson { border: 1px solid transparent;"
        " border-radius: 15px; padding: 2px 9px 2px 2px; font-size: 11px; }"
        "QToolButton#organizationTaskPerson:hover {"
        " background: rgba(127, 127, 127, 40); }"
        "QToolButton#organizationTaskPerson:checked {"
        " border-color: #2f81f7; background: rgba(47, 129, 247, 38); }"));
    m_organizationTaskPeopleLayout =
        new QHBoxLayout(m_organizationTaskPeopleStrip);
    m_organizationTaskPeopleLayout->setContentsMargins(0, 2, 0, 2);
    m_organizationTaskPeopleLayout->setSpacing(4);
    m_organizationTaskPeopleLayout->addStretch(1);
    m_organizationTaskPeopleGroup = new QButtonGroup(m_organizationTaskPeopleStrip);
    m_organizationTaskPeopleGroup->setExclusive(false);
    connect(m_organizationTaskPeopleGroup, &QButtonGroup::buttonClicked, this,
            [this](QAbstractButton *button) {
                const QString filter =
                    button->property("assigneeFilter").toString();
                setOrganizationTaskAssigneeFilter(
                    filter == m_organizationTaskAssigneeFilter
                        ? QStringLiteral("all")
                        : filter);
                if (m_organizationTaskAssigneeFilter != QLatin1String("all") &&
                    m_organizationTaskMineFilterButton &&
                    m_organizationTaskMineFilterButton->isChecked()) {
                    const QSignalBlocker blocker(
                        m_organizationTaskMineFilterButton);
                    m_organizationTaskMineFilterButton->setChecked(false);
                }
                m_organizationTaskShownRows = 0;
                renderOrganizationTaskRows();
            });
    peopleScroll->setWidget(m_organizationTaskPeopleStrip);
    outer->addWidget(peopleScroll);
    rebuildOrganizationTaskPeopleStrip();

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    m_organizationTasksTable = new QTableWidget(0, 1);
    m_organizationTasksTable->setObjectName(
        QStringLiteral("organizationTasksTable"));
    m_organizationTasksTable->horizontalHeader()->hide();
    m_organizationTasksTable->verticalHeader()->hide();
    m_organizationTasksTable->setShowGrid(false);
    m_organizationTasksTable->setFrameShape(QFrame::NoFrame);
    m_organizationTasksTable->setSelectionBehavior(
        QAbstractItemView::SelectRows);
    m_organizationTasksTable->setSelectionMode(
        QAbstractItemView::SingleSelection);
    m_organizationTasksTable->setEditTriggers(
        QAbstractItemView::NoEditTriggers);
    m_organizationTasksTable->setAlternatingRowColors(false);
    m_organizationTasksTable->setSortingEnabled(false);
    m_organizationTasksTable->setWordWrap(false);
    m_organizationTasksTable->setMouseTracking(true);
    m_organizationTasksTable->setItemDelegate(
        new OrganizationTaskRowDelegate(m_organizationTasksTable));
    m_organizationTasksTable->verticalHeader()->setDefaultSectionSize(
        kTaskRowHeight);
    m_organizationTasksTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    connect(m_organizationTasksTable, &QTableWidget::itemSelectionChanged,
            this, [this] {
                renderOrganizationTaskDetail();
                updateOrganizationTaskActions();
            });
    connect(m_organizationTasksTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *) { editOrganizationTask(); });
    setOrganizationTaskListView(forkmesh::ui::loadListViewMode(
        QStringLiteral("tasks/listView"), forkmesh::ui::ListViewMode::Full));

    auto *tableHost = new QWidget;
    auto *tableLayout = new QVBoxLayout(tableHost);
    tableLayout->setContentsMargins(0, 0, 0, 0);
    tableLayout->setSpacing(6);
    tableLayout->addWidget(m_organizationTasksTable, 1);

    m_organizationTaskPageLabel = new QLabel;
    m_organizationTaskPageLabel->setObjectName(QStringLiteral("mutedLabel"));
    tableLayout->addWidget(m_organizationTaskPageLabel);
    connect(m_organizationTasksTable->verticalScrollBar(),
            &QScrollBar::valueChanged, this, [this] {
                const int lastVisible = m_organizationTasksTable->rowAt(
                    m_organizationTasksTable->viewport()->height() - 1);
                if (lastVisible < 0 ||
                    lastVisible + kOrganizationTaskLoadAheadRows >=
                        m_organizationTasksTable->rowCount())
                    appendOrganizationTaskRows(kOrganizationTaskPageSize);
            });
    splitter->addWidget(tableHost);

    auto *detailHost = new QWidget;
    auto *detailLayout = new QVBoxLayout(detailHost);
    detailLayout->setContentsMargins(12, 0, 0, 0);
    detailLayout->setSpacing(8);
    m_organizationTaskDetail = new QTextBrowser;
    m_organizationTaskDetail->setObjectName(
        QStringLiteral("organizationTaskDetail"));
    m_organizationTaskDetail->setOpenExternalLinks(false);
    m_organizationTaskDetail->setOpenLinks(false);
    m_organizationTaskFields = new QTextBrowser;
    m_organizationTaskFields->setObjectName(
        QStringLiteral("organizationTaskDetail"));
    m_organizationTaskFields->setOpenExternalLinks(false);
    m_organizationTaskFields->setOpenLinks(false);
    m_organizationTaskFields->setWordWrapMode(
        QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_organizationTaskFields->setFixedWidth(300);
    const auto openDetailLink = [this](const QUrl &url) {
        const QString href = url.toString();
        if (forkmesh::taskmarkdown::isSafeLink(href))
            QDesktopServices::openUrl(url);
        else if (href.startsWith(kAgentLinkScheme))
            switchToAgentsTab(
                href.mid(kAgentLinkScheme.size()).toInt());
        else if (href.startsWith(QStringLiteral("forkmesh-task-image:"))) {
            const QString key = QUrl::fromPercentEncoding(
                href.mid(QStringLiteral("forkmesh-task-image:").size())
                    .toLatin1());
            const QImage image = QImage::fromData(
                m_organizationTaskImageData.value(key));
            if (image.isNull())
                return;
            auto *dialog = new QDialog(this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(QStringLiteral("Task image"));
            QSize maximum(1200, 800);
            if (QScreen *screen = QGuiApplication::primaryScreen()) {
                const QSize available = screen->availableSize();
                maximum = QSize(available.width() * 9 / 10,
                                available.height() * 9 / 10);
            }
            QPixmap shown = QPixmap::fromImage(image);
            if (shown.width() > maximum.width() ||
                shown.height() > maximum.height()) {
                shown = shown.scaled(maximum, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }
            auto *label = new QLabel;
            label->setAlignment(Qt::AlignCenter);
            label->setPixmap(shown);
            auto *scroll = new QScrollArea;
            scroll->setWidgetResizable(true);
            scroll->setAlignment(Qt::AlignCenter);
            scroll->setWidget(label);
            auto *close = new QPushButton(QStringLiteral("Close"));
            connect(close, &QPushButton::clicked, dialog,
                    &QDialog::accept);
            auto *buttons = new QHBoxLayout;
            buttons->addStretch();
            buttons->addWidget(close);
            auto *layout = new QVBoxLayout(dialog);
            layout->setContentsMargins(12, 12, 12, 12);
            layout->addWidget(scroll, 1);
            layout->addLayout(buttons);
            dialog->resize(qMin(shown.width() + 48, maximum.width()),
                           qMin(shown.height() + 96, maximum.height()));
            dialog->show();
        }
    };
    connect(m_organizationTaskDetail, &QTextBrowser::anchorClicked, this,
            openDetailLink);
    new TaskCheckboxClickFilter(
        m_organizationTaskDetail,
        [this](int index) { return organizationTaskCheckboxToggleable(index); },
        [this](int index) { toggleOrganizationTaskCheckbox(index); });
    connect(m_organizationTaskFields, &QTextBrowser::anchorClicked, this,
            openDetailLink);

    auto *actionHost = new QWidget;
    auto *actions = new FlowLayout(actionHost, 0, 2, 4);
    auto addAction = [this, actions](QPushButton **slot, const QString &text,
                                     const QString &icon) {
        *slot = new VerticalIconButton(text, VerticalIconButton::Action);
        (*slot)->setObjectName(QStringLiteral("organizationTaskAction"));
        setOcticon(*slot, icon, 16);
        actions->addWidget(*slot);
    };
    addAction(&m_organizationTaskEditButton, QStringLiteral("Edit"),
              QStringLiteral("pencil"));
    addAction(&m_organizationTaskStartButton, QStringLiteral("Start"),
              QStringLiteral("play"));
    addAction(&m_organizationTaskCompleteButton, QStringLiteral("Complete"),
              QStringLiteral("check-circle"));
    addAction(&m_organizationTaskUndoButton, QStringLiteral("Mark undone"),
              QStringLiteral("history"));
    addAction(&m_organizationTaskAgentButton, QStringLiteral("Assign Bot"),
              QStringLiteral("sparkle"));
    addAction(&m_organizationTaskStartAgentButton,
              QStringLiteral("Start agent"), QStringLiteral("rocket"));
    static_cast<VerticalIconButton *>(m_organizationTaskStartAgentButton)
        ->setAccentColor(Theme::nextOr(
            Theme::Role::AccentFg,
            QColor(currentThemeIsDark() ? "#3fb950" : "#1a7f37")));
    m_organizationTaskStartAgentButton->setToolTip(
        QStringLiteral("Start agent with prompt settings: attach this task to "
                       "the prompt box and start an agent on it with the "
                       "repository, provider, model and mode the prompt box "
                       "is set to"));
    addAction(&m_organizationTaskPromptButton,
              QStringLiteral("Add to prompt"), QStringLiteral("plus"));
    m_organizationTaskPromptButton->setToolTip(
        QStringLiteral("Add this task's title and details to the prompt box"));
    addAction(&m_organizationTaskQaButton, QStringLiteral("Send to QA"),
              QStringLiteral("shield-check"));
    addAction(&m_organizationTaskReturnButton,
              QStringLiteral("Return to list"), QStringLiteral("arrow-left"));
    addAction(&m_organizationTaskFollowUpButton, QStringLiteral("Follow up"),
              QStringLiteral("git-branch"));
    addAction(&m_organizationTaskIssueButton,
              QStringLiteral("Move to issue"), QStringLiteral("issue-opened"));
    m_organizationTaskIssueButton->setToolTip(
        QStringLiteral("Create a repository issue from this task and remove "
                       "the task after the issue is filed"));
    addAction(&m_organizationTaskCommentButton, QStringLiteral("Comment"),
              QStringLiteral("comment"));
    m_organizationTaskCommentButton->setToolTip(
        QStringLiteral("Comment on this task. Everyone it routes through is "
                       "pinged, and so is every @name the comment uses"));
    addAction(&m_organizationTaskCheckinButton, QStringLiteral("Check in"),
              QStringLiteral("broadcast"));
    m_organizationTaskCheckinButton->setToolTip(
        QStringLiteral("Tell your team how this task is going. Sends a ping"));
    addAction(&m_organizationTaskRemindButton, QStringLiteral("Remind"),
              QStringLiteral("bell"));
    m_organizationTaskRemindButton->setToolTip(
        QStringLiteral("Ping the assignee to check in on this task"));
    addAction(&m_organizationTaskAttachWorktreeButton,
              QStringLiteral("Attach worktree"),
              QStringLiteral("file-directory"));
    m_organizationTaskAttachWorktreeButton->setToolTip(
        QStringLiteral("Attach a branch or worktree on this desktop to the "
                       "task, so deleting the task cleans it up"));
    addAction(&m_organizationTaskDeleteButton, QStringLiteral("Delete"),
              QStringLiteral("trash"));
    m_organizationTaskDeleteButton->setToolTip(
        QStringLiteral("Move this task to the trash (restorable for 30 "
                       "days on the web Tasks page)"));
    static_cast<VerticalIconButton *>(m_organizationTaskDeleteButton)
        ->setAccentColor(QColor(currentThemeIsDark() ? "#f85149" : "#cf222e"));
    connect(m_organizationTaskEditButton, &QPushButton::clicked, this,
            &MainWindow::editOrganizationTask);
    connect(m_organizationTaskAgentButton, &QPushButton::clicked, this,
            [this] {
                const QJsonObject task = selectedOrganizationTask(
                    m_organizationTasksTable, m_organizationTasks);
                const int sessionId = localAgentSessionIdForTask(
                    taskText(task, QStringLiteral("id")));
                if (sessionId > 0)
                    switchToAgentsTab(sessionId);
                else
                    assignOrganizationTaskToAgent();
            });
    connect(m_organizationTaskStartButton, &QPushButton::clicked, this,
            [this] {
                const QJsonObject task = selectedOrganizationTask(
                    m_organizationTasksTable, m_organizationTasks);
                runOrganizationTaskAction(
                    taskText(task, QStringLiteral("status")) ==
                            QLatin1String("active")
                        ? QStringLiteral("stop")
                        : QStringLiteral("start"));
            });
    connect(m_organizationTaskCompleteButton, &QPushButton::clicked, this,
            [this] { runOrganizationTaskAction(QStringLiteral("complete")); });
    connect(m_organizationTaskQaButton, &QPushButton::clicked, this,
            [this] { runOrganizationTaskAction(QStringLiteral("qa")); });
    connect(m_organizationTaskReturnButton, &QPushButton::clicked, this,
            [this] { runOrganizationTaskAction(QStringLiteral("return")); });
    connect(m_organizationTaskUndoButton, &QPushButton::clicked, this,
            [this] { runOrganizationTaskAction(QStringLiteral("undo")); });
    connect(m_organizationTaskDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteOrganizationTask);
    connect(m_organizationTaskAttachWorktreeButton, &QPushButton::clicked,
            this, &MainWindow::attachExistingWorktreeToOrganizationTask);
    connect(m_organizationTaskFollowUpButton, &QPushButton::clicked, this,
            &MainWindow::createOrganizationTaskFollowUp);
    connect(m_organizationTaskIssueButton, &QPushButton::clicked, this,
            &MainWindow::moveOrganizationTaskToIssue);
    connect(m_organizationTaskPromptButton, &QPushButton::clicked, this,
            &MainWindow::addOrganizationTaskToPrompt);
    connect(m_organizationTaskCommentButton, &QPushButton::clicked, this,
            &MainWindow::commentOnOrganizationTask);
    connect(m_organizationTaskCheckinButton, &QPushButton::clicked, this,
            &MainWindow::checkInOnOrganizationTask);
    connect(m_organizationTaskRemindButton, &QPushButton::clicked, this,
            &MainWindow::remindOrganizationTaskAssignee);
    connect(m_organizationTaskStartAgentButton, &QPushButton::clicked, this,
            &MainWindow::startOrganizationTaskAgentFromPrompt);
    detailLayout->addWidget(actionHost);
    auto *detailBody = new QHBoxLayout;
    detailBody->setContentsMargins(0, 0, 0, 0);
    detailBody->setSpacing(12);
    detailBody->addWidget(m_organizationTaskDetail, 1);
    detailBody->addWidget(m_organizationTaskFields);
    detailLayout->addLayout(detailBody, 1);
    splitter->addWidget(detailHost);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    // The startup pull answered before this page existed: render its answer
    // rather than asking the relay for the same list a second time.
    if (!m_organizationTasksStartupPayload.isEmpty()) {
        const QJsonObject startup = m_organizationTasksStartupPayload;
        const qint64 fetchedAt = m_organizationTasksLoadedAt;
        m_organizationTasksStartupPayload = QJsonObject();
        applyOrganizationTasks(startup);
        m_organizationTasksLoadedAt = fetchedAt;
    }
    updateOrganizationTaskRefreshAge();

    renderOrganizationTaskDetail();
    updateOrganizationTaskActions();
    return page;
}

void MainWindow::requestOrganizationTasks(
    const QByteArray &method, const QString &path, const QJsonObject &body,
    OrganizationTaskReplyHandler handler, const QList<int> &expectedHttpStatuses)
{
    if (!m_networkAccess) {
        handler(false, {}, QStringLiteral(
            "Sign in to an organization account to use private tasks."));
        return;
    }
    QUrl url = catalogApiUrl();
    const QUrl relative(path);
    url.setPath(relative.path());
    url.setQuery(relative.query());
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    for (const int expected : expectedHttpStatuses)
        forkmesh::markExpectedHttpStatus(request, expected);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (const auto *network =
            qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess))
        request.setHeader(
            QNetworkRequest::UserAgentHeader,
            QStringLiteral("ForkMeshDesktop/%1")
                .arg(network->clientVersion().isEmpty()
                         ? QStringLiteral("0")
                         : network->clientVersion()));
    bool keySigned = false;
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (!sessionToken.isEmpty()) {
        request.setRawHeader(
            QByteArrayLiteral("Authorization"),
            QByteArrayLiteral("Bearer ") + sessionToken.toUtf8());
    } else {
        // Key-only sessions allow the signed subset; other writes need a token.
        QString resource;
        const QString proof = organizationTaskProof(
            method, relative.path(), body, &resource);
        if (proof.isEmpty() ||
            !authenticateOrgTaskRequest(url, request, proof, resource)) {
            handler(false, {},
                    m_accountAuthenticated
                        ? QString::fromUtf8(
                              "This change needs a password sign-in on this "
                              "device, not just its saved account key \xE2"
                              "\x80\x94 use Settings \xE2\x86\x92 \"Log in to "
                              "a user account\", then try again.")
                        : QStringLiteral(
                              "Sign in to an organization account to use "
                              "private tasks."));
            return;
        }
        keySigned = true;
    }
    const QByteArray payload =
        body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(
                                             QJsonDocument::Compact);
    if (!payload.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));

    QNetworkReply *reply = nullptr;
    if (method == QByteArrayLiteral("GET"))
        reply = m_networkAccess->get(request);
    else if (method == QByteArrayLiteral("POST"))
        reply = m_networkAccess->post(request, payload);
    else
        reply = m_networkAccess->sendCustomRequest(request, method, payload);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, method, path, keySigned, expectedHttpStatuses,
             handler = std::move(handler)]() mutable {
                const QByteArray raw = reply->readAll();
                const QJsonDocument document = QJsonDocument::fromJson(raw);
                QJsonObject data = document.object();
                const int status = reply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                status >= 200 && status < 300 &&
                                data.value(QStringLiteral("ok")).toBool(true);
                if (!ok && (BackoffNetworkAccessManager::isBackoffSuppressed(reply)
                            || status == 429)) {
                    const auto *network =
                        qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
                    data.insert(kOrganizationTaskBackoffKey,
                                double(network ? network->hostCooldownRemainingMs(
                                                     reply->url().host())
                                               : 0));
                }
                QString error = ok
                    ? QString()
                    : taskErrorText(
                          data,
                          status > 0
                              ? QStringLiteral("HTTP %1").arg(status)
                              : reply->errorString());
                if (keySigned && status == 401)
                    error += QString::fromUtf8(
                        " \xE2\x80\x94 this machine's key is not authorized "
                        "for the account. Use Settings \xE2\x86\x92 \"Log in "
                        "to a user account\" to register it.");
                if (!ok && !expectedHttpStatuses.contains(status)) {
                    const QString context = taskRequestContextText(data);
                    logSystem(
                        QStringLiteral("Organization tasks: %1 %2 failed: %3%4.")
                            .arg(QString::fromLatin1(method), path, error,
                                 context.isEmpty()
                                     ? QString()
                                     : QStringLiteral(" [%1]").arg(context)));
                }
                reply->deleteLater();
                handler(ok, data, error);
            });
}

void MainWindow::requestOrganizationTaskAttachment(
    const QString &taskId, const QString &attachmentId,
    OrganizationTaskAttachmentHandler handler)
{
    if (!m_networkAccess || taskId.isEmpty() || attachmentId.isEmpty()) {
        handler(false, {}, {}, QStringLiteral("Task image unavailable"));
        return;
    }
    const QString path = QStringLiteral("/api/tasks/%1/attachments/%2")
                             .arg(taskId, attachmentId);
    QUrl url = catalogApiUrl();
    const QUrl relative(path);
    url.setPath(relative.path());
    url.setQuery(relative.query());
    QNetworkRequest request;
    if (!authenticateOrgTaskRequest(
            url, request, kOrgTaskAttachmentProof,
            taskId + QLatin1Char('\n') + attachmentId)) {
        handler(false, {}, {}, QStringLiteral("Task image authentication failed"));
        return;
    }
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("image/*"));
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_networkAccess->get(request);
    if (!reply) {
        handler(false, {}, {}, QStringLiteral("Task image request failed"));
        return;
    }
    connect(reply, &QNetworkReply::finished, this,
            [reply, handler = std::move(handler)]() mutable {
                const QByteArray bytes = reply->readAll();
                const int status = reply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                status >= 200 && status < 300 &&
                                !bytes.isEmpty();
                const QString mime = QString::fromLatin1(
                    reply->header(QNetworkRequest::ContentTypeHeader)
                        .toByteArray())
                    .section(QLatin1Char(';'), 0, 0)
                    .trimmed()
                    .toLower();
                const QString error = ok
                    ? QString()
                    : QStringLiteral("Task image request failed (%1)")
                          .arg(status > 0 ? QString::number(status)
                                          : reply->errorString());
                reply->deleteLater();
                handler(ok, bytes, mime, error);
            });
}

void MainWindow::requestOrganizationTaskCatalog(
    OrganizationTaskReplyHandler handler)
{
    requestOrganizationTaskCatalogPage(
        0, QJsonArray(), QJsonObject(), std::move(handler));
}

void MainWindow::requestOrganizationTaskCatalogPage(
    int offset, QJsonArray catalog, QJsonObject firstPage,
    OrganizationTaskReplyHandler handler)
{
    constexpr int kTaskCatalogLimit = 2000;
    const QString path = offset > 0
        ? QStringLiteral("/api/tasks?offset=%1").arg(offset)
        : QStringLiteral("/api/tasks");
    requestOrganizationTasks(
        QByteArrayLiteral("GET"), path, {},
        [this, offset, catalog = std::move(catalog),
         firstPage = std::move(firstPage),
         handler = std::move(handler)](
            bool ok, const QJsonObject &payload,
            const QString &error) mutable {
            if (!ok) {
                handler(false, {}, error);
                return;
            }
            if (offset == 0)
                firstPage = payload;
            const QJsonArray page =
                payload.value(QStringLiteral("tasks")).toArray();
            for (const QJsonValue &task : page) {
                if (catalog.size() >= kTaskCatalogLimit)
                    break;
                catalog.append(task);
            }
            const int next =
                payload.value(QStringLiteral("nextOffset")).toInt(-1);
            if (payload.value(QStringLiteral("hasMore")).toBool() &&
                next > offset && catalog.size() < kTaskCatalogLimit) {
                requestOrganizationTaskCatalogPage(
                    next, std::move(catalog), std::move(firstPage),
                    std::move(handler));
                return;
            }
            const bool complete =
                !payload.value(QStringLiteral("hasMore")).toBool();
            firstPage.insert(QStringLiteral("tasks"), catalog);
            reconcileOrganizationTaskWorkItems(catalog, complete);
            handler(true, firstPage, QString());
        });
}

void MainWindow::refreshOrganizationTasks()
{
    if (m_organizationTasksLoading || !m_organizationTasksStatus)
        return;
    m_organizationTasksLoading = true;
    // A refresh is the one moment the operator has asked for current truth, so
    // cached timelines are dropped with it. A comment by someone else never
    // moves the task row's updatedAt, so no field on the catalog could stand
    // in for this.
    m_organizationTaskHistory.clear();
    m_organizationTaskHistoryAt.clear();
    updateOrganizationTaskActions();
    updateOrganizationTaskRefreshAge();
    requestOrganizationTaskCatalog(
        [this](bool ok, const QJsonObject &payload, const QString &error) {
            m_organizationTasksLoading = false;
            const auto pushed = qScopeGuard(
                [this] { drainPendingOrganizationTaskEvent(); });
            if (!ok) {
                m_organizationTasksStatus->setText(
                    QStringLiteral("Tasks unavailable: %1").arg(error));
                updateOrganizationTaskRefreshAge();
                updateOrganizationTaskActions();
                return;
            }
            m_organizationTasksStatus->clear();
            applyOrganizationTasks(payload);
            nativePackageSearchIndexChanged();
        });
}

void MainWindow::updateOrganizationTaskRefreshAge()
{
    if (!m_organizationTaskRefreshButton)
        return;
    if (m_organizationTasksLoading) {
        m_organizationTaskRefreshButton->setText(
            QString::fromUtf8("Loading\xE2\x80\xA6"));
        return;
    }
    if (m_organizationTasksLoadedAt <= 0) {
        m_organizationTaskRefreshButton->setText(QStringLiteral("Refresh"));
        m_organizationTaskRefreshButton->setToolTip(
            QStringLiteral("Refresh the private task catalog"));
        return;
    }
    const qint64 ageSecs = (QDateTime::currentMSecsSinceEpoch() -
                            m_organizationTasksLoadedAt) / 1000;
    m_organizationTaskRefreshButton->setText(
        ageSecs < 60
            ? QStringLiteral("just now")
            : formatShortRelativeTime(m_organizationTasksLoadedAt / 1000) +
                  QStringLiteral(" ago"));
    m_organizationTaskRefreshButton->setToolTip(
        QStringLiteral("Refreshed at %1. Click to refresh the private task "
                       "catalog")
            .arg(QDateTime::fromMSecsSinceEpoch(m_organizationTasksLoadedAt)
                     .toLocalTime()
                     .toString(QStringLiteral("HH:mm:ss"))));
}

void MainWindow::syncOrganizationTaskPageSearch()
{
    const bool onTasks =
        m_sectionStack &&
        m_sectionStack->currentIndex() == kOrganizationTasksSectionIndex;
    const QString query = onTasks && m_globalSearch
                              ? m_globalSearch->text().trimmed()
                              : QString();
    if (onTasks)
        updateOrganizationTaskRefreshAge();
    if (m_organizationTasksSearchMirror == query)
        return;
    m_organizationTasksSearchMirror = query;
    m_organizationTaskShownRows = 0;
    renderOrganizationTaskRows();
}

void MainWindow::setOrganizationTaskBadge(int openCount)
{
    if (m_organizationTaskBadgeStored != openCount) {
        forkmesh::settings::put(QSettings(), kOrganizationTaskOpenCountSetting,
                                openCount);
        m_organizationTaskBadgeStored = openCount;
    }
    if (auto *button = dynamic_cast<ActivityRailButton *>(m_tasksNavButton))
        button->setBadgeCount(openCount);
}

void MainWindow::restoreOrganizationTaskBadge()
{
    if (auto *button = dynamic_cast<ActivityRailButton *>(m_tasksNavButton)) {
        m_organizationTaskBadgeStored =
            QSettings().value(kOrganizationTaskOpenCountSetting, 0).toInt();
        button->setBadgeCount(m_organizationTaskBadgeStored);
    }
}

// The one catalog refresh a launch makes on its own. Large catalogs continue
// through bounded pages, but everything after that refresh is explicitly
// asked for: opening the page, Refresh, or the user's own edit.
void MainWindow::refreshOrganizationTaskBadge()
{
    if (m_organizationTasksLoading || m_organizationTasksLoaded)
        return; // the page already pulled the catalog this launch
    if (m_organizationTasksStatus && m_organizationTasksTable) {
        refreshOrganizationTasks(); // page exists: keep table and badge in step
        return;
    }
    m_organizationTasksLoading = true;
    requestOrganizationTaskCatalog(
        [this](bool ok, const QJsonObject &payload, const QString &) {
            m_organizationTasksLoading = false;
            const auto pushed = qScopeGuard(
                [this] { drainPendingOrganizationTaskEvent(); });
            if (!ok)
                return;
            linkExistingAgentsToOrganizationTasks(
                payload.value(QStringLiteral("tasks")).toArray());
            // Keep the whole payload, not just the open count: the Tasks page
            // is built on first open and renders this instead of pulling
            // again. Opening the page while this read was in flight builds it
            // early, so hand the answer straight to the table in that case.
            if (m_organizationTasksTable) {
                applyOrganizationTasks(payload);
                return;
            }
            m_organizationTasksStartupPayload = payload;
            m_organizationTasksLoadedAt = QDateTime::currentMSecsSinceEpoch();
            setOrganizationTaskBadge(openOrganizationTaskCount(
                payload.value(QStringLiteral("tasks")).toArray()));
        });
}

// The relay pushed the payload-free "tasks" topic: a member of this
// organization created, started, closed or deleted a task - possibly this
// account on another device. The launch pull is deliberately the only
// unprompted read, so this event, not a timer, is what keeps the table and
// the rail badge current for the rest of the session.
void MainWindow::onOrganizationTaskEvent()
{
    if (m_organizationTasksLoading) {
        // A read issued before the write landed can answer without the row
        // this event is about, so remember the wake-up and re-read when that
        // one returns rather than dropping it and going stale until Refresh.
        m_organizationTaskEventPending = true;
        return;
    }
    m_organizationTaskEventPending = false;
    if (m_organizationTasksStatus && m_organizationTasksTable) {
        refreshOrganizationTasks(); // page is built: table and badge together
        return;
    }
    // The page has not been built yet. Clear the launch-pull latch so the
    // badge read runs again and replaces the payload that page will render
    // from when it is first opened.
    m_organizationTasksLoaded = false;
    refreshOrganizationTaskBadge();
}

void MainWindow::drainPendingOrganizationTaskEvent()
{
    if (!m_organizationTaskEventPending || m_organizationTasksLoading)
        return;
    onOrganizationTaskEvent();
}

void MainWindow::applyOrganizationTasks(const QJsonObject &payload)
{
    if (!m_organizationTasksTable)
        return;
    const QJsonObject prior = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString selectedId = taskText(prior, QStringLiteral("id"));
    m_organizationTasksLoaded = true;
    m_organizationTasksLoadedAt = QDateTime::currentMSecsSinceEpoch();
    m_organizationTasks =
        payload.value(QStringLiteral("tasks")).toArray();
    overlayOrganizationTaskLocalEdits();
    linkExistingAgentsToOrganizationTasks(m_organizationTasks);
    refreshBranchLinks();
    m_organizationTaskActor =
        payload.value(QStringLiteral("actor")).toString().toLower();
    m_organizationTasksCanManage =
        payload.value(QStringLiteral("canManage")).toBool();
    m_organizationTaskMembers.clear();
    for (const QJsonValue &value :
         payload.value(QStringLiteral("members")).toArray()) {
        const QString member = value.toString().trimmed().toLower();
        if (!member.isEmpty())
            m_organizationTaskMembers.append(member);
    }
    const QString selectedMember =
        m_organizationTaskAssigneeFilter.startsWith(QLatin1String("user:"))
            ? m_organizationTaskAssigneeFilter.mid(5)
            : QString();
    if (!selectedMember.isEmpty() &&
        !m_organizationTaskMembers.contains(selectedMember))
        m_organizationTaskAssigneeFilter = QStringLiteral("all");
    rebuildOrganizationTaskPeopleStrip();
    m_organizationTaskDepartments.clear();
    for (const QJsonValue &value :
         payload.value(QStringLiteral("departments")).toArray()) {
        const QString department = value.toString().trimmed().toLower();
        if (!department.isEmpty())
            m_organizationTaskDepartments.append(department);
    }

    QString requestedId = selectedId;
    QString missingLinkedTaskId;
    if (!m_pendingOrganizationTaskSelection.isEmpty()) {
        requestedId = m_pendingOrganizationTaskSelection;
        m_organizationTasksSearchMirror.clear();
        m_organizationTaskShownRows = 0;
        if (m_organizationTaskMineFilterButton) {
            const QSignalBlocker blocker(m_organizationTaskMineFilterButton);
            m_organizationTaskMineFilterButton->setChecked(false);
        }
        setOrganizationTaskAssigneeFilter(QStringLiteral("all"));
        for (int i = 0; i < m_organizationTasks.size(); ++i) {
            if (taskText(m_organizationTasks.at(i).toObject(),
                         QStringLiteral("id")) == requestedId) {
                revealOrganizationTaskStatus(
                    m_organizationTasks.at(i).toObject());
                m_pendingOrganizationTaskSelection.clear();
                break;
            }
        }
        if (!m_pendingOrganizationTaskSelection.isEmpty()) {
            missingLinkedTaskId = m_pendingOrganizationTaskSelection;
            m_pendingOrganizationTaskSelection.clear();
        }
    }
    renderOrganizationTaskRows(requestedId);
    const int openCount = openOrganizationTaskCount(m_organizationTasks);
    if (m_organizationTasksSummary) {
        m_organizationTasksSummary->setText(
            QString::fromUtf8("%1 open \xC2\xB7 %2 closed%3")
                .arg(openCount)
                .arg(m_organizationTasks.size() - openCount)
                .arg(m_organizationTasksCanManage
                         ? QString::fromUtf8(" \xC2\xB7 manager")
                         : QString()));
    }
    setOrganizationTaskBadge(openCount);
    updateOrganizationTaskRefreshAge();

    // The list is intentionally capped. A linked historical task may sit past
    // that cap, so resolve it by id instead of leaving the Tasks page open with
    // no selected row.
    if (!missingLinkedTaskId.isEmpty()) {
        m_organizationTasksLoading = true;
        requestOrganizationTasks(
            QByteArrayLiteral("GET"),
            QStringLiteral("/api/tasks/") + missingLinkedTaskId, {},
            [this, missingLinkedTaskId](bool ok, const QJsonObject &payload,
                                        const QString &error) {
                m_organizationTasksLoading = false;
                const auto pushed = qScopeGuard(
                    [this] { drainPendingOrganizationTaskEvent(); });
                const QJsonObject task =
                    payload.value(QStringLiteral("task")).toObject();
                if (!ok || task.isEmpty()) {
                    if (m_organizationTasksStatus)
                        m_organizationTasksStatus->setText(
                            QStringLiteral("Linked task unavailable: %1")
                                .arg(error));
                    return;
                }
                m_organizationTasks.append(task);
                m_organizationTaskShownRows = 0;
                renderOrganizationTaskRows(missingLinkedTaskId);
                if (m_organizationTasksStatus)
                    m_organizationTasksStatus->setText(
                        QStringLiteral("Opened linked organization task."));
            });
    }
}

bool MainWindow::replaceOrganizationTask(const QJsonObject &task)
{
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return false;
    for (int index = 0; index < m_organizationTasks.size(); ++index) {
        if (taskText(m_organizationTasks.at(index).toObject(),
                     QStringLiteral("id")) != id) {
            continue;
        }
        m_organizationTasks.replace(index, task);
        renderOrganizationTaskRows(id);
        return true;
    }
    return false;
}

void MainWindow::overlayOrganizationTaskLocalEdits()
{
    for (int index = 0; index < m_organizationTasks.size() &&
                        !m_organizationTaskLocalEdits.isEmpty();
         ++index) {
        const QJsonObject listed = m_organizationTasks.at(index).toObject();
        const QString id = taskText(listed, QStringLiteral("id"));
        auto local = m_organizationTaskLocalEdits.find(id);
        if (local == m_organizationTaskLocalEdits.end())
            continue;
        // Every task write bumps updatedAt, so once the catalog row is at
        // least as new as the saved edit this read already carries it.
        if (!m_organizationTaskEditsInFlight.contains(id) &&
            qint64(listed.value(QStringLiteral("updatedAt")).toDouble()) >=
                qint64(local->value(QStringLiteral("updatedAt")).toDouble())) {
            m_organizationTaskLocalEdits.erase(local);
            continue;
        }
        m_organizationTasks.replace(index, *local);
    }
}

void MainWindow::renderOrganizationTaskRows(const QString &selectTaskId)
{
    if (!m_organizationTasksTable)
        return;
    publishTaskRepositoryLogos(m_repositories);
    const QString selectedId =
        selectTaskId.isEmpty()
            ? taskText(selectedOrganizationTask(m_organizationTasksTable,
                                                m_organizationTasks),
                       QStringLiteral("id"))
            : selectTaskId;
    const QString query = m_organizationTasksSearchMirror;
    const bool mineOnly = m_organizationTaskMineFilterButton &&
                          m_organizationTaskMineFilterButton->isChecked();
    const QString assigneeFilter = m_organizationTaskAssigneeFilter;
    const auto assigneeKey = [](const QJsonObject &task) {
        const QString kind = taskText(task, QStringLiteral("assigneeKind"));
        if (kind == QLatin1String("agent") ||
            kind == QLatin1String("unassigned"))
            return kind;
        const QString name =
            taskText(task, QStringLiteral("assignee")).trimmed().toLower();
        return name.isEmpty() ? QStringLiteral("unassigned")
                              : QStringLiteral("user:") + name;
    };
    const auto matchesQuery = [&query](const QJsonObject &task) {
        if (query.isEmpty())
            return true;
        const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
        return QStringList{
            taskText(task, QStringLiteral("title")),
            taskText(task, QStringLiteral("details")),
            taskAssigneeLabel(task),
            taskText(task, QStringLiteral("repository")),
            taskText(task, QStringLiteral("department")),
            taskText(task, QStringLiteral("status")),
            taskText(qa, QStringLiteral("howToTest")),
            taskText(task, QStringLiteral("id")),
        }
            .join(QLatin1Char('\n'))
            .contains(query, Qt::CaseInsensitive);
    };
    int mineCount = 0;
    int openCount = 0;
    int closedCount = 0;
    QHash<QString, int> openByAssignee;
    QList<int> matches;
    matches.reserve(m_organizationTasks.size());
    for (int index = 0; index < m_organizationTasks.size(); ++index) {
        const QJsonObject task = m_organizationTasks.at(index).toObject();
        const bool closed = organizationTaskIsClosed(task);
        const QString key = assigneeKey(task);
        if (!closed) {
            ++openByAssignee[key];
            ++openByAssignee[QStringLiteral("all")];
        }
        if (!matchesQuery(task))
            continue;
        const bool statusMatches =
            !((m_organizationTaskStatusFilter == QLatin1String("open") &&
               closed) ||
              (m_organizationTaskStatusFilter == QLatin1String("closed") &&
               !closed));
        const bool mine =
            organizationTaskAssignedTo(task, m_organizationTaskActor);
        if (mine && statusMatches)
            ++mineCount;
        if (mineOnly && !mine)
            continue;
        if (assigneeFilter != QLatin1String("all") && key != assigneeFilter)
            continue;
        if (closed)
            ++closedCount;
        else
            ++openCount;
        if (statusMatches)
            matches.append(index);
    }
    if (m_organizationTaskMineFilterButton) {
        const bool haveActor = !m_organizationTaskActor.trimmed().isEmpty();
        m_organizationTaskMineFilterButton->setEnabled(haveActor);
        m_organizationTaskMineFilterButton->setToolTip(
            haveActor
                ? QStringLiteral("Mine: %1 tasks assigned to %2")
                      .arg(mineCount)
                      .arg(m_organizationTaskActor)
                : QStringLiteral("Sign in to filter tasks assigned to you"));
        m_organizationTaskMineFilterButton->setText(
            haveActor ? QString::number(mineCount) : QString());
    }
    const auto setTabCount = [](QPushButton *button, int count) {
        if (!button)
            return;
        button->setText(QString::number(count));
        button->setToolTip(QStringLiteral("%1: %2 tasks")
                               .arg(button->accessibleName())
                               .arg(count));
        button->updateGeometry();
    };
    setTabCount(m_organizationTaskOpenFilterButton, openCount);
    setTabCount(m_organizationTaskClosedFilterButton, closedCount);
    setTabCount(m_organizationTaskAllFilterButton, openCount + closedCount);
    updateOrganizationTaskPeopleStrip(openByAssignee);
    std::stable_sort(matches.begin(), matches.end(),
                     [this](int left, int right) {
                         return organizationTaskComesBefore(
                             m_organizationTasks.at(left).toObject(),
                             m_organizationTasks.at(right).toObject(),
                             m_organizationTaskSortKey);
                     });

    int selectedMatch = -1;
    if (!selectedId.isEmpty()) {
        for (int position = 0; position < matches.size(); ++position) {
            if (taskText(m_organizationTasks.at(matches.at(position)).toObject(),
                         QStringLiteral("id")) == selectedId) {
                selectedMatch = position;
                break;
            }
        }
    }
    const bool keepScroll = m_organizationTaskShownRows > 0;
    const int scrollValue =
        m_organizationTasksTable->verticalScrollBar()->value();
    int shown = keepScroll
                    ? qMax(kOrganizationTaskPageSize, m_organizationTaskShownRows)
                    : kOrganizationTaskPageSize;
    if (selectedMatch >= shown)
        shown = (selectedMatch / kOrganizationTaskPageSize + 1) *
                kOrganizationTaskPageSize;

    m_organizationTaskMatches.clear();
    m_organizationTaskShownRows = 0;
    m_organizationTasksTable->setRowCount(0);
    m_organizationTaskMatches = std::move(matches);
    appendOrganizationTaskRows(shown);
    updateOrganizationTaskPageLabel();
    if (keepScroll)
        m_organizationTasksTable->verticalScrollBar()->setValue(scrollValue);
    if (selectedMatch >= 0)
        m_organizationTasksTable->selectRow(selectedMatch);
    else if (m_organizationTasksTable->rowCount() > 0)
        m_organizationTasksTable->selectRow(0);
    renderOrganizationTaskDetail();
    updateOrganizationTaskActions();
}

void MainWindow::appendOrganizationTaskRows(int count)
{
    if (!m_organizationTasksTable)
        return;
    const int first = m_organizationTasksTable->rowCount();
    const int last = qMin(m_organizationTaskMatches.size(), first + count);
    if (last <= first)
        return;
    m_organizationTasksTable->setRowCount(last);
    for (int row = first; row < last; ++row) {
        const int index = m_organizationTaskMatches.at(row);
        if (index < 0 || index >= m_organizationTasks.size())
            continue;
        const QJsonObject task = m_organizationTasks.at(index).toObject();
        const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
        const QString statusKey = taskStatusKey(task);
        const QString department = taskText(task, QStringLiteral("department"));
        const QString repository = taskText(task, QStringLiteral("repository"));
        const QString qaStatus = taskText(qa, QStringLiteral("status"));
        const bool qaRequested =
            qa.value(QStringLiteral("requestedAt")).toDouble() > 0;

        auto *item =
            new QTableWidgetItem(taskText(task, QStringLiteral("title")));
        item->setData(Qt::UserRole, index);
        item->setData(kTaskStatusRole, statusKey);
        item->setData(kTaskDepartmentRole, department);
        item->setData(kTaskRepositoryRole, repository);
        item->setData(kTaskAssigneeKindRole,
                      taskText(task, QStringLiteral("assigneeKind")));
        item->setData(kTaskAssigneeRole,
                      taskText(task, QStringLiteral("assignee")));
        item->setData(kTaskQaRole, qaStatus);
        item->setData(kTaskQaRequestedRole, qaRequested);
        item->setData(
            kTaskPriorityRole,
            QString::number(task.value(QStringLiteral("priority")).toInt()));
        item->setData(
            kTaskCreatedAtRole,
            qint64(task.value(QStringLiteral("createdAt")).toDouble()));
        item->setData(kTaskProgressRole,
                      task.value(QStringLiteral("progress")).toInt());
        item->setData(kTaskCreatorRole,
                      taskText(task, QStringLiteral("createdBy")));
        const QList<forkmesh::taskwork::WorkItem> workItems =
            forkmesh::taskwork::fromJsonArray(
                task.value(QStringLiteral("workItems")).toArray());
        QString branch = workItems.isEmpty() ? QString()
                                             : workItems.constFirst().branch;
        if (workItems.size() > 1)
            branch += QStringLiteral(" +%1").arg(workItems.size() - 1);
        item->setData(kTaskBranchRole, branch);
        item->setData(kTaskThumbnailsRole,
                      taskRowThumbnails(task, m_organizationTaskImageData));
        item->setData(
            kTaskAttachmentCountRole,
            int(task.value(QStringLiteral("attachments")).toArray().size()));
        applyOrganizationTaskAgentFace(item, task);
        applyOrganizationTaskPeopleFaces(item);
        item->setToolTip(taskRowTooltipHtml(task, statusKey, department,
                                             repository, qaStatus, qaRequested));
        m_organizationTasksTable->setItem(row, 0, item);
    }
    m_organizationTaskShownRows = last;
    updateOrganizationTaskPageLabel();
    updateOrganizationTaskSpinTimer();
}

void MainWindow::refreshOrganizationTaskRowThumbnails(const QString &taskId)
{
    if (!m_organizationTasksTable || taskId.isEmpty())
        return;
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_organizationTasksTable->item(row, 0);
        if (!item)
            continue;
        const int index = item->data(Qt::UserRole).toInt();
        if (index < 0 || index >= m_organizationTasks.size())
            continue;
        const QJsonObject task = m_organizationTasks.at(index).toObject();
        if (taskText(task, QStringLiteral("id")) != taskId)
            continue;
        item->setData(kTaskThumbnailsRole,
                      taskRowThumbnails(task, m_organizationTaskImageData));
        return;
    }
}

bool MainWindow::organizationTaskAgentFace(const QJsonObject &task,
                                           QPixmap *face, bool *working) const
{
    const int sessionId =
        localAgentSessionIdForTask(taskText(task, QStringLiteral("id")));
    AgentSession identity;
    bool known = false;
    for (const AgentSession &session : m_agentSessions) {
        if (sessionId > 0 && session.id == sessionId) {
            identity = session;
            known = true;
            break;
        }
    }
    const QJsonObject agent = task.value(QStringLiteral("agent")).toObject();
    if (!known &&
        taskText(task, QStringLiteral("assigneeKind")) ==
            QLatin1String("agent") &&
        (!taskText(agent, QStringLiteral("provider")).isEmpty() ||
         !taskText(agent, QStringLiteral("model")).isEmpty())) {
        identity.provider = taskText(agent, QStringLiteral("provider"));
        identity.model = taskText(agent, QStringLiteral("model"));
        identity.status = taskText(agent, QStringLiteral("status")).toLower();
        known = true;
    }
    if (face)
        *face = known ? agentStatusModelIcon(identity).pixmap(kTaskAgentBadge,
                                                              kTaskAgentBadge)
                      : QPixmap();
    if (working)
        *working = known && !identity.merged &&
                   identity.status == AgentStatus::Running &&
                   !organizationTaskIsClosed(task);
    return known;
}

void MainWindow::applyOrganizationTaskAgentFace(QTableWidgetItem *item,
                                                const QJsonObject &task)
{
    if (!item)
        return;
    QPixmap face;
    bool working = false;
    organizationTaskAgentFace(task, &face, &working);
    item->setData(kTaskAgentFaceRole,
                  face.isNull() ? QVariant() : QVariant(face));
    item->setData(kTaskAgentWorkingRole, working);
    item->setData(kTaskAgentAngleRole, m_organizationTaskSpinAngle);
}

void MainWindow::applyOrganizationTaskPeopleFaces(QTableWidgetItem *item)
{
    if (!item)
        return;
    const auto apply = [this, item](int nameRole, int pictureRole) {
        const QString name = item->data(nameRole).toString().trimmed();
        const QPixmap picture =
            name.isEmpty() ? QPixmap()
                           : chatSenderAvatarSource(QString(), name);
        const QPixmap current = item->data(pictureRole).value<QPixmap>();
        if (current.cacheKey() == picture.cacheKey())
            return;
        item->setData(pictureRole,
                      picture.isNull() ? QVariant() : QVariant(picture));
    };
    apply(kTaskCreatorRole, kTaskCreatorPictureRole);
    apply(kTaskAssigneeRole, kTaskAssigneePictureRole);
}

void MainWindow::refreshOrganizationTaskPeopleFaces()
{
    if (!m_organizationTasksTable)
        return;
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row)
        applyOrganizationTaskPeopleFaces(m_organizationTasksTable->item(row, 0));
    if (!m_organizationTaskPeopleGroup)
        return;
    QHash<QString, int> openByAssignee;
    for (QAbstractButton *button : m_organizationTaskPeopleGroup->buttons())
        openByAssignee.insert(button->property("assigneeFilter").toString(),
                              button->property("openCount").toInt());
    updateOrganizationTaskPeopleStrip(openByAssignee);
}

void MainWindow::rebuildOrganizationTaskPeopleStrip()
{
    if (!m_organizationTaskPeopleLayout || !m_organizationTaskPeopleGroup)
        return;
    const QString actor = m_organizationTaskActor.trimmed().toLower();
    QStringList members;
    for (const QString &member : std::as_const(m_organizationTaskMembers)) {
        if (!member.isEmpty() && !members.contains(member))
            members.append(member);
    }
    std::sort(members.begin(), members.end(),
              [&actor](const QString &left, const QString &right) {
                  if ((left == actor) != (right == actor))
                      return left == actor;
                  return left < right;
              });
    QStringList keys{QStringLiteral("all")};
    for (const QString &member : std::as_const(members))
        keys.append(QStringLiteral("user:") + member);
    keys << QStringLiteral("agent") << QStringLiteral("unassigned");
    if (keys != m_organizationTaskPeopleKeys) {
        QHash<QString, int> previous;
        for (QAbstractButton *button : m_organizationTaskPeopleGroup->buttons()) {
            previous.insert(button->property("assigneeFilter").toString(),
                            button->property("openCount").toInt());
            m_organizationTaskPeopleGroup->removeButton(button);
            button->deleteLater();
        }
        int position = 0;
        for (const QString &key : std::as_const(keys)) {
            auto *button = new QToolButton(m_organizationTaskPeopleStrip);
            button->setObjectName(QStringLiteral("organizationTaskPerson"));
            button->setProperty("assigneeFilter", key);
            button->setProperty("openCount", previous.value(key));
            button->setCheckable(true);
            button->setAutoRaise(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setIconSize(QSize(24, 24));
            m_organizationTaskPeopleGroup->addButton(button);
            m_organizationTaskPeopleLayout->insertWidget(position++, button);
        }
        m_organizationTaskPeopleKeys = keys;
    }
    QHash<QString, int> openByAssignee;
    for (QAbstractButton *button : m_organizationTaskPeopleGroup->buttons())
        openByAssignee.insert(button->property("assigneeFilter").toString(),
                              button->property("openCount").toInt());
    updateOrganizationTaskPeopleStrip(openByAssignee);
}

void MainWindow::updateOrganizationTaskPeopleStrip(
    const QHash<QString, int> &openByAssignee)
{
    if (!m_organizationTaskPeopleGroup)
        return;
    const QString actor = m_organizationTaskActor.trimmed().toLower();
    const QColor glyph =
        m_organizationTaskPeopleStrip->palette().color(QPalette::WindowText);
    const auto glyphFace = [&glyph](const QString &octicon) {
        constexpr int side = 24;
        const qreal ratio = iconDevicePixelRatio();
        QPixmap face(QSize(side, side) * ratio);
        face.setDevicePixelRatio(ratio);
        face.fill(Qt::transparent);
        QPainter painter(&face);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor ring = glyph;
        ring.setAlpha(70);
        painter.setPen(QPen(ring, 1));
        QColor fill = glyph;
        fill.setAlpha(24);
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(0.5, 0.5, side - 1, side - 1));
        painter.drawPixmap(QPoint(4, 4), tintedOcticonPixmap(octicon, glyph, 16));
        return face;
    };
    for (QAbstractButton *button : m_organizationTaskPeopleGroup->buttons()) {
        const QString key = button->property("assigneeFilter").toString();
        const int open = openByAssignee.value(key);
        button->setProperty("openCount", open);
        QString label;
        QPixmap face;
        if (key == QLatin1String("all")) {
            label = QStringLiteral("Everyone");
            face = glyphFace(QStringLiteral("people"));
        } else if (key == QLatin1String("agent")) {
            label = QStringLiteral("Bot");
            face = glyphFace(QStringLiteral("sparkle"));
        } else if (key == QLatin1String("unassigned")) {
            label = QStringLiteral("Unassigned");
            face = glyphFace(QStringLiteral("circle-slash"));
        } else {
            const QString name = key.mid(5);
            label = name == actor ? QStringLiteral("@%1 (you)").arg(name)
                                  : QStringLiteral("@%1").arg(name);
            face = taskPersonFace(
                QVariant(chatSenderAvatarSource(QString(), name)), name, 24,
                0.5);
        }
        button->setIcon(QIcon(face));
        button->setText(QString::number(open));
        button->setAccessibleName(label);
        button->setToolTip(QStringLiteral("%1: %2 open").arg(label).arg(open));
        button->setChecked(key == m_organizationTaskAssigneeFilter);
    }
}

void MainWindow::setOrganizationTaskAssigneeFilter(const QString &filter)
{
    m_organizationTaskAssigneeFilter =
        filter.isEmpty() ? QStringLiteral("all") : filter;
    if (!m_organizationTaskPeopleGroup)
        return;
    for (QAbstractButton *button : m_organizationTaskPeopleGroup->buttons())
        button->setChecked(button->property("assigneeFilter").toString() ==
                           m_organizationTaskAssigneeFilter);
}

QJsonObject MainWindow::organizationTaskForRow(int row) const
{
    const QTableWidgetItem *item =
        m_organizationTasksTable ? m_organizationTasksTable->item(row, 0)
                                 : nullptr;
    if (!item)
        return {};
    const int index = item->data(Qt::UserRole).toInt();
    return index >= 0 && index < m_organizationTasks.size()
               ? m_organizationTasks.at(index).toObject()
               : QJsonObject();
}

int MainWindow::organizationTaskRowForId(const QString &taskId) const
{
    if (!m_organizationTasksTable || taskId.isEmpty())
        return -1;
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row) {
        if (taskText(organizationTaskForRow(row), QStringLiteral("id")) ==
            taskId)
            return row;
    }
    return -1;
}

void MainWindow::refreshOrganizationTaskAgentRows()
{
    if (!m_organizationTasksTable)
        return;
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row)
        applyOrganizationTaskAgentFace(m_organizationTasksTable->item(row, 0),
                                       organizationTaskForRow(row));
    updateOrganizationTaskSpinTimer();
}

bool MainWindow::organizationTaskRowsHaveWorkingAgent() const
{
    if (!m_organizationTasksTable)
        return false;
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_organizationTasksTable->item(row, 0);
        if (item && item->data(kTaskAgentWorkingRole).toBool())
            return true;
    }
    return false;
}

void MainWindow::updateOrganizationTaskSpinTimer()
{
    const bool wanted = m_organizationTasksTable &&
                        m_organizationTasksTable->isVisible() &&
                        m_organizationTaskListView !=
                            forkmesh::ui::ListViewMode::Zen &&
                        organizationTaskRowsHaveWorkingAgent();
    if (!wanted) {
        if (m_organizationTaskSpinTimer)
            m_organizationTaskSpinTimer->stop();
        return;
    }
    if (!m_organizationTaskSpinTimer) {
        m_organizationTaskSpinTimer = new QTimer(this);
        m_organizationTaskSpinTimer->setInterval(kTaskAgentSpinTickMs);
        connect(m_organizationTaskSpinTimer, &QTimer::timeout, this,
                &MainWindow::tickOrganizationTaskSpinners);
    }
    if (!m_organizationTaskSpinTimer->isActive())
        m_organizationTaskSpinTimer->start();
}

void MainWindow::setOrganizationTaskListView(forkmesh::ui::ListViewMode view)
{
    m_organizationTaskListView = view;
    QString description;
    switch (view) {
    case forkmesh::ui::ListViewMode::Compact:
        description = QStringLiteral("priority, title, status, assignee and "
                                     "age on one line");
        break;
    case forkmesh::ui::ListViewMode::Zen:
        description = QStringLiteral("nothing but each task's title; hover a "
                                     "task for its details");
        break;
    case forkmesh::ui::ListViewMode::Full:
        description = QStringLiteral("the title, with status, department, "
                                     "repository, branch, QA and age in "
                                     "columns underneath");
        break;
    }
    forkmesh::ui::storeListViewMode(QStringLiteral("tasks/listView"), view);
    if (m_organizationTaskViewButton) {
        const forkmesh::ui::ListViewModeLabel label =
            forkmesh::ui::listViewModeLabel(view);
        m_organizationTaskViewButton->setText(label.caption);
        setOcticon(m_organizationTaskViewButton, label.icon, 16);
        m_organizationTaskViewButton->setAccessibleName(
            forkmesh::ui::listViewModeAccessibleName(
                QStringLiteral("Task list"), view));
        m_organizationTaskViewButton->setToolTip(
            forkmesh::ui::listViewModeTooltip(view, description));
    }
    if (!m_organizationTasksTable)
        return;
    if (auto *delegate = dynamic_cast<OrganizationTaskRowDelegate *>(
            m_organizationTasksTable->itemDelegate()))
        delegate->setMode(view);
    const int rowHeight = taskRowHeight(view);
    m_organizationTasksTable->verticalHeader()->setDefaultSectionSize(
        rowHeight);
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row)
        m_organizationTasksTable->setRowHeight(row, rowHeight);
    m_organizationTasksTable->viewport()->update();
    updateOrganizationTaskSpinTimer();
}

void MainWindow::tickOrganizationTaskSpinners()
{
    if (!m_organizationTasksTable || !m_organizationTasksTable->isVisible()) {
        if (m_organizationTaskSpinTimer)
            m_organizationTaskSpinTimer->stop();
        return;
    }
    m_organizationTaskSpinAngle = std::fmod(
        m_organizationTaskSpinAngle + kTaskAgentSpinStepDegrees, 360.0);
    bool anyWorking = false;
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_organizationTasksTable->item(row, 0);
        if (!item || !item->data(kTaskAgentWorkingRole).toBool())
            continue;
        bool working = false;
        organizationTaskAgentFace(organizationTaskForRow(row), nullptr,
                                  &working);
        if (!working) {
            item->setData(kTaskAgentWorkingRole, false);
            continue;
        }
        anyWorking = true;
        item->setData(kTaskAgentAngleRole, m_organizationTaskSpinAngle);
    }
    if (!anyWorking && m_organizationTaskSpinTimer)
        m_organizationTaskSpinTimer->stop();
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testSetAccountAvatar(const QString &account,
                                      const QPixmap &picture)
{
    const QString key = QStringLiteral("user:") + account.trimmed().toLower();
    if (picture.isNull())
        m_avatars.remove(key);
    else
        m_avatars.insert(key, picture);
    refreshOrganizationTaskPeopleFaces();
}

QPixmap MainWindow::testOrganizationTaskCreatorPicture(
    const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row < 0 ? QPixmap()
                   : m_organizationTasksTable->item(row, 0)
                         ->data(kTaskCreatorPictureRole)
                         .value<QPixmap>();
}

QPixmap MainWindow::testOrganizationTaskAssigneePicture(
    const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row < 0 ? QPixmap()
                   : m_organizationTasksTable->item(row, 0)
                         ->data(kTaskAssigneePictureRole)
                         .value<QPixmap>();
}

QPixmap MainWindow::testOrganizationTaskAgentFace(const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row < 0 ? QPixmap()
                   : m_organizationTasksTable->item(row, 0)
                         ->data(kTaskAgentFaceRole)
                         .value<QPixmap>();
}

bool MainWindow::testOrganizationTaskAgentWorking(const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row >= 0 && m_organizationTasksTable->item(row, 0)
                           ->data(kTaskAgentWorkingRole)
                           .toBool();
}

bool MainWindow::testOrganizationTaskSpinTimerActive() const
{
    return m_organizationTaskSpinTimer &&
           m_organizationTaskSpinTimer->isActive();
}

QImage MainWindow::testOrganizationTaskRowImage(const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    if (row < 0)
        return {};
    const QRect rect = m_organizationTasksTable->visualItemRect(
        m_organizationTasksTable->item(row, 0));
    return m_organizationTasksTable->viewport()->grab(rect).toImage();
}
#endif

void MainWindow::updateOrganizationTaskPageLabel()
{
    if (!m_organizationTaskPageLabel)
        return;
    const int total = m_organizationTaskMatches.size();
    if (total == 0) {
        m_organizationTaskPageLabel->setText(
            m_organizationTasks.isEmpty()
                ? QStringLiteral("No tasks")
                : QStringLiteral("No tasks match these filters"));
        return;
    }
    m_organizationTaskPageLabel->setText(
        m_organizationTaskShownRows < total
            ? QString::fromUtf8("%1 of %2 \xC2\xB7 scroll for more")
                  .arg(m_organizationTaskShownRows)
                  .arg(total)
            : (total == 1 ? QStringLiteral("1 task")
                          : QStringLiteral("%1 tasks").arg(total)));
}

void MainWindow::openCreatedOrganizationTask(const QJsonObject &task)
{
    const QString taskId = taskText(task, QStringLiteral("id"));
    ensureSectionBuilt(kOrganizationTasksSectionIndex);
    if (!m_organizationTasksTable || taskId.isEmpty()) {
        showSection(kOrganizationTasksSectionIndex);
        if (m_organizationTasksLoaded)
            refreshOrganizationTasks();
        return;
    }

    // The create response already carries the relay's canonical task. Put that
    // answer on screen immediately instead of hiding it behind another catalog
    // round trip. A later refresh may restore the catalog's normal sort order,
    // but preserves this selection by id.
    int existingIndex = -1;
    for (int index = 0; index < m_organizationTasks.size(); ++index) {
        if (taskText(m_organizationTasks.at(index).toObject(),
                     QStringLiteral("id")) == taskId) {
            existingIndex = index;
            break;
        }
    }
    if (existingIndex >= 0)
        m_organizationTasks.replace(existingIndex, task);
    else
        m_organizationTasks.insert(0, task);

    // A filter or another page must not make a successful create look as if it
    // vanished. The first row is the newly-created task, and selecting it opens
    // its detail pane.
    m_organizationTasksSearchMirror.clear();
    if (m_organizationTaskMineFilterButton) {
        const QSignalBlocker blocker(m_organizationTaskMineFilterButton);
        m_organizationTaskMineFilterButton->setChecked(false);
    }
    setOrganizationTaskAssigneeFilter(QStringLiteral("all"));
    revealOrganizationTaskStatus(task);
    m_organizationTaskShownRows = 0;
    renderOrganizationTaskRows(taskId);

    const int openCount = openOrganizationTaskCount(m_organizationTasks);
    if (m_organizationTasksSummary) {
        m_organizationTasksSummary->setText(
            QString::fromUtf8("%1 open \xC2\xB7 %2 closed%3")
                .arg(openCount)
                .arg(m_organizationTasks.size() - openCount)
                .arg(m_organizationTasksCanManage
                         ? QString::fromUtf8(" \xC2\xB7 manager")
                         : QString()));
    }
    setOrganizationTaskBadge(openCount);
    showSection(kOrganizationTasksSectionIndex);
    if (QTableWidgetItem *item = m_organizationTasksTable->currentItem())
        m_organizationTasksTable->scrollToItem(item);
}

void MainWindow::renderOrganizationTaskDetail()
{
    if (!m_organizationTaskDetail)
        return;
    updateOrganizationTaskRefreshAge();
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString taskId = taskText(task, QStringLiteral("id"));
    const int sessionId = localAgentSessionIdForTask(taskId);
    const QString details = taskText(task, QStringLiteral("details"));
    for (const QString &path : promptAttachmentPaths(details)) {
        if (isVideoAttachmentPath(path) || !QFileInfo::exists(path))
            continue;
        const QString key = taskLocalImageCacheKey(taskId, path);
        if (m_organizationTaskImageData.contains(key))
            continue;
        QString mime;
        const QByteArray bytes = taskImagePreviewBytes(path, &mime);
        if (!bytes.isEmpty()) {
            m_organizationTaskImageData.insert(key, bytes);
            m_organizationTaskImageMime.insert(key, mime);
        }
    }
    for (const QJsonValue &value :
         task.value(QStringLiteral("attachments")).toArray()) {
        const QJsonObject attachment = value.toObject();
        const QString attachmentId =
            taskText(attachment, QStringLiteral("id"));
        const QString key = taskImageCacheKey(taskId, attachmentId);
        if (!attachmentId.isEmpty() &&
            !m_organizationTaskImageData.contains(key)) {
            QString mime;
            const QByteArray thumbnail = taskImageDataUriBytes(
                attachment.value(QStringLiteral("thumbnail")).toString(),
                &mime);
            if (!thumbnail.isEmpty()) {
                m_organizationTaskImageData.insert(key, thumbnail);
                m_organizationTaskImageMime.insert(key, mime);
            }
        }
        if (!attachmentId.isEmpty() &&
            !m_organizationTaskImageData.contains(key) &&
            !m_organizationTaskImageLoads.contains(key)) {
            m_organizationTaskImageLoads.insert(key);
            requestOrganizationTaskAttachment(
                taskId, attachmentId,
                [this, taskId, key](bool ok, const QByteArray &bytes,
                                    const QString &mime, const QString &) {
                    m_organizationTaskImageLoads.remove(key);
                    if (ok && !bytes.isEmpty()) {
                        QImage image = QImage::fromData(bytes);
                        if (!image.isNull()) {
                            m_organizationTaskImageData.insert(key, bytes);
                            m_organizationTaskImageMime.insert(
                                key, taskImageMimeAllowed(mime)
                                         ? mime
                                         : QStringLiteral("image/jpeg"));
                            refreshOrganizationTaskRowThumbnails(taskId);
                        }
                    }
                    const QJsonObject selected = selectedOrganizationTask(
                        m_organizationTasksTable, m_organizationTasks);
                    if (taskText(selected, QStringLiteral("id")) == taskId)
                        renderOrganizationTaskDetail();
                });
        }
    }
    m_organizationTaskDetail->setHtml(
        taskDetailHtml(task, m_organizationTaskImageData,
                       m_organizationTaskImageMime,
                       m_organizationTaskHistory.value(taskId)));
    if (m_organizationTaskFields) {
        m_organizationTaskFields->setHtml(taskFieldsHtml(
            task, localAgentRunLabelForTask(taskId), sessionId));
        m_organizationTaskFields->setVisible(!task.isEmpty());
    }
    if (!taskId.isEmpty() &&
        (!m_organizationTaskHistory.contains(taskId) ||
         QDateTime::currentMSecsSinceEpoch() -
                 m_organizationTaskHistoryAt.value(taskId) >
             kOrganizationTaskHistoryStaleMs))
        loadOrganizationTaskHistory(taskId);
}

int MainWindow::localAgentSessionIdForTask(const QString &taskId) const
{
    if (taskId.isEmpty())
        return 0;
    const AgentSession *newest = nullptr;
    for (const AgentSession &session : m_agentSessions) {
        if (session.orgTaskId != taskId || session.id <= 0)
            continue;
        if (!newest || session.createdAtMs > newest->createdAtMs)
            newest = &session;
    }
    return newest ? newest->id : 0;
}

void MainWindow::loadOrganizationTaskHistory(const QString &taskId)
{
    if (taskId.isEmpty() || m_organizationTaskHistoryInFlight == taskId)
        return;
    m_organizationTaskHistoryInFlight = taskId;
    const qint64 sentAt = QDateTime::currentMSecsSinceEpoch();
    requestOrganizationTasks(
        QByteArrayLiteral("GET"),
        QStringLiteral("/api/tasks/%1/history").arg(taskId), QJsonObject(),
        [this, taskId, sentAt](bool ok, const QJsonObject &payload,
                               const QString &) {
            if (m_organizationTaskHistoryInFlight == taskId)
                m_organizationTaskHistoryInFlight.clear();
            const bool cached = m_organizationTaskHistory.contains(taskId);
            if (cached && sentAt < m_organizationTaskHistoryAt.value(taskId))
                return;
            m_organizationTaskHistoryAt.insert(taskId, sentAt);
            if (ok) {
                m_organizationTaskHistory.insert(
                    taskId,
                    taskHistoryWithReplies(
                        payload.value(QStringLiteral("history")).toArray(),
                        payload.value(QStringLiteral("replies")).toArray()));
            } else if (!cached) {
                m_organizationTaskHistory.insert(taskId, QJsonArray());
            }
            const QJsonObject selected = selectedOrganizationTask(
                m_organizationTasksTable, m_organizationTasks);
            if (taskText(selected, QStringLiteral("id")) == taskId)
                renderOrganizationTaskDetail();
        });
}

QString MainWindow::localAgentRunLabelForTask(const QString &taskId) const
{
    const int sessionId = localAgentSessionIdForTask(taskId);
    const AgentSession *session = nullptr;
    for (const AgentSession &candidate : m_agentSessions) {
        if (candidate.id == sessionId) {
            session = &candidate;
            break;
        }
    }
    if (!session)
        return QString();
    QStringList parts;
    parts << QStringLiteral("#%1").arg(session->id);
    const QString provider = agentProviderName(session->provider);
    if (!provider.isEmpty())
        parts << provider;
    if (!session->status.isEmpty())
        parts << session->status;
    return parts.join(QStringLiteral(", ")) +
           QStringLiteral(" (this desktop)");
}

void MainWindow::openOrganizationTask(const QString &taskId)
{
    if (taskId.isEmpty())
        return;
    m_pendingOrganizationTaskSelection = taskId;
    showSection(kOrganizationTasksSectionIndex);
    if (!m_organizationTasksTable)
        return;
    if (!m_organizationTasksLoaded) {
        refreshOrganizationTasks();
        return;
    }
    m_organizationTasksSearchMirror.clear();
    if (m_organizationTaskMineFilterButton) {
        const QSignalBlocker blocker(m_organizationTaskMineFilterButton);
        m_organizationTaskMineFilterButton->setChecked(false);
    }
    setOrganizationTaskAssigneeFilter(QStringLiteral("all"));
    for (int i = 0; i < m_organizationTasks.size(); ++i) {
        if (taskText(m_organizationTasks.at(i).toObject(),
                     QStringLiteral("id")) != taskId)
            continue;
        revealOrganizationTaskStatus(m_organizationTasks.at(i).toObject());
        m_organizationTaskShownRows = 0;
        m_pendingOrganizationTaskSelection.clear();
        renderOrganizationTaskRows(taskId);
        return;
    }
    refreshOrganizationTasks();
}

void MainWindow::revealOrganizationTaskStatus(const QJsonObject &task)
{
    const bool closed = organizationTaskIsClosed(task);
    if (m_organizationTaskStatusFilter == QLatin1String("all") ||
        (m_organizationTaskStatusFilter == QLatin1String("closed")) == closed)
        return;
    m_organizationTaskStatusFilter = QStringLiteral("all");
    if (m_organizationTaskAllFilterButton)
        m_organizationTaskAllFilterButton->setChecked(true);
}

void MainWindow::linkExistingAgentsToOrganizationTasks(const QJsonArray &tasks)
{
    if (!m_agentStore)
        return;

    bool changed = false;
    for (const QJsonValue &value : tasks) {
        const QJsonObject task = value.toObject();
        QString agentId = task.value(QStringLiteral("agent"))
                              .toObject()
                              .value(QStringLiteral("sessionId"))
                              .toString()
                              .trimmed();
        if (agentId.isEmpty())
            agentId = taskText(task, QStringLiteral("agentSessionId"));
        bool numeric = false;
        const int sessionId = agentId.toInt(&numeric);
        if (!numeric || sessionId <= 0)
            continue; // an opaque remote bot id cannot name a local session
        AgentSession *session = findAgentSession(sessionId);
        if (!session || !session->orgTaskId.isEmpty())
            continue;
        const QString repository =
            session->owner + QLatin1Char('/') + session->name;
        if (taskText(task, QStringLiteral("repository"))
                .compare(repository, Qt::CaseInsensitive) != 0)
            continue;
        const QJsonObject agent = task.value(QStringLiteral("agent")).toObject();
        const QString taskProvider =
            taskText(agent, QStringLiteral("provider"));
        if (!taskProvider.isEmpty() && !session->provider.isEmpty() &&
            taskProvider != session->provider)
            continue;
        // Session numbers are local to a desktop. Repository and provider
        // narrow the candidate; matching either persisted title or prompt
        // prevents another member's coincidental session #12 from being linked
        // to this machine's #12.
        bool comparableContent = false;
        bool matchingContent = false;
        const QString title = taskText(task, QStringLiteral("title"));
        if (!title.isEmpty() && !session->issueTitle.isEmpty()) {
            comparableContent = true;
            matchingContent |= title == session->issueTitle;
        }
        const QString details = taskText(task, QStringLiteral("details"));
        if (!details.isEmpty() && !session->prompt.isEmpty()) {
            comparableContent = true;
            matchingContent |= details == session->prompt.trimmed();
        }
        if (comparableContent && !matchingContent)
            continue;
        session->orgTask = true;
        session->orgTaskId = taskText(task, QStringLiteral("id"));
        changed |= m_agentStore->saveSession(*session);
        syncOrgTaskAgentStatus(sessionId);
        refreshAgentTaskLinkUi(sessionId);
    }

    if (changed)
        refreshAgentTable();
}

void MainWindow::updateOrganizationTaskActions()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const bool selected = !task.isEmpty();
    const bool done = organizationTaskIsClosed(task);
    const bool active =
        taskText(task, QStringLiteral("status")) == QLatin1String("active");
    const bool agent =
        taskText(task, QStringLiteral("assigneeKind")) ==
        QLatin1String("agent");
    const bool mine =
        taskText(task, QStringLiteral("assignee")).compare(
            m_organizationTaskActor, Qt::CaseInsensitive) == 0;
    const QString qaReviewer = task.value(QStringLiteral("qa"))
                                   .toObject()
                                   .value(QStringLiteral("reviewer"))
                                   .toString()
                                   .trimmed();
    const bool hasRepository =
        !taskText(task, QStringLiteral("repository")).isEmpty();
    if (m_organizationTaskNewButton)
        m_organizationTaskNewButton->setVisible(
            m_organizationTasksCanManage);
    if (m_organizationTaskDeleteClosedButton) {
        int closedCount = 0;
        for (const QJsonValue &value : std::as_const(m_organizationTasks)) {
            if (organizationTaskIsClosed(value.toObject()))
                ++closedCount;
        }
        m_organizationTaskDeleteClosedButton->setVisible(
            m_organizationTasksCanManage);
        m_organizationTaskDeleteClosedButton->setEnabled(
            m_organizationTasksCanManage && closedCount > 0 &&
            !m_organizationTaskClosedDeleteInFlight &&
            !m_organizationTasksLoading);
    }
    if (m_organizationTaskEditButton)
        m_organizationTaskEditButton->setEnabled(
            selected && m_organizationTasksCanManage && !active && !done);
    if (m_organizationTaskAgentButton) {
        const int linkedSessionId = localAgentSessionIdForTask(
            taskText(task, QStringLiteral("id")));
        m_organizationTaskAgentButton->setText(
            linkedSessionId > 0
                ? QStringLiteral("Agent #%1").arg(linkedSessionId)
                : QStringLiteral("Assign Bot"));
        m_organizationTaskAgentButton->setToolTip(
            linkedSessionId > 0
                ? QStringLiteral("Open linked agent session #%1")
                      .arg(linkedSessionId)
                : QStringLiteral("Assign this task to an agent"));
        m_organizationTaskAgentButton->setEnabled(
            linkedSessionId > 0 ||
            (selected && m_organizationTasksCanManage && !active && !done &&
             hasRepository));
    }
    if (m_organizationTaskStartButton) {
        m_organizationTaskStartButton->setText(
            active ? QStringLiteral("Stop") : QStringLiteral("Start"));
        m_organizationTaskStartButton->setEnabled(
            selected && mine && !done);
    }
    if (m_organizationTaskCompleteButton)
        m_organizationTaskCompleteButton->setEnabled(
            selected && !done && (mine || m_organizationTasksCanManage));
    if (m_organizationTaskCompleteButton)
        m_organizationTaskCompleteButton->setVisible(!done);
    if (m_organizationTaskUndoButton)
        m_organizationTaskUndoButton->setVisible(done);
    if (m_organizationTaskIssueButton)
        m_organizationTaskIssueButton->setVisible(!done);
    if (m_organizationTaskUndoButton)
        m_organizationTaskUndoButton->setEnabled(
            selected && done &&
            (mine || m_organizationTasksCanManage ||
             taskText(task, QStringLiteral("createdBy")) ==
                 m_organizationTaskActor ||
             qaReviewer.compare(m_organizationTaskActor, Qt::CaseInsensitive) ==
                 0));
    if (m_organizationTaskQaButton)
        m_organizationTaskQaButton->setEnabled(
            selected && !done && m_organizationTasksCanManage);
    if (m_organizationTaskReturnButton)
        m_organizationTaskReturnButton->setEnabled(
            selected && !done && agent && m_organizationTasksCanManage);
    if (m_organizationTaskDeleteButton)
        m_organizationTaskDeleteButton->setEnabled(
            selected && m_organizationTasksCanManage);
    if (m_organizationTaskAttachWorktreeButton)
        m_organizationTaskAttachWorktreeButton->setEnabled(
            selected &&
            (m_organizationTasksCanManage || mine ||
             taskText(task, QStringLiteral("createdBy")) ==
                 m_organizationTaskActor));
    if (m_organizationTaskFollowUpButton)
        m_organizationTaskFollowUpButton->setEnabled(
            selected &&
            (m_organizationTasksCanManage || mine ||
             taskText(task, QStringLiteral("createdBy")) ==
                 m_organizationTaskActor));
    if (m_organizationTaskPromptButton)
        m_organizationTaskPromptButton->setEnabled(selected);
    if (m_organizationTaskIssueButton)
        m_organizationTaskIssueButton->setEnabled(
            selected && m_organizationTasksCanManage && !active && !done &&
            !m_organizationTaskIssueMoveInFlight && hasRepository);
    if (m_organizationTaskStartAgentButton)
        m_organizationTaskStartAgentButton->setEnabled(selected && !done);
    // Any member who can see the task can comment on it, including on a task
    // that is already done — that is where the QA conversation happens.
    if (m_organizationTaskCommentButton)
        m_organizationTaskCommentButton->setEnabled(selected);
    // Checking in is the assignee saying how their running work is going, so
    // it is offered exactly when the relay would accept one.
    if (m_organizationTaskCheckinButton)
        m_organizationTaskCheckinButton->setEnabled(
            selected && mine && active && !done);
    if (m_organizationTaskRemindButton)
        m_organizationTaskRemindButton->setEnabled(
            selected && !done && !mine &&
            taskText(task, QStringLiteral("assigneeKind")) ==
                QLatin1String("user") &&
            !taskText(task, QStringLiteral("assignee")).isEmpty());
}

void MainWindow::addOrganizationTaskToPrompt()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    if (task.isEmpty())
        return;
    appendTextToActivePrompt(organizationTaskPromptText(task));
    if (m_organizationTasksStatus)
        m_organizationTasksStatus->setText(
            QStringLiteral("Task added to the prompt box."));
}

void MainWindow::startOrganizationTaskAgentFromPrompt()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    if (task.isEmpty() || !m_organizationTasksStatus)
        return;
    const QString id = taskText(task, QStringLiteral("id"));
    const QString title = taskText(task, QStringLiteral("title"));
    if (id.isEmpty())
        return;

    const QString provider =
        m_quickAddAgentProvider
            ? m_quickAddAgentProvider->currentData().toString()
            : QStringLiteral("claude-code");
    if (quickAddProviderIsPromptAction(provider) ||
        agentIsCloudflareAiProvider(provider)) {
        m_organizationTasksStatus->setText(QStringLiteral(
            "Pick a coding agent in the prompt box before starting one on a "
            "task."));
        return;
    }

    const QStringList repository =
        taskText(task, QStringLiteral("repository"))
            .split(QLatin1Char('/'), Qt::SkipEmptyParts);
    int repoIndex = repository.size() == 2
                        ? repoIndexFor(repository.at(0), repository.at(1))
                        : -1;
    if (repoIndex >= 0 && repoIndex < m_repositories.size() &&
        m_repositories.at(repoIndex).localPath.isEmpty())
        repoIndex = -1;
    if (repoIndex < 0)
        repoIndex = issuesRepoIndex();
    if (repoIndex < 0) {
        m_organizationTasksStatus->setText(QStringLiteral(
            "Open a repository with a local checkout to run this task's "
            "agent."));
        return;
    }

    addOrganizationTaskToPrompt();
    QString prompt = m_issueQuickAdd ? m_issueQuickAdd->toPlainText().trimmed()
                                     : organizationTaskPromptText(task);
    if (prompt.isEmpty())
        return;
    const QStringList images = m_quickAddImages;
    for (const QString &attachment : images) {
        if (!prompt.endsWith(QLatin1Char('\n')))
            prompt += QLatin1Char('\n');
        prompt += promptAttachmentLine(attachment);
    }

    const QString model = (provider == QLatin1String("claude-code") ||
                           agentIsCodexProvider(provider))
                              ? selectedModelComboValue(m_quickAddClaudeModel)
                              : QString();
    const bool createPr =
        m_quickAddCreatePr && m_quickAddCreatePr->isChecked();
    const int sessionId = startAdHocAgentForRepo(
        repoIndex, prompt, provider, createPr, model,
        /*titleOverride=*/title, /*genie=*/false, /*switchToTab=*/true,
        /*orgTaskId=*/id, /*modeOverride=*/QString(),
        /*baseBranch=*/QString(),
        /*priority=*/task.value(QStringLiteral("priority")).toInt(50));
    if (sessionId <= 0) {
        m_organizationTasksStatus->setText(
            QStringLiteral("Could not start an agent on this task."));
        return;
    }
    if (m_issueQuickAdd)
        m_issueQuickAdd->clear();
    clearQuickAddImages();
    QStringList details;
    const QString modelLabel = agentModelLabel(model);
    if (!modelLabel.isEmpty())
        details << modelLabel;
    if (m_quickAddModeSelector)
        details << m_quickAddModeSelector->currentText();
    const QString started =
        QStringLiteral("Started %1 agent session #%2 on this task%3.")
            .arg(agentProviderName(provider))
            .arg(sessionId)
            .arg(details.isEmpty()
                     ? QString()
                     : QStringLiteral(" (%1)")
                           .arg(details.join(QStringLiteral(", "))));
    m_organizationTasksStatus->setText(started);
    logSystem(started);
    renderOrganizationTaskDetail();
}

void MainWindow::createOrganizationTask()
{
    composeOrganizationTask(QStringLiteral("New organization task"), QString(),
                            QString(), nullptr);
}

void MainWindow::composeOrganizationTask(const QString &windowTitle,
                                         const QString &prefillTitle,
                                         const QString &prefillDetails,
                                         std::function<void()> onCreated)
{
    QDialog dialog(this);
    dialog.setWindowTitle(windowTitle);
    auto *layout = new QFormLayout(&dialog);
    auto *title = new QLineEdit(prefillTitle);
    title->setMaxLength(kOrganizationTaskTitleLimit);
    auto *details = new QPlainTextEdit(prefillDetails);
    details->setMaximumHeight(120);
    auto *department = new QComboBox;
    department->addItems(m_organizationTaskDepartments.isEmpty()
                             ? QStringList{QStringLiteral("general"),
                                           QStringLiteral("engineering")}
                             : m_organizationTaskDepartments);
    auto *repository = new QLineEdit(QStringLiteral("forkmesh/forkmesh"));
    auto *assignee = new QComboBox;
    assignee->addItem(QStringLiteral("Bot"), QStringLiteral("agent"));
    assignee->addItem(QStringLiteral("Unassigned"),
                      QStringLiteral("unassigned"));
    for (const QString &member : std::as_const(m_organizationTaskMembers))
        assignee->addItem(member, QStringLiteral("user:") + member);
    auto *priority = new QSpinBox;
    priority->setRange(1, 99);
    priority->setValue(50);
    layout->addRow(QStringLiteral("Title"), title);
    layout->addRow(QStringLiteral("Details"), details);
    layout->addRow(QStringLiteral("Department"), department);
    layout->addRow(QStringLiteral("Repository"), repository);
    layout->addRow(QStringLiteral("Assignee"), assignee);
    layout->addRow(QStringLiteral("Priority"), priority);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    title->setFocus();
    title->selectAll();
    if (dialog.exec() != QDialog::Accepted || title->text().trimmed().isEmpty())
        return;

    const QString assignment = assignee->currentData().toString();
    const bool agent = assignment == QLatin1String("agent");
    QJsonObject body{
        {QStringLiteral("title"), title->text().trimmed()},
        {QStringLiteral("details"), details->toPlainText().trimmed()},
        {QStringLiteral("department"), department->currentText()},
        {QStringLiteral("destination"),
         agent ? QStringLiteral("agent") : QStringLiteral("department")},
        {QStringLiteral("assigneeKind"),
         agent ? QStringLiteral("agent")
               : assignment == QLatin1String("unassigned")
                   ? QStringLiteral("unassigned")
                   : QStringLiteral("user")},
        {QStringLiteral("repository"), repository->text().trimmed()},
        {QStringLiteral("priority"), priority->value()},
    };
    if (assignment.startsWith(QLatin1String("user:")))
        body.insert(QStringLiteral("assignee"), assignment.mid(5));
    requestOrganizationTasks(
        QByteArrayLiteral("POST"), QStringLiteral("/api/tasks"), body,
        [this, agent, onCreated = std::move(onCreated)](
            bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok) {
                const QString failure =
                    QStringLiteral("Task creation failed: %1").arg(error);
                m_organizationTasksStatus->setText(failure);
                // A conversion is started from another page, where that status
                // label is off screen — say it where the operator is standing.
                if (onCreated)
                    flashMessage(failure, true);
                return;
            }
            const QJsonObject task =
                payload.value(QStringLiteral("task")).toObject();
            m_organizationTasksStatus->setText(QStringLiteral("Task created."));
            openCreatedOrganizationTask(task);
            if (agent)
                queueOrganizationTaskAgent(task);
            if (onCreated)
                onCreated();
        });
}

void MainWindow::createQuickAddOrganizationTask()
{
    if (!m_issueQuickAdd)
        return;
    const QString prompt = m_issueQuickAdd->toPlainText().trimmed();
    if (prompt.isEmpty()) {
        flashMessage(QStringLiteral("Type a task first."), true, QString(),
                     forkmesh::kLocalOperationNoticeKind);
        m_issueQuickAdd->setFocus();
        return;
    }

    QString title = prompt.section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.size() > 160)
        title = title.left(159).trimmed() + QString::fromUtf8("\xE2\x80\xA6");
    QString repository;
    const int repoIndex = issuesRepoIndex();
    if (repoIndex >= 0 && repoIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(repoIndex);
        repository = repo.owner + QLatin1Char('/') + repo.name;
    }
    QString attachmentError;
    const QJsonArray attachments = organizationTaskImageAttachments(
        m_quickAddImages, &attachmentError);
    if (!attachmentError.isEmpty()) {
        flashMessage(attachmentError, true);
        return;
    }
    QJsonObject body{
        {QStringLiteral("title"), title},
        {QStringLiteral("details"), promptTextWithoutAttachments(prompt)},
        {QStringLiteral("department"), QStringLiteral("general")},
        {QStringLiteral("destination"), QStringLiteral("department")},
        {QStringLiteral("assigneeKind"), QStringLiteral("unassigned")},
        {QStringLiteral("repository"), repository},
        {QStringLiteral("priority"), 50},
    };
    if (!attachments.isEmpty())
        body.insert(QStringLiteral("attachments"), attachments);
    requestOrganizationTasks(
        QByteArrayLiteral("POST"), QStringLiteral("/api/tasks"), body,
        [this, prompt](bool ok, const QJsonObject &payload,
                       const QString &error) {
            if (!ok) {
                flashMessage(QStringLiteral("Task creation failed: %1").arg(error),
                             true);
                return;
            }
            recordQuickAddHistory(prompt);
            m_issueQuickAdd->clear();
            clearQuickAddImages();
            openCreatedOrganizationTask(
                payload.value(QStringLiteral("task")).toObject());
            flashMessage(QStringLiteral("Task added to General."));
        });
}

void MainWindow::createOrganizationTaskFollowUp()
{
    const QJsonObject parent = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString parentId = taskText(parent, QStringLiteral("id"));
    if (parentId.isEmpty())
        return;
    bool accepted = false;
    const QString title = QInputDialog::getText(
        this, QStringLiteral("Follow-up task"),
        QStringLiteral("Task title (assignee is inherited)"),
        QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || title.isEmpty())
        return;
    requestOrganizationTasks(
        QByteArrayLiteral("POST"), QStringLiteral("/api/tasks"),
        {{QStringLiteral("title"), title},
         {QStringLiteral("parentTaskId"), parentId}},
        [this](bool ok, const QJsonObject &payload, const QString &error) {
            m_organizationTasksStatus->setText(
                ok ? QStringLiteral(
                         "Follow-up created with the same assignee.")
                   : QStringLiteral("Follow-up creation failed: %1")
                         .arg(error));
            if (ok)
                openCreatedOrganizationTask(
                    payload.value(QStringLiteral("task")).toObject());
        });
}

void MainWindow::moveOrganizationTaskToIssue()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString taskId = taskText(task, QStringLiteral("id"));
    if (taskId.isEmpty() || !m_organizationTasksCanManage ||
        m_organizationTaskIssueMoveInFlight)
        return;

    const QString status = taskText(task, QStringLiteral("status"));
    if (status == QLatin1String("active") ||
        task.value(QStringLiteral("completedAt")).toDouble() > 0 ||
        status == QLatin1String("done")) {
        flashMessage(QStringLiteral(
                         "Stop or reopen the task before moving it into an issue."),
                     true);
        return;
    }

    const QString repository = taskText(task, QStringLiteral("repository"));
    const QStringList repositoryParts =
        repository.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (repositoryParts.size() != 2) {
        QMessageBox::warning(
            this, QStringLiteral("Move task to issue"),
            QStringLiteral("This task needs an owner/repository before it can "
                           "be moved into an issue."));
        return;
    }

    int repoIndex = -1;
    for (int index = 0; index < m_repositories.size(); ++index) {
        const RepositoryRecord &repo = m_repositories.at(index);
        if (repo.owner.compare(repositoryParts.at(0), Qt::CaseInsensitive) == 0 &&
            repo.name.compare(repositoryParts.at(1), Qt::CaseInsensitive) == 0) {
            repoIndex = index;
            break;
        }
    }
    if (repoIndex < 0) {
        QMessageBox::warning(
            this, QStringLiteral("Move task to issue"),
            QStringLiteral("Open the task's repository (%1) on this desktop "
                           "before moving the task into an issue.")
                .arg(repository));
        return;
    }

    const QString title = taskText(task, QStringLiteral("title"));
    if (title.isEmpty())
        return;
    const QString repoLabel = repositoryParts.join(QLatin1Char('/'));
    const QString question =
        QStringLiteral("Move \"%1\" into an issue in %2?\n\n"
                       "The task will be removed after the issue is filed.")
            .arg(title, repoLabel);
    if (QMessageBox::question(this, QStringLiteral("Move task to issue"),
                              question,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;

    m_organizationTaskIssueMoveInFlight = true;
    updateOrganizationTaskActions();

    // File first and delete second. If the catalog delete is rejected, the
    // issue remains safely filed and the original task remains visible for a
    // retry instead of losing either record.
    auto removeTask = [this, taskId](int issueNumber,
                                     const QString &issueNotice) {
        requestOrganizationTasks(
            QByteArrayLiteral("DELETE"),
            QStringLiteral("/api/tasks/") + taskId, {},
            [this, taskId, issueNumber, issueNotice](
                bool ok, const QJsonObject &data, const QString &error) {
                m_organizationTaskIssueMoveInFlight = false;
                if (ok)
                    cleanupOrganizationTaskWorkItems(
                        taskId,
                        localTaskWorkItems(
                            taskId,
                            data.value(QStringLiteral("workItems")).toArray()),
                        0);
                if (!ok) {
                    const QString failure =
                        QStringLiteral("%1 The task could not be removed: %2")
                            .arg(issueNotice, error);
                    if (m_organizationTasksStatus)
                        m_organizationTasksStatus->setText(failure);
                    flashMessage(failure, true);
                    updateOrganizationTaskActions();
                    return;
                }

                const QString moved =
                    QStringLiteral("%1 The task was removed from the "
                                   "organization catalog.")
                        .arg(issueNotice);
                if (m_organizationTasksStatus)
                    m_organizationTasksStatus->setText(moved);
                refreshOrganizationTasks();
                if (issueNumber > 0) {
                    showSection(0);
                    if (m_repoDetailTabs && m_repoDetailTabs->button(2))
                        m_repoDetailTabs->button(2)->click();
                    showIssue(issueNumber);
                }
            });
    };

    if (repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    showSection(0);
    if (m_repoDetailTabs && m_repoDetailTabs->button(2))
        m_repoDetailTabs->button(2)->click();
    if (m_issuesRepoCombo) {
        const int combo = m_issuesRepoCombo->findData(repoIndex);
        if (combo >= 0)
            m_issuesRepoCombo->setCurrentIndex(combo);
    }
    if (issuesRepoIndex() != repoIndex) {
        m_organizationTaskIssueMoveInFlight = false;
        const QString failure = QStringLiteral(
            "Could not select the issue repository for this task.");
        if (m_organizationTasksStatus)
            m_organizationTasksStatus->setText(failure);
        flashMessage(failure, true);
        updateOrganizationTaskActions();
        return;
    }

    const QString body = organizationTaskIssueBody(task);
    const int priority = task.value(QStringLiteral("priority")).toInt();
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        if (m_organizationTasksStatus)
            m_organizationTasksStatus->setText(
                QStringLiteral("Sending the task to the repository issue inbox…"));
        const bool started = submitNewIssueToInbox(
            title, body, {}, QString(), priority, {}, {}, {},
            [this, removeTask, repoLabel](bool ok, const QString &error) {
                if (!ok) {
                    m_organizationTaskIssueMoveInFlight = false;
                    const QString failure =
                        QStringLiteral("Could not file the task as an issue: %1")
                            .arg(error);
                    if (m_organizationTasksStatus)
                        m_organizationTasksStatus->setText(failure);
                    flashMessage(failure, true);
                    updateOrganizationTaskActions();
                    return;
                }
                removeTask(
                    -1,
                    QStringLiteral("Issue sent to %1's repository inbox.")
                        .arg(repoLabel));
            });
        if (!started) {
            m_organizationTaskIssueMoveInFlight = false;
            const QString failure =
                QStringLiteral("Could not reach the repository issue inbox.");
            if (m_organizationTasksStatus)
                m_organizationTasksStatus->setText(failure);
            flashMessage(failure, true);
            updateOrganizationTaskActions();
        }
        return;
    }

    QString error;
    Issue created;
    const int number = store.createIssue(title, body, {}, QString(), priority,
                                         {}, {}, &error, &created);
    if (number < 0) {
        m_organizationTaskIssueMoveInFlight = false;
        const QString failure =
            QStringLiteral("Could not create the issue: %1").arg(error);
        if (m_organizationTasksStatus)
            m_organizationTasksStatus->setText(failure);
        flashMessage(failure, true);
        updateOrganizationTaskActions();
        return;
    }

    appendCreatedIssue(store, created);
    propagateRepoUpdate(repoIndex);
    if (repoIndex == m_repoDetailIndex)
        triggerTeammatesForEvent(
            QStringLiteral("new-issues"),
            QStringLiteral("Issue #%1 · %2").arg(number).arg(title));
    removeTask(number,
               QStringLiteral("Issue #%1 created in %2.")
                   .arg(number)
                   .arg(repoLabel));
}

void MainWindow::editOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    if (task.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Edit organization task"));
    auto *layout = new QFormLayout(&dialog);
    auto *title = new QLineEdit(taskText(task, QStringLiteral("title")));
    title->setMaxLength(160);
    auto *details = new QPlainTextEdit(
        taskText(task, QStringLiteral("details")));
    details->setMaximumHeight(120);
    auto *repository = new QLineEdit(
        taskText(task, QStringLiteral("repository")));
    auto *assignee = new QComboBox;
    assignee->addItem(QStringLiteral("Bot"), QStringLiteral("agent"));
    assignee->addItem(QStringLiteral("Unassigned"),
                      QStringLiteral("unassigned"));
    for (const QString &member : std::as_const(m_organizationTaskMembers))
        assignee->addItem(member, QStringLiteral("user:") + member);
    QString current = taskText(task, QStringLiteral("assigneeKind"));
    if (current == QLatin1String("user"))
        current = QStringLiteral("user:") +
                  taskText(task, QStringLiteral("assignee"));
    const int assignmentIndex = assignee->findData(current);
    if (assignmentIndex >= 0)
        assignee->setCurrentIndex(assignmentIndex);
    auto *priority = new QSpinBox;
    priority->setRange(1, 99);
    priority->setValue(task.value(QStringLiteral("priority")).toInt(50));
    layout->addRow(QStringLiteral("Title"), title);
    layout->addRow(QStringLiteral("Details"), details);
    layout->addRow(QStringLiteral("Repository"), repository);
    layout->addRow(QStringLiteral("Assignee"), assignee);
    layout->addRow(QStringLiteral("Priority"), priority);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted || title->text().trimmed().isEmpty())
        return;
    const QString assignment = assignee->currentData().toString();
    const bool assignAgent = assignment == QLatin1String("agent");
    QJsonObject body{
        {QStringLiteral("title"), title->text().trimmed()},
        {QStringLiteral("details"), details->toPlainText().trimmed()},
        {QStringLiteral("repository"), repository->text().trimmed()},
        {QStringLiteral("priority"), priority->value()},
        {QStringLiteral("assigneeKind"),
         assignAgent ? QStringLiteral("agent")
                     : assignment == QLatin1String("unassigned")
                         ? QStringLiteral("unassigned")
                         : QStringLiteral("user")},
    };
    if (assignment.startsWith(QLatin1String("user:")))
        body.insert(QStringLiteral("assignee"), assignment.mid(5));
    const QString taskId = taskText(task, QStringLiteral("id"));
    const QString path = QStringLiteral("/api/tasks/") + taskId;

    // Show the edit the moment Save is pressed. The PATCH shares a serialized
    // ingress with catalog reads, so its answer can take seconds; until then
    // the row used to keep its old values and the edit looked unsaved.
    QJsonObject optimistic = task;
    for (auto it = body.constBegin(); it != body.constEnd(); ++it)
        optimistic.insert(it.key(), it.value());
    if (!assignment.startsWith(QLatin1String("user:")))
        optimistic.insert(QStringLiteral("assignee"), QString());
    ++m_organizationTaskEditsInFlight[taskId];
    m_organizationTaskLocalEdits.insert(taskId, optimistic);
    replaceOrganizationTask(optimistic);
    m_organizationTasksStatus->setText(
        QString::fromUtf8("Saving task\xE2\x80\xA6"));

    requestOrganizationTasks(
        QByteArrayLiteral("PATCH"), path, body,
        [this, assignAgent, taskId, task](bool ok, const QJsonObject &payload,
                                          const QString &error) {
            bool updated = false;
            if (!settleOrganizationTaskEdit(taskId, task, ok, payload, error,
                                            &updated))
                return;
            const QJsonObject changed =
                payload.value(QStringLiteral("task")).toObject();
            if (assignAgent &&
                taskText(changed, QStringLiteral("agentSessionId")).isEmpty())
                queueOrganizationTaskAgent(changed);
            else if (!updated)
                refreshOrganizationTasks();
        });
}

bool MainWindow::settleOrganizationTaskEdit(const QString &taskId,
                                            const QJsonObject &previous,
                                            bool ok,
                                            const QJsonObject &payload,
                                            const QString &error,
                                            bool *updated)
{
    const bool lastEdit = --m_organizationTaskEditsInFlight[taskId] <= 0;
    if (lastEdit)
        m_organizationTaskEditsInFlight.remove(taskId);
    if (!ok) {
        m_organizationTasksStatus->setText(
            QStringLiteral("Task update failed: %1").arg(error));
        if (lastEdit) {
            m_organizationTaskLocalEdits.remove(taskId);
            replaceOrganizationTask(previous);
        }
        return false;
    }
    const QJsonObject changed =
        payload.value(QStringLiteral("task")).toObject();
    m_organizationTasksStatus->setText(QStringLiteral("Task updated."));
    // PATCH returns the fully projected, persisted task. Keep it as
    // the local edit too: a catalog read issued before this write can
    // still land afterwards, and must not put the old values back.
    *updated = !lastEdit;
    if (lastEdit && !changed.isEmpty()) {
        m_organizationTaskLocalEdits.insert(taskId, changed);
        *updated = replaceOrganizationTask(changed);
    } else if (lastEdit) {
        m_organizationTaskLocalEdits.remove(taskId);
    }
    return true;
}

bool MainWindow::organizationTaskCheckboxToggleable(int index) const
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    return !task.isEmpty() && m_organizationTasksCanManage &&
           !organizationTaskIsClosed(task) &&
           taskText(task, QStringLiteral("status")) !=
               QLatin1String("active") &&
           !taskCheckboxToggle(task, index).text.isNull();
}

void MainWindow::toggleOrganizationTaskCheckbox(int index)
{
    if (!organizationTaskCheckboxToggleable(index))
        return;
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const TaskCheckboxToggle toggle = taskCheckboxToggle(task, index);
    const QString taskId = taskText(task, QStringLiteral("id"));
    QJsonObject optimistic = task;
    if (toggle.field == QLatin1String("howToTest")) {
        QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
        qa.insert(toggle.field, toggle.text);
        optimistic.insert(QStringLiteral("qa"), qa);
    } else {
        optimistic.insert(toggle.field, toggle.text);
    }
    ++m_organizationTaskEditsInFlight[taskId];
    m_organizationTaskLocalEdits.insert(taskId, optimistic);
    replaceOrganizationTask(optimistic);
    m_organizationTasksStatus->setText(
        QString::fromUtf8("Saving task\xE2\x80\xA6"));
    requestOrganizationTasks(
        QByteArrayLiteral("PATCH"), QStringLiteral("/api/tasks/") + taskId,
        {{toggle.field, toggle.text}},
        [this, taskId, task](bool ok, const QJsonObject &payload,
                             const QString &error) {
            bool updated = false;
            if (settleOrganizationTaskEdit(taskId, task, ok, payload, error,
                                           &updated) &&
                !updated)
                refreshOrganizationTasks();
        });
}

void MainWindow::assignOrganizationTaskToAgent()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    if (task.isEmpty())
        return;
    if (taskText(task, QStringLiteral("assigneeKind")) ==
        QLatin1String("agent")) {
        queueOrganizationTaskAgent(task);
        return;
    }
    const QString path = QStringLiteral("/api/tasks/") +
                         taskText(task, QStringLiteral("id"));
    requestOrganizationTasks(
        QByteArrayLiteral("PATCH"), path,
        {{QStringLiteral("assigneeKind"), QStringLiteral("agent")},
         {QStringLiteral("repository"),
          taskText(task, QStringLiteral("repository"))}},
        [this](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok) {
                m_organizationTasksStatus->setText(
                    QStringLiteral("Agent assignment failed: %1").arg(error));
                return;
            }
            queueOrganizationTaskAgent(
                payload.value(QStringLiteral("task")).toObject());
        });
}

void MainWindow::queueOrganizationTaskAgent(const QJsonObject &task)
{
    const QStringList repository =
        taskText(task, QStringLiteral("repository"))
            .split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (repository.size() != 2) {
        m_organizationTasksStatus->setText(
            QStringLiteral("Agent assignment needs an owner/repository."));
        return;
    }
    const QString id = taskText(task, QStringLiteral("id"));
    const QString title = taskText(task, QStringLiteral("title"));
    const QString details = taskText(task, QStringLiteral("details"));
    const QString prompt =
        QStringLiteral("[task:%1] %2%3")
            .arg(id, title,
                 details.isEmpty() ? QString()
                                   : QStringLiteral("\n\n") + details);
    const QString endpoint =
        QStringLiteral("/api/orgs/%1/repos/%2/agent-bots")
            .arg(repository.at(0).toLower(), repository.at(1));
    m_organizationTasksStatus->setText(
        QString::fromUtf8("Assigning task to an eligible agent node\xE2\x80\xA6"));
    requestOrganizationTasks(
        QByteArrayLiteral("POST"), endpoint,
        {{QStringLiteral("provider"), QStringLiteral("agent")},
         {QStringLiteral("prompt"), prompt.left(8000)},
         {QStringLiteral("title"), title},
         {QStringLiteral("taskKey"), QStringLiteral("task:") + id}},
        [this, id](bool ok, const QJsonObject &payload,
                   const QString &error) {
            if (!ok) {
                m_organizationTasksStatus->setText(
                    QStringLiteral(
                        "Task is assigned to the agent queue, but dispatch "
                        "failed: %1")
                        .arg(error));
                refreshOrganizationTasks();
                return;
            }
            const QString sessionId =
                payload.value(QStringLiteral("session"))
                    .toObject()
                    .value(QStringLiteral("id"))
                    .toString();
            if (sessionId.isEmpty()) {
                m_organizationTasksStatus->setText(
                    QStringLiteral("Agent dispatch returned no session id."));
                refreshOrganizationTasks();
                return;
            }
            requestOrganizationTasks(
                QByteArrayLiteral("PATCH"),
                QStringLiteral("/api/tasks/") + id,
                {{QStringLiteral("agentSessionId"), sessionId}},
                [this](bool linked, const QJsonObject &,
                       const QString &linkError) {
                    m_organizationTasksStatus->setText(
                        linked
                            ? QStringLiteral(
                                  "Agent session queued and linked to the task.")
                            : QStringLiteral(
                                  "Agent queued, but linking its session failed: "
                                  "%1")
                                  .arg(linkError));
                    refreshOrganizationTasks();
                });
        });
}

void MainWindow::runOrganizationTaskAction(const QString &action)
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return;
    QJsonObject body;
    if (action == QLatin1String("complete")) {
        bool accepted = false;
        const QString note = QInputDialog::getMultiLineText(
            this, QStringLiteral("Complete organization task"),
            QStringLiteral("Completion note and evidence"),
            taskText(task, QStringLiteral("completionNote")), &accepted);
        if (!accepted)
            return;
        body.insert(QStringLiteral("completionNote"), note.trimmed());
    } else if (action == QLatin1String("qa")) {
        const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
        bool accepted = false;
        const QString howToTest = QInputDialog::getMultiLineText(
            this, QStringLiteral("Send organization task to QA"),
            QStringLiteral("Exact QA steps"),
            taskText(qa, QStringLiteral("howToTest")), &accepted);
        if (!accepted)
            return;
        body.insert(QStringLiteral("howToTest"), howToTest.trimmed());
    }
    requestOrganizationTasks(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/tasks/%1/%2").arg(id, action), body,
        [this, action](bool ok, const QJsonObject &, const QString &error) {
            m_organizationTasksStatus->setText(
                ok ? QStringLiteral("Task action completed: %1.").arg(action)
                   : QStringLiteral("Task action failed: %1").arg(error));
            if (ok)
                refreshOrganizationTasks();
        });
}

void MainWindow::commentOnOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return;
    bool accepted = false;
    const QString body =
        QInputDialog::getMultiLineText(
            this, QStringLiteral("Comment on organization task"),
            QStringLiteral(
                "Comment on \u201C%1\u201D. Type @name to ping someone.")
                .arg(taskText(task, QStringLiteral("title"))),
            QString(), &accepted)
            .trimmed();
    if (!accepted || body.isEmpty())
        return;
    QJsonObject payload;
    payload.insert(QStringLiteral("body"), body);
    const bool hadHistory = m_organizationTaskHistory.contains(id);
    const QJsonArray before = m_organizationTaskHistory.value(id);
    const qint64 beforeAt = m_organizationTaskHistoryAt.value(id);
    QJsonArray optimistic = before;
    optimistic.append(QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("replied")},
        {QStringLiteral("actor"), m_organizationTaskActor.trimmed().toLower()},
        {QStringLiteral("summary"), body},
        {QStringLiteral("body"), body},
        {QStringLiteral("pending"), true},
        {QStringLiteral("createdAt"),
         double(QDateTime::currentMSecsSinceEpoch())},
    });
    m_organizationTaskHistory.insert(id, optimistic);
    m_organizationTaskHistoryAt.insert(id, QDateTime::currentMSecsSinceEpoch());
    m_organizationTasksStatus->setText(QStringLiteral("Posting comment\u2026"));
    renderOrganizationTaskDetail();
    requestOrganizationTasks(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/tasks/%1/replies").arg(id), payload,
        [this, id, body, hadHistory, before, beforeAt](
            bool ok, const QJsonObject &response, const QString &error) {
            const auto redraw = qScopeGuard([this, id] {
                const QJsonObject selected = selectedOrganizationTask(
                    m_organizationTasksTable, m_organizationTasks);
                if (taskText(selected, QStringLiteral("id")) == id)
                    renderOrganizationTaskDetail();
            });
            if (!ok) {
                if (hadHistory) {
                    m_organizationTaskHistory.insert(id, before);
                    m_organizationTaskHistoryAt.insert(id, beforeAt);
                } else {
                    m_organizationTaskHistory.remove(id);
                    m_organizationTaskHistoryAt.remove(id);
                }
                m_organizationTasksStatus->setText(
                    QStringLiteral("Comment failed: %1").arg(error));
                return;
            }
            const QJsonObject task =
                response.value(QStringLiteral("task")).toObject();
            if (task.contains(QStringLiteral("history"))) {
                m_organizationTaskHistory.insert(
                    id, taskHistoryWithReplies(
                            task.value(QStringLiteral("history")).toArray(),
                            task.value(QStringLiteral("replies")).toArray()));
                m_organizationTaskHistoryAt.insert(
                    id, QDateTime::currentMSecsSinceEpoch());
            } else {
                m_organizationTaskHistory.remove(id);
                m_organizationTaskHistoryAt.remove(id);
            }
            const QStringList pinged = taskMentionedNames(body, m_organizationTaskActor);
            m_organizationTasksStatus->setText(
                pinged.isEmpty()
                    ? QStringLiteral("Comment posted.")
                    : QStringLiteral("Comment posted. Pinged @%1.")
                          .arg(pinged.join(QStringLiteral(", @"))));
        });
}

void MainWindow::checkInOnOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return;
    // Label order matches the relay's CHECKIN_STATES so the index maps
    // straight onto the value it accepts.
    const QStringList labels{QStringLiteral("Going well"),
                             QStringLiteral("Blocked"),
                             QStringLiteral("Needs help")};
    const QStringList states{QStringLiteral("going_well"),
                             QStringLiteral("blocked"),
                             QStringLiteral("needs_help")};
    bool accepted = false;
    const QString label = QInputDialog::getItem(
        this, QStringLiteral("Check in on organization task"),
        QStringLiteral("How is \u201C%1\u201D going?")
            .arg(taskText(task, QStringLiteral("title"))),
        labels, 0, false, &accepted);
    const int index = labels.indexOf(label);
    if (!accepted || index < 0)
        return;
    const QString note =
        QInputDialog::getMultiLineText(
            this, QStringLiteral("Check in on organization task"),
            QStringLiteral("Optional note. Type @name to ping someone."),
            QString(), &accepted)
            .trimmed();
    if (!accepted)
        return;
    QJsonObject payload;
    payload.insert(QStringLiteral("state"), states.at(index));
    if (!note.isEmpty())
        payload.insert(QStringLiteral("note"), note);
    requestOrganizationTasks(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/tasks/%1/checkin").arg(id), payload,
        [this, id, label, note](bool ok, const QJsonObject &,
                                const QString &error) {
            if (!ok) {
                m_organizationTasksStatus->setText(
                    QStringLiteral("Check-in failed: %1").arg(error));
                return;
            }
            m_organizationTaskHistory.remove(id);
            const QStringList pinged = taskMentionedNames(note, m_organizationTaskActor);
            m_organizationTasksStatus->setText(
                pinged.isEmpty()
                    ? QStringLiteral("Checked in: %1. Your team was pinged.")
                          .arg(label.toLower())
                    : QStringLiteral("Checked in: %1. Pinged @%2.")
                          .arg(label.toLower(),
                               pinged.join(QStringLiteral(", @"))));
            refreshOrganizationTasks();
            renderOrganizationTaskDetail();
        });
}

void MainWindow::remindOrganizationTaskAssignee()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    const QString assignee = taskText(task, QStringLiteral("assignee"));
    if (id.isEmpty() || assignee.isEmpty())
        return;
    bool accepted = false;
    const QString note =
        QInputDialog::getText(
            this, QStringLiteral("Remind assignee"),
            QStringLiteral("Ping @%1 to check in on \u201C%2\u201D.\n"
                           "Optional note:")
                .arg(assignee, taskText(task, QStringLiteral("title"))),
            QLineEdit::Normal, QString(), &accepted)
            .trimmed();
    if (!accepted)
        return;
    QJsonObject payload;
    if (!note.isEmpty())
        payload.insert(QStringLiteral("note"), note.left(500));
    requestOrganizationTasks(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/tasks/%1/remind").arg(id), payload,
        [this, assignee](bool ok, const QJsonObject &data,
                         const QString &error) {
            if (!ok) {
                m_organizationTasksStatus->setText(
                    data.value(QStringLiteral("error")).toString() ==
                            QLatin1String("reminder_too_soon")
                        ? QStringLiteral("@%1 was reminded less than 10 "
                                         "minutes ago. Try again later.")
                              .arg(assignee)
                        : QStringLiteral("Reminder failed: %1").arg(error));
                return;
            }
            m_organizationTasksStatus->setText(
                data.value(QStringLiteral("reminderError"))
                        .toString()
                        .isEmpty()
                    ? QStringLiteral("Reminder sent. @%1 was pinged to check "
                                     "in.")
                          .arg(assignee)
                    : QStringLiteral("The reminder was saved, but @%1 could "
                                     "not be pinged.")
                          .arg(assignee));
            refreshOrganizationTasks();
            renderOrganizationTaskDetail();
        });
}

void MainWindow::deleteOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return;
    const QList<forkmesh::taskwork::WorkItem> items = localTaskWorkItems(
        id, task.value(QStringLiteral("workItems")).toArray());
    if (items.isEmpty()) {
        sendOrganizationTaskDelete(id, 0);
        return;
    }
    if (m_organizationTasksStatus)
        m_organizationTasksStatus->setText(QString::fromUtf8(
            "Checking this task's worktrees\xE2\x80\xA6"));
    const QString title = taskText(task, QStringLiteral("title"));
    inspectOrganizationTaskWorkItems(
        items,
        [this, id, title](
            const QList<forkmesh::taskwork::Inspection> &inspections) {
            if (m_organizationTasksStatus)
                m_organizationTasksStatus->clear();
            QStringList losses;
            QStringList removable;
            for (const forkmesh::taskwork::Inspection &inspection :
                 inspections) {
                const QString where =
                    inspection.worktreePath.isEmpty()
                        ? inspection.item.branch
                        : QStringLiteral("%1 (%2)").arg(
                              inspection.item.branch, inspection.worktreePath);
                if (inspection.plan == forkmesh::taskwork::Plan::Confirm)
                    losses.append(where + QLatin1Char('\n') +
                                  forkmesh::taskwork::lossSummary(inspection));
                else if (inspection.plan == forkmesh::taskwork::Plan::Remove)
                    removable.append(where);
            }
            if (losses.isEmpty()) {
                sendOrganizationTaskDelete(id, 1);
                return;
            }
            QMessageBox box(QMessageBox::Warning, QStringLiteral("Delete task"),
                            QStringLiteral("Delete \"%1\" and its work on this "
                                           "desktop?")
                                .arg(title),
                            QMessageBox::NoButton, this);
            QString detail =
                QStringLiteral("These worktrees and branches hold work that "
                               "is not merged into the default branch:\n\n") +
                losses.join(QStringLiteral("\n\n")) +
                QStringLiteral("\n\nDeleting them first saves a backup at "
                               "refs/forkmesh/trash/tasks/%1/<branch>.")
                    .arg(id);
            if (!removable.isEmpty())
                detail += QStringLiteral("\n\nClean, merged work that is "
                                         "removed either way:\n  ") +
                          removable.join(QStringLiteral("\n  "));
            box.setInformativeText(detail);
            QPushButton *discard = box.addButton(
                QStringLiteral("Delete task and work"),
                QMessageBox::DestructiveRole);
            QPushButton *keep = box.addButton(
                QStringLiteral("Delete task, keep work"),
                QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Cancel);
            box.setDefaultButton(QMessageBox::Cancel);
            box.exec();
            if (box.clickedButton() == discard)
                sendOrganizationTaskDelete(id, 1);
            else if (box.clickedButton() == keep)
                sendOrganizationTaskDelete(id, 2);
        });
}

void MainWindow::sendOrganizationTaskDelete(const QString &taskId,
                                            int cleanupMode, bool force)
{
    const QJsonObject task = [this, &taskId] {
        for (const QJsonValue &value : std::as_const(m_organizationTasks))
            if (taskText(value.toObject(), QStringLiteral("id")) == taskId)
                return value.toObject();
        return QJsonObject();
    }();
    const QJsonArray listed = task.value(QStringLiteral("workItems")).toArray();
    requestOrganizationTasks(
        QByteArrayLiteral("DELETE"), QStringLiteral("/api/tasks/") + taskId,
        force ? QJsonObject{{QStringLiteral("force"), true}} : QJsonObject(),
        [this, taskId, listed, cleanupMode, force](
            bool ok, const QJsonObject &data, const QString &error) {
            const bool alreadyGone =
                !ok && data.value(QStringLiteral("error")).toString()
                           == QLatin1String("task_not_found");
            if (!ok && !alreadyGone
                && data.contains(kOrganizationTaskBackoffKey)
                && g_organizationTaskDeleteRetries.value(taskId)
                       < kOrganizationTaskDeleteMaxRetries) {
                g_organizationTaskDeleteRetries[taskId] += 1;
                const qint64 waitMs = qBound(
                    qint64(1000),
                    qint64(data.value(kOrganizationTaskBackoffKey).toDouble()) + 500,
                    qint64(10 * 60 * 1000));
                m_organizationTasksStatus->setText(
                    QStringLiteral("The relay is backing off; deleting the task "
                                   "again in %1 s.")
                        .arg((waitMs + 999) / 1000));
                QTimer::singleShot(
                    int(waitMs), this, [this, taskId, cleanupMode, force] {
                        sendOrganizationTaskDelete(taskId, cleanupMode, force);
                    });
                return;
            }
            g_organizationTaskDeleteRetries.remove(taskId);
            m_organizationTasksStatus->setText(
                ok || alreadyGone
                    ? QStringLiteral("Task moved to the trash; restore it "
                                     "from the web Tasks page within 30 days.")
                    : QStringLiteral("Task deletion failed: %1").arg(error));
            if (ok || alreadyGone) {
                QJsonArray items = listed;
                for (const QJsonValue &value :
                     data.value(QStringLiteral("workItems")).toArray())
                    items.append(value);
                cleanupOrganizationTaskWorkItems(
                    taskId, localTaskWorkItems(taskId, items), cleanupMode);
                refreshOrganizationTasks();
                return;
            }
            const QString blocked = taskBlockedText(data);
            if (!force && !blocked.isEmpty() &&
                data.value(QStringLiteral("error")).toString() ==
                    QLatin1String("task_delete_blocked")) {
                if (!data.value(QStringLiteral("canForce")).toBool(true)) {
                    QMessageBox::warning(
                        this, QStringLiteral("Delete task"),
                        QStringLiteral("This task is busy: %1. Only an "
                                       "organization owner can move it to the "
                                       "trash anyway.")
                            .arg(blocked));
                    return;
                }
                if (QMessageBox::question(
                        this, QStringLiteral("Delete task"),
                        QStringLiteral("This task is busy: %1.\n\nMove it to "
                                       "the trash anyway? It can be restored "
                                       "from the web Tasks page for 30 days.")
                            .arg(blocked),
                        QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel) == QMessageBox::Yes)
                    sendOrganizationTaskDelete(taskId, cleanupMode, true);
                return;
            }
            QMessageBox::warning(this, QStringLiteral("Delete task"), error);
        },
        {404});
}

// code-health: allow(git-in-loop) the repository scan is inside the QtConcurrent::run worker
void MainWindow::attachExistingWorktreeToOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString taskId = taskText(task, QStringLiteral("id"));
    if (taskId.isEmpty())
        return;
    int preferred = 0;
    const QStringList repositories = taskWorkItemRepositories(
        taskText(task, QStringLiteral("repository")), &preferred);
    QString start = QDir::homePath();
    if (!repositories.isEmpty()) {
        const QString nested =
            QDir(repositories.first()).filePath(QStringLiteral(".worktrees"));
        start = QDir(nested).exists() ? nested : repositories.first();
    }
    const QString picked = QFileDialog::getExistingDirectory(
        this, QStringLiteral("Attach a worktree to this task"), start);
    if (picked.isEmpty())
        return;
    auto *watcher = new QFutureWatcher<QPair<QString, forkmesh::taskwork::WorktreeEntry>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, taskId, picked] {
                watcher->deleteLater();
                const auto [repoPath, entry] = watcher->result();
                const QString branch = entry.branch.startsWith(QLatin1String("refs/heads/"))
                                           ? entry.branch.mid(11)
                                           : QString();
                if (repoPath.isEmpty() || branch.isEmpty()) {
                    flashMessage(
                        repoPath.isEmpty()
                            ? QStringLiteral("%1 is not a linked worktree of a "
                                             "repository on this desktop.")
                                  .arg(picked)
                            : QStringLiteral("%1 has no branch checked out.").arg(picked),
                        true, QString(), forkmesh::kLocalOperationNoticeKind);
                    return;
                }
                QString repository;
                for (const RepositoryRecord &repo : std::as_const(m_repositories))
                    if (forkmesh::taskwork::samePath(repo.localPath, repoPath))
                        repository = repo.owner + QLatin1Char('/') + repo.name;
                attachOrganizationTaskWorkItem(
                    taskId,
                    forkmesh::taskwork::validRepository(repository) ? repository
                                                                    : QString(),
                    branch, entry.path);
                flashMessage(QStringLiteral("Attaching %1 to the task.").arg(branch),
                             false, QString(), forkmesh::kLocalOperationNoticeKind);
            });
    watcher->setFuture(QtConcurrent::run([repositories, picked] {
        for (const QString &repo : repositories) {
            forkmesh::taskwork::WorktreeEntry entry;
            if (forkmesh::taskwork::findLinkedWorktree(repo, picked, &entry))
                return qMakePair(repo, entry);
        }
        return qMakePair(QString(), forkmesh::taskwork::WorktreeEntry());
    }));
}

void MainWindow::deleteClosedOrganizationTasks()
{
    if (!m_organizationTasksCanManage ||
        m_organizationTaskClosedDeleteInFlight)
        return;

    QStringList taskIds;
    QStringList awaitingQaIds;
    for (const QJsonValue &value : std::as_const(m_organizationTasks)) {
        const QJsonObject task = value.toObject();
        if (!organizationTaskIsClosed(task))
            continue;
        const QString id = taskText(task, QStringLiteral("id"));
        if (id.isEmpty())
            continue;
        if (organizationTaskAwaitsQa(task))
            awaitingQaIds.append(id);
        else
            taskIds.append(id);
    }
    const int awaitingQa = awaitingQaIds.size();
    taskIds.append(awaitingQaIds);
    if (taskIds.isEmpty())
        return;

    const int total = taskIds.size();
    const QString kept =
        awaitingQa > 0
            ? QStringLiteral("\n\n%1 of them %2 still awaiting QA review. The "
                             "relay keeps %3 on the board unless an "
                             "organization owner moves %3 anyway.")
                  .arg(awaitingQa)
                  .arg(awaitingQa == 1 ? QStringLiteral("is")
                                       : QStringLiteral("are"))
                  .arg(awaitingQa == 1 ? QStringLiteral("it")
                                       : QStringLiteral("them"))
            : QString();
    const auto choice = QMessageBox::warning(
        this, QStringLiteral("Delete closed tasks"),
        QStringLiteral("Move %1 closed task%2 to the trash? They can be "
                       "restored from the web Tasks page for 30 days.%3")
            .arg(QString::number(total),
                 total == 1 ? QString() : QStringLiteral("s"), kept),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (choice != QMessageBox::Yes)
        return;

    m_organizationTaskClosedDeleteInFlight = true;
    m_organizationTaskClosedDeleteForce = false;
    m_organizationTaskClosedDeleteIds = taskIds;
    m_organizationTaskClosedDeleteTotal = total;
    m_organizationTaskClosedDeleteCount = 0;
    m_organizationTaskClosedDeleteRetries = 0;
    m_organizationTaskClosedDeleteKept = 0;
    m_organizationTaskClosedDeleteBusy.clear();
    m_organizationTaskClosedDeleteReasons.clear();
    updateOrganizationTaskActions();
    deleteNextClosedOrganizationTask();
}

void MainWindow::deleteNextClosedOrganizationTask()
{
    if (m_organizationTaskClosedDeleteIds.isEmpty()) {
        finishClosedOrganizationTaskDelete();
        return;
    }

    if (m_organizationTasksStatus)
        m_organizationTasksStatus->setText(
            QString::fromUtf8("Deleting closed tasks\xE2\x80\xA6 %1 of %2")
                .arg(m_organizationTaskClosedDeleteTotal -
                     m_organizationTaskClosedDeleteIds.size() + 1)
                .arg(m_organizationTaskClosedDeleteTotal));
    const QString id = m_organizationTaskClosedDeleteIds.takeFirst();
    requestOrganizationTasks(
        QByteArrayLiteral("DELETE"), QStringLiteral("/api/tasks/") + id,
        m_organizationTaskClosedDeleteForce
            ? QJsonObject{{QStringLiteral("force"), true}}
            : QJsonObject(),
        [this, id](bool ok, const QJsonObject &data, const QString &error) {
            const QString code = data.value(QStringLiteral("error")).toString();
            if (ok || code == QLatin1String("task_not_found")) {
                if (ok)
                    cleanupOrganizationTaskWorkItems(
                        id,
                        localTaskWorkItems(
                            id,
                            data.value(QStringLiteral("workItems")).toArray()),
                        0);
                for (int index = 0; index < m_organizationTasks.size(); ++index) {
                    if (taskText(m_organizationTasks.at(index).toObject(),
                                 QStringLiteral("id")) == id) {
                        m_organizationTasks.removeAt(index);
                        break;
                    }
                }
                m_organizationTaskClosedDeleteRetries = 0;
                ++m_organizationTaskClosedDeleteCount;
                deleteNextClosedOrganizationTask();
                return;
            }
            if (data.contains(kOrganizationTaskBackoffKey) &&
                m_organizationTaskClosedDeleteRetries <
                    kOrganizationTaskDeleteMaxRetries) {
                ++m_organizationTaskClosedDeleteRetries;
                m_organizationTaskClosedDeleteIds.prepend(id);
                const qint64 waitMs = qBound(
                    qint64(1000),
                    qint64(data.value(kOrganizationTaskBackoffKey).toDouble()) +
                        500,
                    qint64(10 * 60 * 1000));
                if (m_organizationTasksStatus)
                    m_organizationTasksStatus->setText(
                        QStringLiteral("The relay is backing off; deleting "
                                       "closed tasks again in %1 s.")
                            .arg((waitMs + 999) / 1000));
                QTimer::singleShot(int(waitMs), this, [this] {
                    deleteNextClosedOrganizationTask();
                });
                return;
            }
            m_organizationTaskClosedDeleteRetries = 0;
            if (code == QLatin1String("task_delete_blocked") ||
                code == QLatin1String("force_requires_owner")) {
                const QString blocked = taskBlockedText(data);
                if (!blocked.isEmpty() &&
                    !m_organizationTaskClosedDeleteReasons.contains(blocked))
                    m_organizationTaskClosedDeleteReasons.append(blocked);
                if (code == QLatin1String("task_delete_blocked") &&
                    data.value(QStringLiteral("canForce")).toBool(true))
                    m_organizationTaskClosedDeleteBusy.append(id);
                else
                    ++m_organizationTaskClosedDeleteKept;
                deleteNextClosedOrganizationTask();
                return;
            }

            const QString failure =
                QStringLiteral("Moved %1 of %2 closed tasks to the trash: %3")
                    .arg(m_organizationTaskClosedDeleteCount)
                    .arg(m_organizationTaskClosedDeleteTotal)
                    .arg(error);
            m_organizationTaskClosedDeleteInFlight = false;
            m_organizationTaskClosedDeleteIds.clear();
            m_organizationTaskClosedDeleteBusy.clear();
            m_organizationTaskClosedDeleteReasons.clear();
            m_organizationTaskClosedDeleteTotal = 0;
            m_organizationTaskClosedDeleteCount = 0;
            m_organizationTaskClosedDeleteKept = 0;
            if (m_organizationTasksStatus)
                m_organizationTasksStatus->setText(failure);
            renderOrganizationTaskRows();
            refreshOrganizationTasks();
            updateOrganizationTaskActions();
            QMessageBox::warning(this, QStringLiteral("Delete closed tasks"),
                                 failure);
        },
        {404, 409});
}

void MainWindow::finishClosedOrganizationTaskDelete()
{
    const QStringList busy = m_organizationTaskClosedDeleteBusy;
    const QString reasons =
        m_organizationTaskClosedDeleteReasons.join(QStringLiteral("; "));
    m_organizationTaskClosedDeleteBusy.clear();
    if (!busy.isEmpty() && !m_organizationTaskClosedDeleteForce) {
        if (m_organizationTasksStatus)
            m_organizationTasksStatus->clear();
        const auto choice = QMessageBox::question(
            this, QStringLiteral("Delete closed tasks"),
            QStringLiteral("%1 closed task%2 %3 busy: %4.\n\nMove %5 to the "
                           "trash anyway? %6 can be restored from the web "
                           "Tasks page for 30 days.")
                .arg(busy.size())
                .arg(busy.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(busy.size() == 1 ? QStringLiteral("is")
                                      : QStringLiteral("are"))
                .arg(reasons.isEmpty() ? QStringLiteral("the relay refused it")
                                       : reasons)
                .arg(busy.size() == 1 ? QStringLiteral("it")
                                      : QStringLiteral("them"))
                .arg(busy.size() == 1 ? QStringLiteral("It")
                                      : QStringLiteral("They")),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (choice == QMessageBox::Yes) {
            m_organizationTaskClosedDeleteForce = true;
            m_organizationTaskClosedDeleteIds = busy;
            m_organizationTaskClosedDeleteTotal =
                m_organizationTaskClosedDeleteCount + busy.size();
            m_organizationTaskClosedDeleteReasons.clear();
            deleteNextClosedOrganizationTask();
            return;
        }
    }

    const int moved = m_organizationTaskClosedDeleteCount;
    const int kept = m_organizationTaskClosedDeleteKept + busy.size();
    QString summary =
        QStringLiteral("Moved %1 closed task%2 to the trash.")
            .arg(moved)
            .arg(moved == 1 ? QString() : QStringLiteral("s"));
    if (kept > 0) {
        summary += QStringLiteral(" %1 stayed on the board").arg(kept);
        if (!reasons.isEmpty())
            summary += QStringLiteral(" (%1)").arg(reasons);
        if (m_organizationTaskClosedDeleteKept > 0)
            summary += QStringLiteral("; only an organization owner can move "
                                      "busy tasks to the trash");
        summary += QLatin1Char('.');
    }
    m_organizationTaskClosedDeleteInFlight = false;
    m_organizationTaskClosedDeleteForce = false;
    m_organizationTaskClosedDeleteTotal = 0;
    m_organizationTaskClosedDeleteCount = 0;
    m_organizationTaskClosedDeleteKept = 0;
    m_organizationTaskClosedDeleteReasons.clear();
    if (m_organizationTasksStatus)
        m_organizationTasksStatus->setText(summary);
    if (m_organizationTasksStatus && m_organizationTasksStatus->isVisible())
        fileMessage(summary, moved == 0 && kept > 0,
                    forkmesh::kLocalOperationNoticeKind);
    else
        flashMessage(summary, moved == 0 && kept > 0, QString(),
                     forkmesh::kLocalOperationNoticeKind);
    renderOrganizationTaskRows();
    if (m_organizationTasksLoading)
        m_organizationTaskEventPending = true;
    else
        refreshOrganizationTasks();
    updateOrganizationTaskActions();
}
