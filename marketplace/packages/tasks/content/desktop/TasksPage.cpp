#include "TasksPage.h"

#include "DesignTokens.h"
#include "DesktopUi.h"
#include "FlowLayout.h"
#include "OcticonIcons.h"
#include "TaskMarkdown.h"
#include "TaskWorkItems.h"
#include "Theme.h"
#include "VerticalIconButton.h"

#include <QAbstractTextDocumentLayout>
#include <QActionGroup>
#include <QBuffer>
#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScreen>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <memory>
#include <vector>
#include <cmath>

using namespace forkmesh::ui;

namespace forkmesh::tasks {
namespace {
constexpr int kOrganizationTaskPageSize = 100;
constexpr int kOrganizationTaskDeleteMaxRetries = 5;
constexpr int kOrganizationTaskLoadAheadRows = 10;
constexpr qint64 kOrganizationTaskHistoryStaleMs = 30 * 1000;

bool organizationTaskAwaitsQa(const QJsonObject &task)
{
    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
    return qa.value(QStringLiteral("requestedAt")).toDouble() > 0 &&
           qa.value(QStringLiteral("reviewedAt")).toDouble() <= 0;
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

QString taskCreatedAge(qint64 milliseconds)
{
    const QString age = formatShortRelativeTime(milliseconds / 1000);
    return age.isEmpty() ? QString::fromUtf8("\xE2\x80\x94") : age;
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
constexpr int kTaskAssigneeBadge = 17;
constexpr int kTaskAssigneeOverhang = 2;
constexpr int kTaskAgentBadge = 20;
constexpr int kTaskAgentFaceSource = 48;
constexpr int kTaskFullAvatar = 40;
constexpr int kTaskCompactAvatar = 24;
constexpr int kTaskRowThumbSide = 34;
constexpr int kTaskRowThumbGap = 4;
constexpr int kTaskRowThumbMax = 3;
constexpr int kTaskAgentOrbit = 34;
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
        return left.isEmpty() ? 1 : -1;
    const int compared = left.compare(right, Qt::CaseInsensitive);
    return descending ? -compared : compared;
}

int compareTaskNumber(qint64 left, qint64 right, bool descending,
                      bool missingIsZero = true)
{
    if (missingIsZero && (left <= 0) != (right <= 0))
        return left <= 0 ? 1 : -1;
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
    return 3;
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
        compared = compareTaskNumber(
            taskSortTimestamp(left, QStringLiteral("createdAt")),
            taskSortTimestamp(right, QStringLiteral("createdAt")), true);
    }
    if (compared != 0)
        return compared < 0;

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

struct TaskRepositoryLogo {
    QString path;
    qint64 modifiedMs = 0;
};

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
        reader.setScaledSize(native.scaled(px, px, Qt::KeepAspectRatio));
    }
    const QImage image = reader.read();
    QPixmap mark;
    if (!image.isNull()) {
        QPixmap scaled = QPixmap::fromImage(image);
        if (scaled.width() > px || scaled.height() > px)
            scaled = scaled.scaled(px, px, Qt::KeepAspectRatio,
                                   Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(dpr);
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

void publishTaskRepositoryLogos(const QJsonArray &repositories)
{
    QHash<QString, TaskRepositoryLogo> logos;
    QSet<QString> ambiguous;
    for (const QJsonValue &value : repositories) {
        const QJsonObject repo = value.toObject();
        const QString path = repo.value(QStringLiteral("logo")).toString();
        const QString owner = repo.value(QStringLiteral("owner")).toString();
        const QString name = repo.value(QStringLiteral("name")).toString();
        if (path.isEmpty())
            continue;
        TaskRepositoryLogo logo;
        logo.path = path;
        logo.modifiedMs = QFileInfo(path).lastModified().toMSecsSinceEpoch();
        logos.insert((owner + QLatin1Char('/') + name).toLower(), logo);
        const QString bare = name.toLower();
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

struct TaskGlyph {
    QString icon;
    QColor color;
    QString avatarSeed;
    qreal avatarRadius = 0.5;
    TaskRepositoryLogo logo;
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
        const QString createdAge =
            taskCreatedAge(index.data(kTaskCreatedAtRole).toLongLong());
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

        const int avatarD =
            qMax(24, qMin(kTaskFullAvatar, option.rect.height() - 12));
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

        const int left = avatar.right() + 4;
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

        if (x < right) {
            drawGlyph(taskStatusGlyph(statusKey, dark));
            x += kTaskMetaGlyphSize + kTaskMetaGap;
        }
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
        if (x < right) {
            painter->setPen(muted);
            painter->drawText(QRect(x, metaLine.top(), right - x,
                                    metaLine.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, createdAge);
        }
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

    static void paintCompactAvatar(QPainter *painter, const QModelIndex &index,
                                   const QRect &avatar, const QColor &muted,
                                   const QColor &rowBg, bool selected)
    {
        const QString assigneeKind =
            index.data(kTaskAssigneeKindRole).toString();
        const QString assignee = index.data(kTaskAssigneeRole).toString();
        const QPixmap agentFace =
            index.data(kTaskAgentFaceRole).value<QPixmap>();
        if (!agentFace.isNull()) {
            painter->drawPixmap(avatar, agentFace);
            if (index.data(kTaskAgentWorkingRole).toBool()) {
                const int orbit = avatar.width() + 10;
                painter->drawPixmap(
                    QPoint(avatar.center().x() - orbit / 2,
                           avatar.center().y() - orbit / 2),
                    agentRunningFireballPixmap(
                        orbit, index.data(kTaskAgentAngleRole).toReal()));
            }
            return;
        }
        if (assigneeKind == QLatin1String("agent")) {
            const int glyph = avatar.width() - 6;
            painter->drawPixmap(
                avatar.topLeft() + QPoint(3, 3),
                tintedOcticonPixmap(
                    QStringLiteral("sparkle"),
                    selected ? QColor("#ffffff")
                             : Theme::nextOr(Theme::Role::FgSecondary,
                                             QColor(Theme::kGenie)),
                    glyph));
            return;
        }
        if (!assignee.isEmpty() &&
            assigneeKind != QLatin1String("unassigned")) {
            const QPixmap face =
                taskPersonFace(index.data(kTaskAssigneePictureRole), assignee,
                               avatar.width(), 0.5);
            if (!face.isNull()) {
                painter->drawPixmap(avatar, face);
                return;
            }
        }
        painter->setBrush(rowBg);
        painter->setPen(QPen(muted, 1, Qt::DashLine));
        painter->drawEllipse(QRectF(avatar).adjusted(0.5, 0.5, -0.5, -0.5));
        const int glyph = avatar.width() - 10;
        painter->drawPixmap(
            avatar.topLeft() + QPoint(5, 5),
            tintedOcticonPixmap(QStringLiteral("person"), muted, glyph));
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

        const QColor rowBg =
            selected ? Theme::nextOr(Theme::Role::SurfaceActive,
                                     QColor(dark ? "#1f6feb" : "#0969da"))
            : option.state.testFlag(QStyle::State_MouseOver)
                ? Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa"))
                : option.palette.base().color();
        const int avatarD = qMin(kTaskCompactAvatar, rect.height() - 6);
        const QRect avatar(priorityLeft + kTaskPriorityWidth + 8,
                           rect.top() + (rect.height() - avatarD) / 2, avatarD,
                           avatarD);
        paintCompactAvatar(painter, index, avatar, muted, rowBg, selected);

        const int x0 = avatar.right() + 10;
        const int glyphTop =
            rect.top() + (rect.height() - kTaskMetaGlyphSize) / 2;
        const int minTitle = 120;
        const auto fits = [&](int right, int width) {
            return right - width >= x0 + minTitle;
        };
        const auto drawMark = [&](const TaskGlyph &glyph, int x) {
            if (glyph.icon.isEmpty())
                return;
            painter->drawPixmap(
                x, glyphTop,
                tintedOcticonPixmap(glyph.icon,
                                    selected ? QColor("#ffffff") : glyph.color,
                                    kTaskMetaGlyphSize));
        };

        int right = rect.right() - 8;
        if (index.data(kTaskAttachmentCountRole).toInt() > 0 &&
            fits(right, kTaskMetaGlyphSize + kTaskMetaGap)) {
            drawMark({QStringLiteral("paperclip"), muted, {}, 0.5},
                     right - kTaskMetaGlyphSize);
            right -= kTaskMetaGlyphSize + kTaskMetaGap;
        }
        const int ageWidth = metrics.horizontalAdvance(QStringLiteral("00mo"));
        if (fits(right, ageWidth)) {
            painter->setPen(muted);
            painter->drawText(
                QRect(right - ageWidth, rect.top(), ageWidth, rect.height()),
                Qt::AlignVCenter | Qt::AlignRight,
                taskCreatedAge(index.data(kTaskCreatedAtRole).toLongLong()));
            right -= ageWidth + 10;
        }
        const QString statusKey = index.data(kTaskStatusRole).toString();
        if (fits(right, kTaskMetaGlyphSize)) {
            drawMark(taskStatusGlyph(statusKey, dark),
                     right - kTaskMetaGlyphSize);
            right -= kTaskMetaGlyphSize + kTaskMetaGap;
        }
        const QString branch = index.data(kTaskBranchRole).toString();
        if (!branch.isEmpty()) {
            const int full = kTaskMetaGlyphSize + 4 + 18 * charW;
            const int width = fits(right, full) ? full : kTaskMetaGlyphSize;
            if (fits(right, width)) {
                const int x = right - width;
                drawMark({QStringLiteral("git-branch"), muted, {}, 0.5}, x);
                const int textX = x + kTaskMetaGlyphSize + 4;
                if (right > textX) {
                    painter->setPen(muted);
                    painter->drawText(
                        QRect(textX, rect.top(), right - textX, rect.height()),
                        Qt::AlignVCenter | Qt::AlignLeft,
                        metrics.elidedText(branch, Qt::ElideMiddle,
                                           right - textX));
                }
                right -= width + kTaskMetaGap;
            }
        }

        const QRect titleRect(x0, rect.top(), qMax(0, right - x0),
                              rect.height());
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

QString taskNextCaptionHtml(const QString &caption)
{
    return QStringLiteral("<p style='margin:18px 0 8px 0;font-size:11px;"
                          "font-weight:600;color:%1'>%2</p>")
        .arg(Theme::hex(Theme::Role::FgSubtle), caption.toHtmlEscaped());
}

QString taskHistoryNextHtml(const QJsonArray &history)
{
    QString html = taskNextCaptionHtml(QStringLiteral("ACTIVITY"));
    const QString muted = Theme::hex(Theme::Role::FgMuted);
    const QString subtle = Theme::hex(Theme::Role::FgSubtle);
    const QString fg = Theme::hex(Theme::Role::Fg);
    if (history.isEmpty())
        return html + QStringLiteral("<p style='margin:0;color:%1'>No activity "
                                     "recorded on this task yet.</p>")
                          .arg(muted);
    html += QStringLiteral("<table cellspacing='0' width='100%'>");
    for (const QJsonValue &value : history) {
        const QJsonObject entry = value.toObject();
        const QString actor = taskText(entry, QStringLiteral("actor"));
        const QString kind = taskText(entry, QStringLiteral("kind"));
        const QString summary = taskText(entry, QStringLiteral("summary"));
        const QString body = taskText(entry, QStringLiteral("body"));
        const qint64 at =
            qint64(entry.value(QStringLiteral("createdAt")).toDouble());
        const QString who =
            actor.isEmpty()
                ? QStringLiteral("<span style='color:%1'>Someone</span>").arg(fg)
                : userProfileLinkHtml(actor, QStringLiteral("@") + actor);
        QString line = QStringLiteral("%1 <span style='color:%2'>%3</span>")
                           .arg(who, muted,
                                taskEventLabel(kind).toHtmlEscaped());
        if (kind == QLatin1String("replied") && !body.isEmpty()) {
            line += QStringLiteral("<div style='margin:4px 0 0 0;color:%1'>%2</div>")
                        .arg(fg, forkmesh::taskmarkdown::toHtml(body));
            const QString receipt = taskCommentReceiptHtml(entry);
            if (!receipt.isEmpty())
                line += QStringLiteral("<div style='color:%1;font-size:11px'>%2</div>")
                            .arg(subtle, receipt.toHtmlEscaped());
        } else if (!summary.isEmpty()) {
            line += QStringLiteral("<span style='color:%1'> &mdash; %2</span>")
                        .arg(muted, summary.toHtmlEscaped());
        }
        html += QStringLiteral(
                    "<tr><td width='26' valign='top' style='padding:4px 8px 4px 0'>%1</td>"
                    "<td valign='top' style='padding:4px 0'>%2</td>"
                    "<td valign='top' align='right' style='padding:4px 0 4px 8px;"
                    "color:%3;white-space:nowrap'>%4</td></tr>")
                    .arg(taskGlyphMarkup(taskPersonGlyph(actor, Theme::activeDark()), 18),
                         line, subtle,
                         taskCreatedAge(at).toHtmlEscaped());
    }
    return html + QStringLiteral("</table>");
}

QString taskHistoryHtml(const QJsonArray &history, bool dark)
{
    if (Theme::isNext())
        return taskHistoryNextHtml(history);
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
                         : userProfileLinkHtml(actor, QStringLiteral("@") + actor))
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
    auto row = [](const TaskGlyph &glyph, const QString &label,
                  const QString &value, bool valueIsHtml = false) {
        const QString shown =
            value.trimmed().isEmpty() ? QString::fromUtf8("\xE2\x80\x94")
                                      : valueIsHtml ? value
                                                    : value.toHtmlEscaped();
        return QStringLiteral(
                   "<tr><td style='padding:3px 7px 3px 0'>%1</td>"
                   "<th align='left' style='padding:3px 12px 3px 0;"
                   "color:%4%5'>%2</th><td style='padding:3px 0'>%3</td></tr>")
            .arg(taskGlyphMarkup(glyph, 14), label.toHtmlEscaped(), shown,
                 Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#8b949e")),
                 Theme::isNext() ? QStringLiteral(";font-weight:normal")
                                 : QString());
    };
    QString html = QStringLiteral("<table cellspacing='0'>");
    const QString destination = taskText(task, QStringLiteral("destination"));
    html += row(taskDestinationGlyph(destination, dark),
                QStringLiteral("Destination"), destination);
    const QString createdBy = taskText(task, QStringLiteral("createdBy"));
    html += row(taskPersonGlyph(createdBy, dark), QStringLiteral("Created by"),
                createdBy);
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
                       const QJsonArray &history,
                       const QString &fieldsHtml = QString())
{
    const bool next = Theme::isNext();
    if (task.isEmpty()) {
        return QStringLiteral(
            "<p style='color:%1'>Select a task to inspect its private "
            "details and available actions.</p>")
            .arg(Theme::nextOrHex(Theme::Role::FgMuted, QStringLiteral("#8b949e")));
    }
    const bool dark = currentThemeIsDark();
    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
    QString html =
        next ? QString()
             : QStringLiteral("<h2 style='margin:0 0 8px 0'>%1</h2>")
                   .arg(taskText(task, QStringLiteral("title")).toHtmlEscaped());
    auto block = [&html, next](const TaskGlyph &glyph, const QString &heading,
                               const QString &value) {
        const QString rendered = forkmesh::taskmarkdown::toHtml(value);
        if (next) {
            const bool details = heading == QLatin1String("Details");
            if (rendered.isEmpty() && !details)
                return;
            if (!details)
                html += taskNextCaptionHtml(heading.toUpper());
            html += rendered.isEmpty()
                        ? QStringLiteral("<p style='margin:0;color:%1'>No details.</p>")
                              .arg(Theme::hex(Theme::Role::FgSubtle))
                        : QStringLiteral("<div style='margin:0'>%1</div>").arg(rendered);
            return;
        }
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
    if (!attachmentHtml.isEmpty() && next) {
        html += taskNextCaptionHtml(QStringLiteral("ATTACHMENTS")) +
                QStringLiteral("<div style='white-space:normal'>%1</div>")
                    .arg(attachmentHtml);
    } else if (!attachmentHtml.isEmpty()) {
        html += QStringLiteral(
            "<h3 style='margin:14px 0 4px 0'>%1&nbsp;Attachments</h3>"
            "<div style='white-space:normal'>%2</div>")
            .arg(taskGlyphMarkup({QStringLiteral("paperclip"),
                                  taskMutedColor(dark), {}, 0.5}, 15),
                 attachmentHtml);
    }
    if (!fieldsHtml.isEmpty())
        html += taskNextCaptionHtml(QStringLiteral("DETAILS")) + fieldsHtml;
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

constexpr int kBoardIdRole = Qt::UserRole + 60;
constexpr int kBoardShortIdRole = Qt::UserRole + 61;
constexpr int kBoardProjectRole = Qt::UserRole + 62;
constexpr int kBoardPriorityRole = Qt::UserRole + 63;
constexpr int kBoardDoneRole = Qt::UserRole + 64;
constexpr int kBoardAvatarRole = Qt::UserRole + 65;
constexpr int kBoardAvatarKindRole = Qt::UserRole + 66;
constexpr int kBoardChecklistRole = Qt::UserRole + 67;
constexpr int kBoardBranchRole = Qt::UserRole + 68;
constexpr int kBoardAttachmentsRole = Qt::UserRole + 69;
constexpr int kBoardQaRole = Qt::UserRole + 70;
constexpr int kBoardQaStateRole = Qt::UserRole + 71;
constexpr int kBoardAgeRole = Qt::UserRole + 72;
constexpr int kBoardWorkingRole = Qt::UserRole + 73;
constexpr int kBoardProgressRole = Qt::UserRole + 74;
constexpr int kBoardCardPad = 12;
constexpr int kBoardCardGap = 8;
constexpr int kBoardTitleLines = 3;
constexpr int kBoardAvatar = 20;
constexpr int kBoardLaneMinWidth = 236;
const char kTasksNextHooksProperty[] = "tasksNextHooks";
const char kTasksNextKindProperty[] = "tasksNextKind";
const char kTasksNextFitPending[] = "tasksNextFitPending";
const char kTasksBoardMime[] = "application/x-forkmesh-task-id";

QString tasksBoardColumn(const QJsonObject &task)
{
    if (taskIsClosed(task))
        return QStringLiteral("done");
    if (organizationTaskAwaitsQa(task))
        return QStringLiteral("qa");
    return taskStatusKey(task);
}

QString tasksBoardColumnLabel(const QString &column)
{
    if (column == QLatin1String("qa"))
        return QStringLiteral("In QA");
    return taskStatusLabel(column);
}

QString tasksBoardChecklist(const QString &markdown)
{
    static const QRegularExpression item(QStringLiteral(
        "^(?:[ \\t]*>)*[ \\t]*(?:[-*+]|\\d{1,9}[.)])[ \\t]+\\[([ xX])\\][ \\t]"));
    static const QRegularExpression fence(
        QStringLiteral("^(?:[ \\t]*>)*[ \\t]{0,3}(`{3,}|~{3,})"));
    int total = 0;
    int checked = 0;
    QString openFence;
    const QStringList lines =
        QString(markdown).replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
            .split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch fenceMatch = fence.match(line);
        if (!openFence.isEmpty()) {
            const QString marker = fenceMatch.captured(1);
            if (fenceMatch.hasMatch() && marker.at(0) == openFence.at(0) &&
                marker.size() >= openFence.size())
                openFence.clear();
            continue;
        }
        if (fenceMatch.hasMatch()) {
            openFence = fenceMatch.captured(1);
            continue;
        }
        const QRegularExpressionMatch match = item.match(line);
        if (!match.hasMatch())
            continue;
        ++total;
        if (match.captured(1) != QLatin1String(" "))
            ++checked;
    }
    return total > 0 ? QStringLiteral("%1/%2").arg(checked).arg(total)
                     : QString();
}

QStringList tasksBoardTitleLines(const QString &text, const QFont &font,
                                 int width, int maxLines)
{
    QStringList lines;
    const QString source = text.simplified();
    if (width <= 0 || source.isEmpty())
        return lines;
    const QFontMetrics metrics(font);
    QTextLayout layout(source, font);
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (lines.size() == maxLines - 1) {
            lines.append(metrics.elidedText(source.mid(line.textStart()),
                                            Qt::ElideRight, width));
            break;
        }
        lines.append(source.mid(line.textStart(), line.textLength()).trimmed());
    }
    layout.endLayout();
    return lines;
}

QFont tasksNextFont(const QFont &base, int pixels, QFont::Weight weight)
{
    QFont font = base;
    font.setPixelSize(pixels);
    font.setWeight(weight);
    return font;
}

QFont tasksNextMonoFont(const QFont &base, int pixels)
{
    QFont font = Theme::monospaceFont(base);
    font.setPixelSize(pixels);
    font.setWeight(QFont::Normal);
    return font;
}

QPixmap tasksNextCanvas(int side)
{
    const qreal ratio = iconDevicePixelRatio();
    QPixmap pixmap(QSize(side, side) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    return pixmap;
}

QPixmap tasksBoardStatusPixmap(const QString &column, int side)
{
    if (column == QLatin1String("qa"))
        return tintedOcticonPixmap(QStringLiteral("shield-check"),
                                   Theme::color(Theme::Role::FgMuted), side);
    if (column == QLatin1String("done"))
        return tintedOcticonPixmap(QStringLiteral("check-circle"),
                                   Theme::color(Theme::Role::Success), side);
    QPixmap pixmap = tasksNextCanvas(side);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool active = column == QLatin1String("active");
    const QColor color = Theme::color(active ? Theme::Role::Accent
                                             : Theme::Role::FgMuted);
    QPen pen(color, 1.5);
    if (column == QLatin1String("queued"))
        pen.setDashPattern({1.6, 1.6});
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const QRectF ring(1.5, 1.5, side - 3.0, side - 3.0);
    painter.drawEllipse(ring);
    if (active) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(ring.center(), side / 6.0, side / 6.0);
    }
    return pixmap;
}

QPixmap tasksNextBoardGlyph(const QColor &color, int side)
{
    QPixmap pixmap = tasksNextCanvas(side);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.3));
    painter.setBrush(Qt::NoBrush);
    const qreal inset = 1.5;
    const QRectF frame(inset, inset, side - 2 * inset, side - 2 * inset);
    painter.drawRoundedRect(frame, 2.5, 2.5);
    const qreal third = frame.width() / 3.0;
    painter.drawLine(QPointF(frame.left() + third, frame.top()),
                     QPointF(frame.left() + third, frame.bottom()));
    painter.drawLine(QPointF(frame.left() + 2 * third, frame.top()),
                     QPointF(frame.left() + 2 * third, frame.bottom()));
    return pixmap;
}

QIcon tasksNextToggleIcon(const std::function<QPixmap(const QColor &)> &draw)
{
    QIcon icon;
    icon.addPixmap(draw(Theme::color(Theme::Role::FgMuted)), QIcon::Normal,
                   QIcon::Off);
    icon.addPixmap(draw(Theme::color(Theme::Role::Fg)), QIcon::Normal,
                   QIcon::On);
    return icon;
}

enum TasksNextButtonKind {
    TasksNextUnstyled = 0,
    TasksNextIcon,
    TasksNextText,
    TasksNextChip,
    TasksNextInverse,
};

class TasksNextButtonStyler : public QObject
{
public:
    using QObject::QObject;

    void adopt(QPushButton *widget, TasksNextButtonKind kind)
    {
        auto *button = dynamic_cast<VerticalIconButton *>(widget);
        if (!button)
            return;
        button->setProperty(kTasksNextKindProperty, int(kind));
        button->installEventFilter(this);
        fit(button);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *button = dynamic_cast<VerticalIconButton *>(watched);
        if (!button)
            return false;
        switch (event->type()) {
        case QEvent::Polish:
        case QEvent::StyleChange:
        case QEvent::FontChange:
            scheduleFit(button);
            return false;
        case QEvent::Show:
            fit(button);
            return false;
        case QEvent::ToolTip:
            if (active(button) && button->toolTip().isEmpty() &&
                !label(button).isEmpty()) {
                QToolTip::showText(
                    static_cast<QHelpEvent *>(event)->globalPos(),
                    label(button), button);
                return true;
            }
            return false;
        case QEvent::Paint:
            if (!active(button))
                return false;
            if (button->size() != preferredSize(button))
                scheduleFit(button);
            paint(button);
            return true;
        default:
            return false;
        }
    }

private:
    static int kindOf(const QWidget *button)
    {
        return button->property(kTasksNextKindProperty).toInt();
    }

    static bool active(const VerticalIconButton *button)
    {
        return Theme::isNext() && !button->inRail() &&
               kindOf(button) != TasksNextUnstyled;
    }

    static QString label(const VerticalIconButton *button)
    {
        if (kindOf(button) == TasksNextChip)
            return button->accessibleName();
        return button->text();
    }

    static QString iconName(const VerticalIconButton *button)
    {
        if (button->text() == QLatin1String("Stop"))
            return QStringLiteral("stop");
        return button->property("forkmeshOcticon").toString();
    }

    static int textWidth(const QFont &font, const QString &text)
    {
        return text.isEmpty()
                   ? 0
                   : qCeil(QFontMetricsF(font).horizontalAdvance(text)) + 1;
    }

    static QSize preferredSize(const VerticalIconButton *button)
    {
        const int kind = kindOf(button);
        if (kind == TasksNextIcon)
            return QSize(28, 28);
        if (kind == TasksNextChip) {
            const QFont font = tasksNextFont(button->font(), 12, QFont::Normal);
            int width = 10 + 14 + 6 + textWidth(font, button->accessibleName());
            if (!button->text().isEmpty())
                width += 6 + textWidth(font, button->text());
            return QSize(width + 10, 28);
        }
        if (kind == TasksNextInverse) {
            const QFont font = tasksNextFont(button->font(), 13, QFont::Medium);
            return QSize(12 + 14 + 6 + textWidth(font, button->text()) + 12, 32);
        }
        const QFont font = tasksNextFont(button->font(), 13, QFont::Normal);
        int width = 8 + 16;
        if (!button->text().isEmpty())
            width += 6 + textWidth(font, button->text());
        if (button->menu())
            width += 4 + 12;
        return QSize(width + 8, 28);
    }

    static void fit(VerticalIconButton *button)
    {
        if (!active(button))
            return;
        const QSize want = preferredSize(button);
        if (button->minimumSize() != want || button->maximumSize() != want)
            button->setFixedSize(want);
    }

    static void scheduleFit(VerticalIconButton *button)
    {
        if (button->property(kTasksNextFitPending).toBool())
            return;
        button->setProperty(kTasksNextFitPending, true);
        QPointer<VerticalIconButton> guarded(button);
        QMetaObject::invokeMethod(
            button,
            [guarded] {
                if (!guarded)
                    return;
                guarded->setProperty(kTasksNextFitPending, false);
                fit(guarded);
            },
            Qt::QueuedConnection);
    }

    static void drawGlyph(QPainter &painter, const VerticalIconButton *button,
                          const QRect &rect, const QColor &color)
    {
        const QString name = iconName(button);
        if (name.isEmpty()) {
            button->icon().paint(&painter, rect, Qt::AlignCenter,
                                 button->isEnabled() ? QIcon::Normal
                                                     : QIcon::Disabled);
            return;
        }
        painter.drawPixmap(rect.topLeft(),
                           tintedOcticonPixmap(name, color, rect.width()));
    }

    static void paint(VerticalIconButton *button)
    {
        QPainter painter(button);
        painter.setRenderHint(QPainter::Antialiasing);
        const int kind = kindOf(button);
        const bool enabled = button->isEnabled();
        const bool hovered = enabled && button->underMouse();
        const bool pressed = enabled && button->isDown();
        const bool checked = button->isChecked();
        const QRectF body(0.5, 0.5, button->width() - 1.0,
                          button->height() - 1.0);

        if (kind == TasksNextInverse) {
            QColor fill = Theme::color(pressed   ? Theme::Role::FgMuted
                                       : hovered ? Theme::Role::FgSecondary
                                                 : Theme::Role::Fg);
            if (!enabled)
                fill.setAlphaF(0.4);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(body, 8, 8);
            const QColor ink = Theme::color(Theme::Role::Bg);
            const QRect iconRect(12, (button->height() - 14) / 2, 14, 14);
            drawGlyph(painter, button, iconRect, ink);
            painter.setPen(ink);
            painter.setFont(tasksNextFont(button->font(), 13, QFont::Medium));
            painter.drawText(QRect(iconRect.right() + 7, 0,
                                   button->width() - iconRect.right() - 7,
                                   button->height()),
                             Qt::AlignVCenter | Qt::AlignLeft, button->text());
            return;
        }

        if (kind == TasksNextChip) {
            const QColor border = Theme::color(
                checked ? Theme::Role::Accent : Theme::Role::Border);
            const QColor fill = Theme::color(
                checked   ? Theme::Role::AccentSubtle
                : hovered ? Theme::Role::SurfaceRaised
                          : Theme::Role::Surface);
            QColor ink = Theme::color(checked   ? Theme::Role::AccentFg
                                      : hovered ? Theme::Role::Fg
                                                : Theme::Role::FgMuted);
            if (!enabled)
                ink = Theme::color(Theme::Role::FgSubtle);
            painter.setPen(QPen(border, 1));
            painter.setBrush(fill);
            painter.drawRoundedRect(body, body.height() / 2, body.height() / 2);
            const QRect iconRect(10, (button->height() - 14) / 2, 14, 14);
            drawGlyph(painter, button, iconRect, ink);
            const QFont font = tasksNextFont(button->font(), 12, QFont::Normal);
            painter.setFont(font);
            painter.setPen(ink);
            int x = iconRect.right() + 7;
            const QString name = button->accessibleName();
            painter.drawText(QRect(x, 0, textWidth(font, name), button->height()),
                             Qt::AlignVCenter | Qt::AlignLeft, name);
            x += textWidth(font, name) + 6;
            if (!button->text().isEmpty())
                painter.drawText(
                    QRect(x, 0, button->width() - x, button->height()),
                    Qt::AlignVCenter | Qt::AlignLeft, button->text());
            return;
        }

        if (hovered || pressed || checked) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::color(pressed || checked
                                              ? Theme::Role::SurfaceActive
                                              : Theme::Role::SurfaceRaised));
            painter.drawRoundedRect(body, 6, 6);
        }
        QColor ink = Theme::color(hovered || pressed || checked
                                      ? Theme::Role::Fg
                                      : Theme::Role::FgMuted);
        if (!enabled)
            ink = Theme::color(Theme::Role::FgSubtle);
        const QColor accent = button->accentColor();
        const QColor glyph = accent.isValid() && enabled ? accent : ink;
        const int iconX = kind == TasksNextIcon ? (button->width() - 16) / 2 : 8;
        const QRect iconRect(iconX, (button->height() - 16) / 2, 16, 16);
        drawGlyph(painter, button, iconRect, glyph);
        if (kind == TasksNextIcon)
            return;
        int x = iconRect.right() + 7;
        int right = button->width() - 8;
        if (button->menu()) {
            painter.drawPixmap(right - 12, (button->height() - 12) / 2,
                               tintedOcticonPixmap(QStringLiteral("chevron-down"),
                                                   ink, 12));
            right -= 16;
        }
        painter.setPen(ink);
        painter.setFont(tasksNextFont(button->font(), 13, QFont::Normal));
        painter.drawText(QRect(x, 0, qMax(0, right - x), button->height()),
                         Qt::AlignVCenter | Qt::AlignLeft, button->text());
    }
};

class TasksBoardCardDelegate : public QStyledItemDelegate
{
public:
    explicit TasksBoardCardDelegate(QListWidget *view)
        : QStyledItemDelegate(view), m_view(view)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        const int width = qMax(120, m_view->viewport()->width());
        const QFont title = tasksNextFont(option.font, 13, QFont::Normal);
        const int lines = qMax<int>(
            1, tasksBoardTitleLines(index.data(Qt::DisplayRole).toString(),
                                    title, width - 2 * kBoardCardPad,
                                    kBoardTitleLines)
                   .size());
        return QSize(width, kBoardCardPad + 16 + 6 +
                                lines * QFontMetrics(title).lineSpacing() + 10 +
                                kBoardAvatar + kBoardCardPad + kBoardCardGap);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
        const bool done = index.data(kBoardDoneRole).toBool();
        const QRect card =
            option.rect.adjusted(0, 0, 0, -kBoardCardGap);
        painter->setPen(QPen(Theme::color(selected  ? Theme::Role::Accent
                                          : hovered ? Theme::Role::BorderStrong
                                                    : Theme::Role::Border),
                             1));
        painter->setBrush(Theme::color(Theme::Role::Surface));
        painter->drawRoundedRect(QRectF(card).adjusted(0.5, 0.5, -0.5, -0.5), 12,
                                 12);

        const QRect inner =
            card.adjusted(kBoardCardPad, kBoardCardPad, -kBoardCardPad,
                          -kBoardCardPad);
        const QColor subtle = Theme::color(Theme::Role::FgSubtle);
        const QColor muted = Theme::color(Theme::Role::FgMuted);

        const QFont mono = tasksNextMonoFont(option.font, 11);
        const QFont small = tasksNextFont(option.font, 11, QFont::Normal);
        int x = inner.left();
        const QRect idLine(inner.left(), inner.top(), inner.width(), 16);
        const QString priority = index.data(kBoardPriorityRole).toString();
        int idRight = inner.right();
        if (!priority.isEmpty()) {
            painter->setFont(mono);
            const int width = QFontMetrics(mono).horizontalAdvance(priority);
            painter->setPen(subtle);
            painter->drawText(QRect(inner.right() - width + 1, idLine.top(), width,
                                    idLine.height()),
                              Qt::AlignVCenter | Qt::AlignRight, priority);
            idRight -= width + 8;
        }
        painter->setFont(mono);
        painter->setPen(subtle);
        const QString shortId = index.data(kBoardShortIdRole).toString();
        const int idWidth = QFontMetrics(mono).horizontalAdvance(shortId);
        painter->drawText(QRect(x, idLine.top(), idWidth + 1, idLine.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, shortId);
        x += idWidth + 1;
        const QString project = index.data(kBoardProjectRole).toString();
        if (!project.isEmpty() && x < idRight) {
            painter->setFont(small);
            const QString shown = QFontMetrics(small).elidedText(
                QString::fromUtf8(" \xC2\xB7 ") + project, Qt::ElideRight,
                idRight - x);
            painter->drawText(QRect(x, idLine.top(), idRight - x, idLine.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, shown);
        }

        const QFont titleFont = tasksNextFont(option.font, 13, QFont::Normal);
        const int lineHeight = QFontMetrics(titleFont).lineSpacing();
        const QStringList lines = tasksBoardTitleLines(
            index.data(Qt::DisplayRole).toString(), titleFont, inner.width(),
            kBoardTitleLines);
        painter->setFont(titleFont);
        painter->setPen(Theme::color(done ? Theme::Role::FgMuted
                                          : Theme::Role::Fg));
        int y = idLine.bottom() + 1 + 6;
        for (const QString &line : lines) {
            painter->drawText(QRect(inner.left(), y, inner.width(), lineHeight),
                              Qt::AlignVCenter | Qt::AlignLeft, line);
            y += lineHeight;
        }
        y = qMax(y, idLine.bottom() + 1 + 6 + lineHeight) + 10;

        const QRect avatar(inner.left(), y, kBoardAvatar, kBoardAvatar);
        const QString avatarKind = index.data(kBoardAvatarKindRole).toString();
        const QPixmap face = index.data(kBoardAvatarRole).value<QPixmap>();
        if (!face.isNull()) {
            QPainterPath clip;
            clip.addRoundedRect(QRectF(avatar),
                                avatarKind == QLatin1String("agent") ? 5 : 10,
                                avatarKind == QLatin1String("agent") ? 5 : 10);
            painter->save();
            painter->setClipPath(clip);
            painter->drawPixmap(avatar, face);
            painter->restore();
        } else if (avatarKind == QLatin1String("agent")) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(avatar), 5, 5);
            painter->drawPixmap(avatar.left() + 3, avatar.top() + 3,
                                tintedOcticonPixmap(QStringLiteral("smart-toy"),
                                                    muted, 14));
        } else {
            painter->setPen(QPen(subtle, 1, Qt::DashLine));
            painter->setBrush(Qt::NoBrush);
            painter->drawEllipse(QRectF(avatar).adjusted(0.5, 0.5, -0.5, -0.5));
            painter->drawPixmap(avatar.left() + 4, avatar.top() + 4,
                                tintedOcticonPixmap(QStringLiteral("person"),
                                                    subtle, 12));
        }
        if (index.data(kBoardWorkingRole).toBool()) {
            const QRectF dot(avatar.right() - 5, avatar.bottom() - 5, 8, 8);
            painter->setPen(QPen(Theme::color(Theme::Role::Surface), 2));
            painter->setBrush(Theme::color(Theme::Role::Accent));
            painter->drawEllipse(dot);
        }

        const QFont meta = tasksNextFont(option.font, 12, QFont::Normal);
        const QFontMetrics metaMetrics(meta);
        painter->setFont(meta);
        const QString age = index.data(kBoardAgeRole).toString();
        int right = inner.right() + 1;
        if (!age.isEmpty()) {
            const int width = metaMetrics.horizontalAdvance(age);
            painter->setPen(subtle);
            painter->drawText(QRect(right - width, avatar.top(), width,
                                    avatar.height()),
                              Qt::AlignVCenter | Qt::AlignRight, age);
            right -= width + 8;
        }
        x = avatar.right() + 1 + 10;
        const auto drawMeta = [&](const QString &icon, const QString &text,
                                  const QColor &color, int maxText) {
            if (text.isEmpty() || x + 12 + 4 >= right)
                return;
            painter->drawPixmap(x, avatar.top() + (avatar.height() - 12) / 2,
                                tintedOcticonPixmap(icon, color, 12));
            x += 12 + 4;
            const int room = qMin(maxText, right - x);
            if (room <= 0)
                return;
            const QString shown =
                metaMetrics.elidedText(text, Qt::ElideRight, room);
            const int width = metaMetrics.horizontalAdvance(shown);
            painter->setPen(color);
            painter->drawText(QRect(x, avatar.top(), width + 1, avatar.height()),
                              Qt::AlignVCenter | Qt::AlignLeft, shown);
            x += width + 10;
        };
        const QString checklist = index.data(kBoardChecklistRole).toString();
        if (!checklist.isEmpty()) {
            drawMeta(QStringLiteral("check-circle"), checklist, muted, 60);
        } else {
            const int progress = index.data(kBoardProgressRole).toInt();
            if (progress > 0 && !done)
                drawMeta(QStringLiteral("pulse"),
                         QStringLiteral("%1%").arg(progress), muted, 60);
        }
        drawMeta(QStringLiteral("git-branch"),
                 index.data(kBoardBranchRole).toString(), muted, 120);
        const int attachments = index.data(kBoardAttachmentsRole).toInt();
        if (attachments > 0)
            drawMeta(QStringLiteral("paperclip"), QString::number(attachments),
                     muted, 40);
        const QString qaState = index.data(kBoardQaStateRole).toString();
        const QColor qaColor = Theme::color(
            qaState == QLatin1String("passed")   ? Theme::Role::Success
            : qaState == QLatin1String("failed") ? Theme::Role::Danger
                                                 : Theme::Role::Warning);
        drawMeta(qaState == QLatin1String("failed")   ? QStringLiteral("circle-slash")
                 : qaState == QLatin1String("passed") ? QStringLiteral("shield-check")
                                                      : QStringLiteral("eye"),
                 index.data(kBoardQaRole).toString(), qaColor, 90);
        painter->restore();
    }

private:
    QListWidget *m_view = nullptr;
};

struct TasksBoardShared {
    std::function<void(const QString &, const QString &)> drop;
    std::function<void()> pending;
};

class TasksBoardList : public QListWidget
{
public:
    TasksBoardList(const QString &column, QFrame *lane,
                   std::shared_ptr<TasksBoardShared> shared)
        : m_column(column), m_lane(lane), m_shared(std::move(shared))
    {
        setObjectName(QStringLiteral("tasksBoardList"));
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        setResizeMode(QListView::Adjust);
        setUniformItemSizes(false);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setMouseTracking(true);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDropIndicatorShown(false);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::MoveAction);
        setItemDelegate(new TasksBoardCardDelegate(this));
        viewport()->setAutoFillBackground(false);
    }

    QString column() const { return m_column; }
    void setDropAllowed(bool allowed) { m_dropAllowed = allowed; }
    void setEmptyText(const QString &text) { m_emptyText = text; }

protected:
    QStringList mimeTypes() const override
    {
        return {QString::fromLatin1(kTasksBoardMime)};
    }

    QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
    {
        auto *mime = new QMimeData;
        if (!items.isEmpty() && items.constFirst())
            mime->setData(QString::fromLatin1(kTasksBoardMime),
                          items.constFirst()->data(kBoardIdRole).toString().toUtf8());
        return mime;
    }

    Qt::DropActions supportedDropActions() const override
    {
        return Qt::CopyAction | Qt::MoveAction;
    }

    void startDrag(Qt::DropActions) override
    {
        QListWidgetItem *item = currentItem();
        if (!item)
            return;
        auto *drag = new QDrag(this);
        drag->setMimeData(mimeData({item}));
        const QRect rect = visualItemRect(item);
        if (rect.isValid()) {
            const qreal ratio = devicePixelRatioF();
            QPixmap pixmap(rect.size() * ratio);
            pixmap.setDevicePixelRatio(ratio);
            pixmap.fill(Qt::transparent);
            QPainter painter(&pixmap);
            QStyleOptionViewItem option;
            initViewItemOption(&option);
            option.rect = QRect(QPoint(0, 0), rect.size());
            option.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
            itemDelegate()->paint(&painter, option, indexFromItem(item));
            painter.end();
            drag->setPixmap(pixmap);
            drag->setHotSpot(viewport()->mapFromGlobal(QCursor::pos()) -
                             rect.topLeft());
        }
        drag->exec(Qt::CopyAction | Qt::MoveAction, Qt::MoveAction);
        const std::shared_ptr<TasksBoardShared> shared = m_shared;
        QMetaObject::invokeMethod(
            this,
            [shared] {
                const std::function<void()> run =
                    std::exchange(shared->pending, {});
                if (run)
                    run();
            },
            Qt::QueuedConnection);
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (!accepts(event)) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
        setHover(true);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (!accepts(event)) {
            event->ignore();
            return;
        }
        event->setDropAction(Qt::MoveAction);
        event->accept();
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        setHover(false);
        event->accept();
    }

    void dropEvent(QDropEvent *event) override
    {
        setHover(false);
        if (!accepts(event)) {
            event->ignore();
            return;
        }
        const QString taskId = QString::fromUtf8(
            event->mimeData()->data(QString::fromLatin1(kTasksBoardMime)));
        const QString column = m_column;
        const std::shared_ptr<TasksBoardShared> shared = m_shared;
        shared->pending = [shared, taskId, column] {
            if (shared->drop)
                shared->drop(taskId, column);
        };
        event->setDropAction(Qt::CopyAction);
        event->accept();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QListWidget::paintEvent(event);
        if (count() > 0 || m_emptyText.isEmpty())
            return;
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box(0.5, 0.5, viewport()->width() - 1.0, 84.0);
        painter.setPen(QPen(Theme::color(Theme::Role::BorderStrong), 1,
                            Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, 12, 12);
        painter.setPen(Theme::color(Theme::Role::FgSubtle));
        painter.setFont(tasksNextFont(font(), 12, QFont::Normal));
        painter.drawText(box, Qt::AlignCenter, m_emptyText);
    }

private:
    bool accepts(const QDropEvent *event) const
    {
        return m_dropAllowed && event->mimeData() &&
               event->mimeData()->hasFormat(QString::fromLatin1(kTasksBoardMime)) &&
               dynamic_cast<TasksBoardList *>(event->source()) &&
               event->source() != this;
    }

    void setHover(bool hover)
    {
        if (!m_lane || m_lane->property("dropTarget").toBool() == hover)
            return;
        m_lane->setProperty("dropTarget", hover);
        m_lane->style()->unpolish(m_lane);
        m_lane->style()->polish(m_lane);
        m_lane->update();
    }

    QString m_column;
    QPointer<QFrame> m_lane;
    std::shared_ptr<TasksBoardShared> m_shared;
    QString m_emptyText;
    bool m_dropAllowed = true;
};

class TasksNextHooks : public QObject
{
public:
    using QObject::QObject;
    std::function<void()> render;
    std::function<void()> detail;
};

struct TasksNextLane {
    QString key;
    QPointer<QFrame> frame;
    QPointer<QLabel> count;
    QPointer<QToolButton> add;
    QPointer<TasksBoardList> list;
};

struct TasksNextUi {
    bool board = true;
    bool syncing = false;
    QPointer<QLabel> summary;
    QPointer<QLineEdit> filter;
    QPointer<QPushButton> assignee;
    QPointer<QPushButton> boardButton;
    QPointer<QPushButton> listButton;
    QPointer<QScrollArea> boardScroll;
    QPointer<QWidget> hint;
    QPointer<QWidget> detailPanel;
    QPointer<QLabel> detailId;
    QPointer<QLabel> detailTitle;
    QPointer<QPushButton> detailMore;
    std::vector<TasksNextLane> lanes;
};

void runTasksNextHook(const QObject *table, bool rows)
{
    if (!table)
        return;
    auto *hooks = dynamic_cast<TasksNextHooks *>(
        table->property(kTasksNextHooksProperty).value<QObject *>());
    if (!hooks)
        return;
    const std::function<void()> &hook = rows ? hooks->render : hooks->detail;
    if (hook)
        hook();
}

}

QWidget *TasksPage::buildOrganizationTasksSection()
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
            &TasksPage::deleteClosedOrganizationTasks);

    m_organizationTaskNewButton =
        new VerticalIconButton(QStringLiteral("New task"),
                               VerticalIconButton::Action);
    m_organizationTaskNewButton->setObjectName(
        QStringLiteral("organizationTaskNewAction"));
    setOcticon(m_organizationTaskNewButton, "plus", 16);
    connect(m_organizationTaskNewButton, &QPushButton::clicked, this,
            &TasksPage::createOrganizationTask);
    headingRow->addWidget(m_organizationTaskNewButton);

    auto *refresh = new VerticalIconButton(QStringLiteral("Refresh"),
                                           VerticalIconButton::Action);
    refresh->setObjectName(QStringLiteral("organizationTaskRefreshAction"));
    setOcticon(refresh, "sync", 16);
    refresh->setToolTip(QStringLiteral("Refresh the private task catalog"));
    connect(refresh, &QPushButton::clicked, this,
            &TasksPage::refreshOrganizationTasks);
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
    const auto openDetailLink = [this](const QUrl &url) {
        const QString href = url.toString();
        if (forkmesh::taskmarkdown::isSafeLink(href))
            QDesktopServices::openUrl(url);
        else if (href.startsWith(kAgentLinkScheme))
            windowCommand(QStringLiteral("agent.show"),
                          {{QStringLiteral("id"),
                            href.mid(kAgentLinkScheme.size()).toInt()}});
        else if (href.startsWith(kUserLinkScheme) && m_host.openUserProfile)
            m_host.openUserProfile(href.mid(kUserLinkScheme.size()));
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
        QStringLiteral("Start agent with prompt settings on this task's repository. "
                       "Includes your draft and attachments. In Task or other "
                       "non-agent modes, uses your most recent coding agent "
                       "or the default agent"));
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
            &TasksPage::editOrganizationTask);
    connect(m_organizationTaskAgentButton, &QPushButton::clicked, this,
            [this] {
                const QJsonObject task = selectedOrganizationTask(
                    m_organizationTasksTable, m_organizationTasks);
                const int sessionId = localAgentSessionIdForTask(
                    taskText(task, QStringLiteral("id")));
                if (sessionId > 0)
                    windowCommand(QStringLiteral("agent.show"),
                                  {{QStringLiteral("id"), sessionId}});
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
            &TasksPage::deleteOrganizationTask);
    connect(m_organizationTaskAttachWorktreeButton, &QPushButton::clicked,
            this, &TasksPage::attachExistingWorktreeToOrganizationTask);
    connect(m_organizationTaskFollowUpButton, &QPushButton::clicked, this,
            &TasksPage::createOrganizationTaskFollowUp);
    connect(m_organizationTaskIssueButton, &QPushButton::clicked, this,
            &TasksPage::moveOrganizationTaskToIssue);
    connect(m_organizationTaskPromptButton, &QPushButton::clicked, this,
            &TasksPage::addOrganizationTaskToPrompt);
    connect(m_organizationTaskCommentButton, &QPushButton::clicked, this,
            &TasksPage::commentOnOrganizationTask);
    connect(m_organizationTaskCheckinButton, &QPushButton::clicked, this,
            &TasksPage::checkInOnOrganizationTask);
    connect(m_organizationTaskRemindButton, &QPushButton::clicked, this,
            &TasksPage::remindOrganizationTaskAssignee);
    connect(m_organizationTaskStartAgentButton, &QPushButton::clicked, this,
            &TasksPage::startOrganizationTaskAgentFromPrompt);
    detailLayout->addWidget(actionHost);
    auto *detailBody = new QHBoxLayout;
    detailBody->setContentsMargins(0, 0, 0, 0);
    detailBody->setSpacing(12);
    auto *mainColumn = new QVBoxLayout;
    mainColumn->setContentsMargins(0, 0, 0, 0);
    mainColumn->setSpacing(6);
    mainColumn->addWidget(m_organizationTaskDetail, 1);

    auto *commentRow = new QHBoxLayout;
    commentRow->setContentsMargins(0, 0, 0, 0);
    commentRow->setSpacing(6);
    m_organizationTaskQuickComment = new QPlainTextEdit;
    m_organizationTaskQuickComment->setObjectName(
        QStringLiteral("organizationTaskQuickComment"));
    m_organizationTaskQuickComment->setAccessibleName(
        QStringLiteral("Comment on this task"));
    m_organizationTaskQuickComment->setPlaceholderText(QStringLiteral(
        "Leave a comment \u2014 @name pings someone. Ctrl+Enter posts."));
    m_organizationTaskQuickComment->setTabChangesFocus(true);
    m_organizationTaskQuickComment->setFixedHeight(
        m_organizationTaskQuickComment->fontMetrics().lineSpacing() * 3 + 14);
    m_organizationTaskQuickCommentSend =
        new QPushButton(QStringLiteral("Comment"));
    m_organizationTaskQuickCommentSend->setObjectName(
        QStringLiteral("organizationTaskQuickCommentSend"));
    m_organizationTaskQuickCommentSend->setToolTip(
        QStringLiteral("Post the comment (Ctrl+Enter). Everyone the task "
                       "routes through is pinged, and so is every @name"));
    setOcticon(m_organizationTaskQuickCommentSend, QStringLiteral("comment"),
               14);
    commentRow->addWidget(m_organizationTaskQuickComment, 1);
    commentRow->addWidget(m_organizationTaskQuickCommentSend, 0,
                          Qt::AlignBottom);
    mainColumn->addLayout(commentRow);
    connect(m_organizationTaskQuickCommentSend, &QPushButton::clicked, this,
            &TasksPage::submitOrganizationTaskQuickComment);
    for (const QKeySequence &keys :
         {QKeySequence(Qt::CTRL | Qt::Key_Return),
          QKeySequence(Qt::CTRL | Qt::Key_Enter)}) {
        auto *shortcut = new QShortcut(keys, m_organizationTaskQuickComment);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this,
                &TasksPage::submitOrganizationTaskQuickComment);
    }
    detailBody->addLayout(mainColumn, 1);

    m_organizationTaskSide = new QWidget;
    m_organizationTaskSide->setObjectName(
        QStringLiteral("organizationTaskSide"));
    m_organizationTaskSide->setFixedWidth(300);
    auto *sideLayout = new QVBoxLayout(m_organizationTaskSide);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(8);
    m_organizationTaskProperties = new QWidget;
    m_organizationTaskProperties->setObjectName(
        QStringLiteral("organizationTaskProperties"));
    m_organizationTaskProperties->setStyleSheet(QStringLiteral(
        "QLineEdit, QSpinBox, QComboBox { padding: 2px 6px; min-height: 18px;"
        " border: 1px solid transparent; border-radius: 4px;"
        " background: transparent; }"
        "QLineEdit:hover, QSpinBox:hover, QComboBox:hover {"
        " border-color: rgba(127, 127, 127, 90); }"
        "QLineEdit:focus, QSpinBox:focus, QComboBox:focus {"
        " border-color: #2f81f7; }"));
    auto *propertiesForm = new QFormLayout(m_organizationTaskProperties);
    propertiesForm->setContentsMargins(0, 0, 0, 0);
    propertiesForm->setHorizontalSpacing(10);
    propertiesForm->setVerticalSpacing(4);
    propertiesForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    propertiesForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const bool darkTheme = currentThemeIsDark();
    m_organizationTaskStatusEdit = new QComboBox;
    for (const auto &[key, label] :
         {std::pair{QStringLiteral("ready"), QStringLiteral("Ready")},
          std::pair{QStringLiteral("active"), QStringLiteral("In progress")},
          std::pair{QStringLiteral("done"), QStringLiteral("Done")}})
        m_organizationTaskStatusEdit->addItem(
            QIcon(tintedOcticonPixmap(taskStatusIcon(key),
                                      taskStatusColor(key, darkTheme), 14)),
            label, key);
    m_organizationTaskPriorityEdit = new QSpinBox;
    m_organizationTaskPriorityEdit->setRange(1, 99);
    m_organizationTaskPriorityEdit->setPrefix(QStringLiteral("P"));
    m_organizationTaskProgressEdit = new QSpinBox;
    m_organizationTaskProgressEdit->setRange(0, 100);
    m_organizationTaskProgressEdit->setSingleStep(10);
    m_organizationTaskProgressEdit->setSuffix(QStringLiteral("%"));
    m_organizationTaskAssigneeEdit = new QComboBox;
    m_organizationTaskDepartmentEdit = new QComboBox;
    m_organizationTaskTeamEdit = new QLineEdit;
    m_organizationTaskTeamEdit->setPlaceholderText(
        QStringLiteral("No specific team"));
    m_organizationTaskRepositoryEdit = new QLineEdit;
    m_organizationTaskRepositoryEdit->setPlaceholderText(
        QStringLiteral("owner/repository"));
    m_organizationTaskParentEdit = new QLineEdit;
    m_organizationTaskParentEdit->setPlaceholderText(
        QStringLiteral("Parent task ID"));
    const QList<std::pair<QString, QWidget *>> properties{
        {QStringLiteral("Status"), m_organizationTaskStatusEdit},
        {QStringLiteral("Priority"), m_organizationTaskPriorityEdit},
        {QStringLiteral("Progress"), m_organizationTaskProgressEdit},
        {QStringLiteral("Assignee"), m_organizationTaskAssigneeEdit},
        {QStringLiteral("Department"), m_organizationTaskDepartmentEdit},
        {QStringLiteral("Team"), m_organizationTaskTeamEdit},
        {QStringLiteral("Repository"), m_organizationTaskRepositoryEdit},
        {QStringLiteral("Follows task"), m_organizationTaskParentEdit},
    };
    for (const auto &[label, editor] : properties) {
        editor->setObjectName(QStringLiteral("organizationTaskPropertyEdit"));
        editor->setAccessibleName(label);
        auto *name = new QLabel(label);
        name->setObjectName(QStringLiteral("mutedLabel"));
        propertiesForm->addRow(name, editor);
    }
    m_organizationTaskPriorityEdit->setKeyboardTracking(false);
    m_organizationTaskProgressEdit->setKeyboardTracking(false);
    connect(m_organizationTaskStatusEdit, &QComboBox::activated, this,
            [this] { commitOrganizationTaskProperty(QStringLiteral("status")); });
    connect(m_organizationTaskAssigneeEdit, &QComboBox::activated, this,
            [this] {
                commitOrganizationTaskProperty(QStringLiteral("assignee"));
            });
    connect(m_organizationTaskDepartmentEdit, &QComboBox::activated, this,
            [this] {
                commitOrganizationTaskProperty(QStringLiteral("department"));
            });
    connect(m_organizationTaskPriorityEdit, &QSpinBox::valueChanged, this,
            [this] {
                commitOrganizationTaskProperty(QStringLiteral("priority"));
            });
    connect(m_organizationTaskProgressEdit, &QSpinBox::valueChanged, this,
            [this] {
                commitOrganizationTaskProperty(QStringLiteral("progress"));
            });
    connect(m_organizationTaskTeamEdit, &QLineEdit::editingFinished, this,
            [this] { commitOrganizationTaskProperty(QStringLiteral("team")); });
    connect(m_organizationTaskRepositoryEdit, &QLineEdit::editingFinished,
            this, [this] {
                commitOrganizationTaskProperty(QStringLiteral("repository"));
            });
    connect(m_organizationTaskParentEdit, &QLineEdit::editingFinished, this,
            [this] {
                commitOrganizationTaskProperty(QStringLiteral("parent"));
            });
    sideLayout->addWidget(m_organizationTaskProperties);
    sideLayout->addWidget(m_organizationTaskFields, 1);
    detailBody->addWidget(m_organizationTaskSide);
    detailLayout->addLayout(detailBody, 1);
    splitter->addWidget(detailHost);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    outer->addWidget(splitter, 1);

    if (Theme::isNext()) {
        auto *styler = new TasksNextButtonStyler(page);
        auto *hooks = new TasksNextHooks(page);
        auto ui = std::make_shared<TasksNextUi>();
        m_organizationTasksTable->setProperty(
            kTasksNextHooksProperty, QVariant::fromValue<QObject *>(hooks));

        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        for (QWidget *widget :
             {static_cast<QWidget *>(m_organizationTaskNewButton),
              static_cast<QWidget *>(refresh),
              static_cast<QWidget *>(m_organizationTaskDeleteClosedButton),
              static_cast<QWidget *>(m_organizationTaskViewButton),
              static_cast<QWidget *>(m_organizationTaskSortButton),
              static_cast<QWidget *>(m_organizationTaskMineFilterButton),
              static_cast<QWidget *>(m_organizationTaskOpenFilterButton),
              static_cast<QWidget *>(m_organizationTaskClosedFilterButton),
              static_cast<QWidget *>(m_organizationTaskAllFilterButton),
              static_cast<QWidget *>(m_organizationTasksStatus),
              static_cast<QWidget *>(m_organizationTasksSummary)})
            headingRow->removeWidget(widget);
        outer->removeItem(headingRow);
        delete headingRow;
        peopleScroll->hide();
        m_organizationTasksSummary->hide();

        auto *header = new QFrame(page);
        header->setObjectName(QStringLiteral("tasksNextHeader"));
        auto *headerRow = new QHBoxLayout(header);
        headerRow->setContentsMargins(24, 12, 16, 12);
        headerRow->setSpacing(6);
        auto *titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        auto *title = new QLabel(QStringLiteral("Tasks"));
        title->setObjectName(QStringLiteral("tasksNextTitle"));
        ui->summary = new QLabel;
        ui->summary->setObjectName(QStringLiteral("tasksNextSummary"));
        ui->summary->setTextFormat(Qt::PlainText);
        titleColumn->addWidget(title);
        titleColumn->addWidget(ui->summary);
        headerRow->addLayout(titleColumn);
        headerRow->addStretch(1);

        ui->filter = new QLineEdit;
        ui->filter->setObjectName(QStringLiteral("tasksNextFilter"));
        ui->filter->setPlaceholderText(QStringLiteral("Filter tasks"));
        ui->filter->setAccessibleName(QStringLiteral("Filter tasks"));
        ui->filter->setFixedWidth(224);
        ui->filter->addAction(
            QIcon(tintedOcticonPixmap(QStringLiteral("search"),
                                      Theme::color(Theme::Role::FgSubtle), 14)),
            QLineEdit::LeadingPosition);
        connect(ui->filter.data(), &QLineEdit::textEdited, this,
                [this](const QString &text) {
                    m_organizationTasksSearchMirror = text.trimmed();
                    m_organizationTaskShownRows = 0;
                    renderOrganizationTaskRows();
                });
        headerRow->addWidget(ui->filter);

        ui->assignee = new QPushButton(QStringLiteral("Everyone"));
        ui->assignee->setObjectName(QStringLiteral("tasksNextAssignee"));
        ui->assignee->setAccessibleName(QStringLiteral("Assignee"));
        ui->assignee->setToolTip(QStringLiteral("Show the tasks of one assignee"));
        ui->assignee->setLayoutDirection(Qt::RightToLeft);
        ui->assignee->setIcon(QIcon(tintedOcticonPixmap(
            QStringLiteral("chevron-down"), Theme::color(Theme::Role::FgMuted),
            12)));
        ui->assignee->setIconSize(QSize(12, 12));
        auto *assigneeMenu = new QMenu(ui->assignee);
        assigneeMenu->setLayoutDirection(Qt::LeftToRight);
        ui->assignee->setMenu(assigneeMenu);
        connect(assigneeMenu, &QMenu::aboutToShow, this, [this, assigneeMenu] {
            assigneeMenu->clear();
            if (!m_organizationTaskPeopleLayout)
                return;
            for (int i = 0; i < m_organizationTaskPeopleLayout->count(); ++i) {
                auto *person = qobject_cast<QToolButton *>(
                    m_organizationTaskPeopleLayout->itemAt(i)->widget());
                if (!person)
                    continue;
                QAction *action = assigneeMenu->addAction(
                    person->icon(),
                    person->accessibleName() + QLatin1Char('\t') + person->text());
                action->setCheckable(true);
                action->setChecked(person->isChecked());
                action->setToolTip(person->toolTip());
                QPointer<QToolButton> guarded(person);
                connect(action, &QAction::triggered, person, [guarded] {
                    if (guarded)
                        guarded->click();
                });
            }
        });
        headerRow->addWidget(ui->assignee);

        auto *viewSwitch = new QFrame;
        viewSwitch->setObjectName(QStringLiteral("tasksNextViewSwitch"));
        auto *viewRow = new QHBoxLayout(viewSwitch);
        viewRow->setContentsMargins(2, 2, 2, 2);
        viewRow->setSpacing(2);
        auto *viewGroup = new QButtonGroup(viewSwitch);
        viewGroup->setExclusive(true);
        const auto makeViewButton = [viewRow, viewGroup](const QString &tip,
                                                          const QIcon &icon) {
            auto *button = new QPushButton;
            button->setObjectName(QStringLiteral("tasksNextViewButton"));
            button->setCheckable(true);
            button->setToolTip(tip);
            button->setAccessibleName(tip);
            button->setIcon(icon);
            button->setIconSize(QSize(14, 14));
            button->setFixedSize(28, 28);
            button->setCursor(Qt::PointingHandCursor);
            viewGroup->addButton(button);
            viewRow->addWidget(button);
            return button;
        };
        ui->boardButton = makeViewButton(
            QStringLiteral("Board"),
            tasksNextToggleIcon([](const QColor &color) {
                return tasksNextBoardGlyph(color, 14);
            }));
        ui->listButton = makeViewButton(
            QStringLiteral("List"),
            tasksNextToggleIcon([](const QColor &color) {
                return tintedOcticonPixmap(QStringLiteral("list-unordered"),
                                           color, 14);
            }));
        headerRow->addWidget(viewSwitch);
        headerRow->addSpacing(2);
        headerRow->addWidget(refresh);
        headerRow->addWidget(m_organizationTaskDeleteClosedButton);
        headerRow->addWidget(m_organizationTaskNewButton);
        styler->adopt(refresh, TasksNextText);
        styler->adopt(m_organizationTaskDeleteClosedButton, TasksNextIcon);
        styler->adopt(m_organizationTaskNewButton, TasksNextInverse);
        outer->insertWidget(0, header);

        auto *chips = new QFrame(page);
        chips->setObjectName(QStringLiteral("tasksNextChips"));
        auto *chipRow = new QHBoxLayout(chips);
        chipRow->setContentsMargins(24, 8, 16, 8);
        chipRow->setSpacing(6);
        for (QPushButton *chip :
             {m_organizationTaskOpenFilterButton,
              m_organizationTaskClosedFilterButton,
              m_organizationTaskAllFilterButton,
              m_organizationTaskMineFilterButton}) {
            chipRow->addWidget(chip);
            styler->adopt(chip, TasksNextChip);
        }
        chipRow->addStretch(1);
        chipRow->addWidget(m_organizationTaskViewButton);
        chipRow->addWidget(m_organizationTaskSortButton);
        styler->adopt(m_organizationTaskViewButton, TasksNextText);
        styler->adopt(m_organizationTaskSortButton, TasksNextText);
        outer->insertWidget(1, chips);

        splitter->setObjectName(QStringLiteral("tasksNextSplit"));
        splitter->setHandleWidth(1);

        auto shared = std::make_shared<TasksBoardShared>();
        ui->boardScroll = new QScrollArea;
        ui->boardScroll->setObjectName(QStringLiteral("tasksNextBoardScroll"));
        ui->boardScroll->setFrameShape(QFrame::NoFrame);
        ui->boardScroll->setWidgetResizable(true);
        ui->boardScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        ui->boardScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        auto *board = new QWidget;
        board->setObjectName(QStringLiteral("tasksNextBoard"));
        auto *boardRow = new QHBoxLayout(board);
        boardRow->setContentsMargins(20, 16, 20, 12);
        boardRow->setSpacing(12);
        for (const char *key : {"ready", "queued", "active", "qa", "done"}) {
            TasksNextLane lane;
            lane.key = QString::fromLatin1(key);
            lane.frame = new QFrame;
            lane.frame->setObjectName(QStringLiteral("tasksBoardColumn"));
            lane.frame->setMinimumWidth(kBoardLaneMinWidth);
            auto *laneLayout = new QVBoxLayout(lane.frame);
            laneLayout->setContentsMargins(8, 0, 8, 8);
            laneLayout->setSpacing(0);
            auto *laneHeader = new QWidget;
            laneHeader->setFixedHeight(44);
            auto *laneHeaderRow = new QHBoxLayout(laneHeader);
            laneHeaderRow->setContentsMargins(6, 0, 0, 0);
            laneHeaderRow->setSpacing(8);
            auto *laneIcon = new QLabel;
            laneIcon->setObjectName(QStringLiteral("tasksBoardColumnIcon"));
            laneIcon->setPixmap(tasksBoardStatusPixmap(lane.key, 16));
            laneIcon->setFixedSize(16, 16);
            auto *laneTitle = new QLabel(tasksBoardColumnLabel(lane.key));
            laneTitle->setObjectName(QStringLiteral("tasksBoardColumnTitle"));
            lane.count = new QLabel(QStringLiteral("0"));
            lane.count->setObjectName(QStringLiteral("tasksBoardColumnCount"));
            laneHeaderRow->addWidget(laneIcon);
            laneHeaderRow->addWidget(laneTitle);
            laneHeaderRow->addWidget(lane.count);
            laneHeaderRow->addStretch(1);
            if (lane.key == QLatin1String("ready")) {
                lane.add = new QToolButton;
                lane.add->setObjectName(QStringLiteral("tasksBoardColumnAdd"));
                lane.add->setIcon(QIcon(tintedOcticonPixmap(
                    QStringLiteral("plus"), Theme::color(Theme::Role::FgMuted),
                    14)));
                lane.add->setIconSize(QSize(14, 14));
                lane.add->setFixedSize(28, 28);
                lane.add->setAutoRaise(true);
                lane.add->setCursor(Qt::PointingHandCursor);
                lane.add->setToolTip(QStringLiteral("New task"));
                lane.add->setAccessibleName(QStringLiteral("New task"));
                connect(lane.add.data(), &QToolButton::clicked, this, [this] {
                    if (m_organizationTaskNewButton)
                        m_organizationTaskNewButton->click();
                });
                laneHeaderRow->addWidget(lane.add);
            }
            lane.list = new TasksBoardList(lane.key, lane.frame, shared);
            lane.list->setDropAllowed(lane.key != QLatin1String("queued"));
            lane.list->setEmptyText(lane.key == QLatin1String("queued")
                                        ? QStringLiteral("No queued tasks")
                                        : QStringLiteral("Drop a task here"));
            laneLayout->addWidget(laneHeader);
            laneLayout->addWidget(lane.list, 1);
            boardRow->addWidget(lane.frame, 1);
            ui->lanes.push_back(lane);
        }
        ui->boardScroll->setWidget(board);
        tableLayout->insertWidget(0, ui->boardScroll, 1);

        const auto selectTask = [this](const QString &taskId) {
            if (!m_organizationTasksTable || taskId.isEmpty())
                return;
            int row = organizationTaskRowForId(taskId);
            if (row < 0) {
                for (int position = 0;
                     position < m_organizationTaskMatches.size(); ++position) {
                    const int index = m_organizationTaskMatches.at(position);
                    if (index < 0 || index >= m_organizationTasks.size() ||
                        taskText(m_organizationTasks.at(index).toObject(),
                                 QStringLiteral("id")) != taskId)
                        continue;
                    appendOrganizationTaskRows(
                        position + 1 - m_organizationTasksTable->rowCount());
                    break;
                }
                row = organizationTaskRowForId(taskId);
            }
            if (row >= 0 && row != m_organizationTasksTable->currentRow())
                m_organizationTasksTable->selectRow(row);
        };
        const auto syncBoardSelection = [this, ui] {
            const QString selectedId =
                taskText(selectedOrganizationTask(m_organizationTasksTable,
                                                  m_organizationTasks),
                         QStringLiteral("id"));
            ui->syncing = true;
            for (const TasksNextLane &lane : ui->lanes) {
                if (!lane.list)
                    continue;
                const QSignalBlocker blocker(lane.list);
                QListWidgetItem *found = nullptr;
                for (int i = 0; i < lane.list->count() && !selectedId.isEmpty();
                     ++i) {
                    if (lane.list->item(i)->data(kBoardIdRole).toString() ==
                        selectedId) {
                        found = lane.list->item(i);
                        break;
                    }
                }
                if (found) {
                    lane.list->setCurrentItem(found);
                    lane.list->scrollToItem(found);
                } else {
                    lane.list->clearSelection();
                    lane.list->setCurrentItem(nullptr);
                }
                lane.list->viewport()->update();
            }
            ui->syncing = false;
        };
        for (const TasksNextLane &lane : ui->lanes) {
            connect(lane.list.data(), &QListWidget::currentItemChanged, this,
                    [ui, selectTask](QListWidgetItem *current) {
                        if (ui->syncing || !current)
                            return;
                        selectTask(current->data(kBoardIdRole).toString());
                    });
            connect(lane.list.data(), &QListWidget::itemClicked, this,
                    [ui, selectTask](QListWidgetItem *item) {
                        if (!item)
                            return;
                        selectTask(item->data(kBoardIdRole).toString());
                        if (ui->detailPanel)
                            ui->detailPanel->show();
                    });
            connect(lane.list.data(), &QListWidget::itemDoubleClicked, this,
                    [this](QListWidgetItem *) { editOrganizationTask(); });
        }
        connect(m_organizationTasksTable, &QTableWidget::itemSelectionChanged,
                this, syncBoardSelection);
        connect(m_organizationTasksTable, &QTableWidget::itemClicked, this,
                [ui](QTableWidgetItem *) {
                    if (ui->detailPanel)
                        ui->detailPanel->show();
                });

        shared->drop = [this, ui, selectTask](const QString &taskId,
                                              const QString &target) {
            selectTask(taskId);
            const QJsonObject task = selectedOrganizationTask(
                m_organizationTasksTable, m_organizationTasks);
            if (taskText(task, QStringLiteral("id")) != taskId)
                return;
            const QString from = tasksBoardColumn(task);
            if (from == target)
                return;
            const QString startText = m_organizationTaskStartButton
                                          ? m_organizationTaskStartButton->text()
                                          : QString();
            QPushButton *button = nullptr;
            if (target == QLatin1String("active") &&
                (from == QLatin1String("ready") ||
                 from == QLatin1String("queued")) &&
                startText == QLatin1String("Start"))
                button = m_organizationTaskStartButton;
            else if (target == QLatin1String("ready") &&
                     from == QLatin1String("active") &&
                     startText == QLatin1String("Stop"))
                button = m_organizationTaskStartButton;
            else if (target == QLatin1String("ready") &&
                     from == QLatin1String("done"))
                button = m_organizationTaskUndoButton;
            else if (target == QLatin1String("done"))
                button = m_organizationTaskCompleteButton;
            else if (target == QLatin1String("qa") &&
                     from != QLatin1String("done"))
                button = m_organizationTaskQaButton;
            if (button && button->isEnabled() && !button->isHidden()) {
                if (ui->detailPanel)
                    ui->detailPanel->show();
                button->click();
                return;
            }
            if (m_organizationTasksStatus)
                m_organizationTasksStatus->setText(
                    QStringLiteral("This task cannot move to %1 from here.")
                        .arg(tasksBoardColumnLabel(target)));
        };

        auto *footer = new QFrame(page);
        footer->setObjectName(QStringLiteral("tasksNextFooter"));
        footer->setFixedHeight(32);
        auto *footerRow = new QHBoxLayout(footer);
        footerRow->setContentsMargins(24, 0, 16, 0);
        footerRow->setSpacing(8);
        auto *hint = new QWidget;
        auto *hintRow = new QHBoxLayout(hint);
        hintRow->setContentsMargins(0, 0, 0, 0);
        hintRow->setSpacing(8);
        auto *hintIcon = new QLabel;
        hintIcon->setPixmap(tintedOcticonPixmap(
            QStringLiteral("clock"), Theme::color(Theme::Role::FgSubtle), 12));
        auto *hintText = new QLabel(
            QStringLiteral("Drag a card to another column to change its status"));
        hintText->setObjectName(QStringLiteral("tasksNextHint"));
        hintRow->addWidget(hintIcon);
        hintRow->addWidget(hintText);
        ui->hint = hint;
        footerRow->addWidget(hint);
        m_organizationTasksStatus->setWordWrap(false);
        footerRow->addWidget(m_organizationTasksStatus, 1);
        outer->addWidget(footer);

        const auto applyMode = [this, ui] {
            const bool boardMode = ui->board;
            if (ui->boardScroll)
                ui->boardScroll->setVisible(boardMode);
            m_organizationTasksTable->setVisible(!boardMode);
            if (m_organizationTaskPageLabel)
                m_organizationTaskPageLabel->setVisible(!boardMode);
            if (m_organizationTaskViewButton)
                m_organizationTaskViewButton->setVisible(!boardMode);
            if (ui->hint)
                ui->hint->setVisible(boardMode);
            if (ui->boardButton)
                ui->boardButton->setChecked(boardMode);
            if (ui->listButton)
                ui->listButton->setChecked(!boardMode);
            updateOrganizationTaskSpinTimer();
        };
        connect(ui->boardButton.data(), &QPushButton::clicked, this, [ui, applyMode] {
            ui->board = true;
            applyMode();
        });
        connect(ui->listButton.data(), &QPushButton::clicked, this, [ui, applyMode] {
            ui->board = false;
            applyMode();
        });
        applyMode();

        detailHost->setObjectName(QStringLiteral("tasksNextDetailPanel"));
        detailHost->setAttribute(Qt::WA_StyledBackground, true);
        detailHost->setMinimumWidth(340);
        ui->detailPanel = detailHost;
        detailLayout->removeWidget(actionHost);
        mainColumn->removeWidget(m_organizationTaskDetail);
        commentRow->removeWidget(m_organizationTaskQuickComment);
        commentRow->removeWidget(m_organizationTaskQuickCommentSend);
        detailBody->removeWidget(m_organizationTaskSide);
        detailLayout->removeItem(detailBody);
        delete detailBody;
        detailLayout->setContentsMargins(0, 0, 0, 0);
        detailLayout->setSpacing(0);

        auto *detailHeader = new QFrame;
        detailHeader->setObjectName(QStringLiteral("tasksNextDetailHeader"));
        detailHeader->setFixedHeight(48);
        auto *detailHeaderRow = new QHBoxLayout(detailHeader);
        detailHeaderRow->setContentsMargins(16, 0, 10, 0);
        detailHeaderRow->setSpacing(2);
        ui->detailId = new QLabel;
        ui->detailId->setObjectName(QStringLiteral("tasksNextDetailId"));
        ui->detailId->setTextFormat(Qt::PlainText);
        ui->detailId->setTextInteractionFlags(Qt::TextSelectableByMouse);
        ui->detailId->setFont(tasksNextMonoFont(ui->detailId->font(), 12));
        detailHeaderRow->addWidget(ui->detailId);
        detailHeaderRow->addStretch(1);
        const auto makeHeaderButton = [](const QString &name, const QString &icon,
                                         const QString &tip) {
            auto *button = new QPushButton;
            button->setObjectName(name);
            button->setIcon(QIcon(tintedOcticonPixmap(
                icon, Theme::color(Theme::Role::FgMuted), 16)));
            button->setIconSize(QSize(16, 16));
            button->setFixedSize(28, 28);
            button->setToolTip(tip);
            button->setAccessibleName(tip);
            button->setCursor(Qt::PointingHandCursor);
            return button;
        };
        ui->detailMore = makeHeaderButton(QStringLiteral("tasksNextDetailMore"),
                                          QStringLiteral("kebab-horizontal"),
                                          QStringLiteral("More actions"));
        auto *moreMenu = new QMenu(ui->detailMore);
        moreMenu->setObjectName(QStringLiteral("tasksNextMoreMenu"));
        ui->detailMore->setMenu(moreMenu);
        connect(moreMenu, &QMenu::aboutToShow, this, [this, moreMenu] {
            moreMenu->clear();
            const auto mirror = [moreMenu](QPushButton *button, const QString &text,
                                           const QIcon &icon) {
                if (!button)
                    return;
                QAction *action = moreMenu->addAction(icon, text);
                action->setEnabled(button->isEnabled());
                action->setToolTip(button->toolTip());
                QPointer<QPushButton> guarded(button);
                QObject::connect(action, &QAction::triggered, button, [guarded] {
                    if (guarded)
                        guarded->click();
                });
            };
            mirror(m_organizationTaskEditButton, QStringLiteral("Edit"),
                   QIcon(tintedOcticonPixmap(QStringLiteral("pencil"),
                                             Theme::color(Theme::Role::FgMuted),
                                             16)));
            moreMenu->addSeparator();
            mirror(m_organizationTaskDeleteButton, QStringLiteral("Move to trash"),
                   QIcon(tintedOcticonPixmap(QStringLiteral("trash"),
                                             Theme::color(Theme::Role::Danger),
                                             16)));
        });
        auto *closeDetail = makeHeaderButton(QStringLiteral("tasksNextDetailClose"),
                                             QStringLiteral("x"),
                                             QStringLiteral("Close"));
        connect(closeDetail, &QPushButton::clicked, detailHost,
                [detailHost] { detailHost->hide(); });
        detailHeaderRow->addWidget(ui->detailMore);
        detailHeaderRow->addWidget(closeDetail);

        actionHost->setObjectName(QStringLiteral("tasksNextDetailActions"));
        actionHost->setAttribute(Qt::WA_StyledBackground, true);
        actions->setContentsMargins(12, 8, 12, 8);
        m_organizationTaskEditButton->hide();
        m_organizationTaskDeleteButton->hide();
        for (QPushButton *button :
             {m_organizationTaskStartButton, m_organizationTaskCompleteButton,
              m_organizationTaskUndoButton, m_organizationTaskAgentButton,
              m_organizationTaskStartAgentButton, m_organizationTaskPromptButton,
              m_organizationTaskQaButton, m_organizationTaskReturnButton,
              m_organizationTaskFollowUpButton, m_organizationTaskIssueButton,
              m_organizationTaskCommentButton, m_organizationTaskCheckinButton,
              m_organizationTaskRemindButton,
              m_organizationTaskAttachWorktreeButton})
            styler->adopt(button, TasksNextIcon);

        auto *detailScroll = new QScrollArea;
        detailScroll->setObjectName(QStringLiteral("tasksNextDetailScroll"));
        detailScroll->setFrameShape(QFrame::NoFrame);
        detailScroll->setWidgetResizable(true);
        detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *detailBodyWidget = new QWidget;
        detailBodyWidget->setObjectName(QStringLiteral("tasksNextDetailBody"));
        auto *bodyLayout = new QVBoxLayout(detailBodyWidget);
        bodyLayout->setContentsMargins(20, 18, 20, 20);
        bodyLayout->setSpacing(14);
        ui->detailTitle = new QLabel;
        ui->detailTitle->setObjectName(QStringLiteral("tasksNextDetailTitle"));
        ui->detailTitle->setTextFormat(Qt::PlainText);
        ui->detailTitle->setWordWrap(true);
        ui->detailTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_organizationTaskSide->setMinimumWidth(0);
        m_organizationTaskSide->setMaximumWidth(QWIDGETSIZE_MAX);
        m_organizationTaskFields->hide();
        m_organizationTaskProperties->setStyleSheet(Theme::expandTokens(
            QStringLiteral(
                "QLabel#mutedLabel { color: @fgMuted; font-size: 13px; }"
                "QLineEdit, QSpinBox, QComboBox { padding: 2px 6px;"
                " min-height: 22px; border: 1px solid transparent;"
                " border-radius: 6px; background: transparent; color: @fg;"
                " font-size: 13px; }"
                "QLineEdit:hover, QSpinBox:hover, QComboBox:hover {"
                " background-color: @surfaceRaised; }"
                "QLineEdit:focus, QSpinBox:focus, QComboBox:focus {"
                " border-color: @accent; }"
                "QLineEdit:disabled, QSpinBox:disabled, QComboBox:disabled {"
                " color: @fgSecondary; }"),
            Theme::activeDesign(), Theme::activeDark()));
        for (QLabel *name : m_organizationTaskProperties->findChildren<QLabel *>(
                 QStringLiteral("mutedLabel"), Qt::FindDirectChildrenOnly))
            name->setMinimumWidth(96);
        propertiesForm->setVerticalSpacing(2);
        m_organizationTaskDetail->setFrameShape(QFrame::NoFrame);
        m_organizationTaskDetail->setVerticalScrollBarPolicy(
            Qt::ScrollBarAlwaysOff);
        m_organizationTaskDetail->setHorizontalScrollBarPolicy(
            Qt::ScrollBarAlwaysOff);
        m_organizationTaskDetail->setSizePolicy(QSizePolicy::Preferred,
                                                QSizePolicy::Fixed);
        QPointer<QTextBrowser> detailBrowser(m_organizationTaskDetail);
        const auto fitDetailBrowser = [detailBrowser] {
            if (!detailBrowser)
                return;
            const int height =
                qCeil(detailBrowser->document()->size().height()) +
                2 * detailBrowser->frameWidth();
            if (detailBrowser->height() != height)
                detailBrowser->setFixedHeight(qMax(24, height));
        };
        connect(m_organizationTaskDetail->document()->documentLayout(),
                &QAbstractTextDocumentLayout::documentSizeChanged, this,
                fitDetailBrowser);
        bodyLayout->addWidget(ui->detailTitle);
        bodyLayout->addWidget(m_organizationTaskSide);
        bodyLayout->addWidget(m_organizationTaskDetail);
        bodyLayout->addStretch(1);
        detailScroll->setWidget(detailBodyWidget);

        auto *commentBar = new QFrame;
        commentBar->setObjectName(QStringLiteral("tasksNextCommentBar"));
        auto *commentBarLayout = new QVBoxLayout(commentBar);
        commentBarLayout->setContentsMargins(12, 10, 12, 12);
        auto *commentBox = new QFrame;
        commentBox->setObjectName(QStringLiteral("tasksNextCommentBox"));
        auto *commentBoxRow = new QHBoxLayout(commentBox);
        commentBoxRow->setContentsMargins(10, 4, 6, 4);
        commentBoxRow->setSpacing(6);
        m_organizationTaskQuickComment->setFixedHeight(
            m_organizationTaskQuickComment->fontMetrics().lineSpacing() * 2 + 12);
        commentBoxRow->addWidget(m_organizationTaskQuickComment, 1);
        commentBoxRow->addWidget(m_organizationTaskQuickCommentSend, 0,
                                 Qt::AlignBottom);
        commentBarLayout->addWidget(commentBox);

        detailLayout->addWidget(detailHeader);
        detailLayout->addWidget(actionHost);
        detailLayout->addWidget(detailScroll, 1);
        detailLayout->addWidget(commentBar);
        splitter->setStretchFactor(0, 1);
        splitter->setStretchFactor(1, 0);
        splitter->setSizes({960, 400});

        hooks->detail = [this, ui, fitDetailBrowser] {
            const QJsonObject task = selectedOrganizationTask(
                m_organizationTasksTable, m_organizationTasks);
            const QString id = taskText(task, QStringLiteral("id"));
            if (ui->detailId) {
                ui->detailId->setText(id.left(8));
                ui->detailId->setToolTip(id);
            }
            if (ui->detailTitle) {
                ui->detailTitle->setText(taskText(task, QStringLiteral("title")));
                ui->detailTitle->setVisible(!task.isEmpty());
            }
            if (ui->detailMore)
                ui->detailMore->setEnabled(!task.isEmpty());
            if (ui->detailPanel && task.isEmpty())
                ui->detailPanel->hide();
            fitDetailBrowser();
        };

        hooks->render = [this, ui, syncBoardSelection] {
            if (!m_organizationTasksTable)
                return;
            if (ui->filter && !ui->filter->hasFocus() &&
                ui->filter->text().trimmed() != m_organizationTasksSearchMirror)
                ui->filter->setText(m_organizationTasksSearchMirror);
            int open = 0;
            int withAgents = 0;
            int inProgress = 0;
            for (const QJsonValue &value : std::as_const(m_organizationTasks)) {
                const QJsonObject task = value.toObject();
                if (taskIsClosed(task))
                    continue;
                ++open;
                if (taskText(task, QStringLiteral("assigneeKind")) ==
                    QLatin1String("agent"))
                    ++withAgents;
                if (taskText(task, QStringLiteral("status")) ==
                    QLatin1String("active"))
                    ++inProgress;
            }
            if (ui->summary) {
                QStringList parts{
                    QStringLiteral("%1 open").arg(open),
                    QStringLiteral("%1 with agents").arg(withAgents),
                    QStringLiteral("%1 in progress").arg(inProgress),
                    QStringLiteral("%1 closed")
                        .arg(m_organizationTasks.size() - open)};
                if (m_organizationTasksCanManage)
                    parts << QStringLiteral("manager");
                ui->summary->setText(
                    parts.join(QString::fromUtf8(" \xC2\xB7 ")));
            }
            if (ui->assignee) {
                QString who = QStringLiteral("Everyone");
                if (m_organizationTaskPeopleGroup) {
                    for (QAbstractButton *button :
                         m_organizationTaskPeopleGroup->buttons()) {
                        if (button->property("assigneeFilter").toString() ==
                            m_organizationTaskAssigneeFilter) {
                            who = button->accessibleName();
                            break;
                        }
                    }
                }
                ui->assignee->setText(who);
            }

            const QString statusFilter = m_organizationTaskStatusFilter;
            QHash<QString, QList<int>> byColumn;
            for (int index : std::as_const(m_organizationTaskMatches)) {
                if (index < 0 || index >= m_organizationTasks.size())
                    continue;
                byColumn[tasksBoardColumn(m_organizationTasks.at(index).toObject())]
                    .append(index);
            }
            ui->syncing = true;
            for (const TasksNextLane &lane : ui->lanes) {
                if (!lane.frame || !lane.list)
                    continue;
                const bool doneLane = lane.key == QLatin1String("done");
                const bool visible =
                    statusFilter == QLatin1String("all") ||
                    (statusFilter == QLatin1String("closed")) == doneLane;
                lane.frame->setVisible(visible);
                const QList<int> indices = byColumn.value(lane.key);
                if (lane.count)
                    lane.count->setText(QString::number(indices.size()));
                if (lane.add)
                    lane.add->setVisible(m_organizationTasksCanManage);
                const QSignalBlocker blocker(lane.list);
                const int scroll = lane.list->verticalScrollBar()->value();
                lane.list->clear();
                for (int index : indices) {
                    const QJsonObject task = m_organizationTasks.at(index).toObject();
                    const QString id = taskText(task, QStringLiteral("id"));
                    const QString statusKey = taskStatusKey(task);
                    const QString department =
                        taskText(task, QStringLiteral("department"));
                    const QString repository =
                        taskText(task, QStringLiteral("repository"));
                    const QJsonObject qa = task.value(QStringLiteral("qa")).toObject();
                    const QString qaStatus = taskText(qa, QStringLiteral("status"));
                    const bool qaRequested =
                        qa.value(QStringLiteral("requestedAt")).toDouble() > 0;
                    auto *item =
                        new QListWidgetItem(taskText(task, QStringLiteral("title")));
                    item->setData(kBoardIdRole, id);
                    item->setData(kBoardShortIdRole, id.left(6));
                    QString project = repository.section(QLatin1Char('/'), -1);
                    if (project.isEmpty() &&
                        department.compare(QLatin1String("general"),
                                           Qt::CaseInsensitive) != 0)
                        project = department;
                    item->setData(kBoardProjectRole, project);
                    if (task.contains(QStringLiteral("priority")))
                        item->setData(
                            kBoardPriorityRole,
                            QStringLiteral("P%1").arg(
                                task.value(QStringLiteral("priority")).toInt()));
                    item->setData(kBoardDoneRole, taskIsClosed(task));
                    const QString kind =
                        taskText(task, QStringLiteral("assigneeKind"));
                    const QString assignee =
                        taskText(task, QStringLiteral("assignee"));
                    QPixmap face;
                    bool working = false;
                    organizationTaskAgentFace(task, &face, &working);
                    QString avatarKind = QStringLiteral("none");
                    if (!face.isNull() || kind == QLatin1String("agent")) {
                        avatarKind = QStringLiteral("agent");
                    } else if (kind == QLatin1String("user") && !assignee.isEmpty()) {
                        avatarKind = QStringLiteral("user");
                        face = taskPersonFace(
                            QVariant(accountPicture(assignee)),
                            assignee, kBoardAvatar, 0.5);
                    }
                    item->setData(kBoardAvatarKindRole, avatarKind);
                    if (!face.isNull())
                        item->setData(kBoardAvatarRole, face);
                    item->setData(kBoardWorkingRole, working);
                    item->setData(kBoardChecklistRole,
                                  tasksBoardChecklist(promptTextWithoutAttachments(
                                      taskText(task, QStringLiteral("details")))));
                    item->setData(kBoardProgressRole,
                                  task.value(QStringLiteral("progress")).toInt());
                    const QList<forkmesh::taskwork::WorkItem> workItems =
                        forkmesh::taskwork::fromJsonArray(
                            task.value(QStringLiteral("workItems")).toArray());
                    QString branch = workItems.isEmpty()
                                         ? QString()
                                         : workItems.constFirst().branch;
                    if (workItems.size() > 1)
                        branch += QStringLiteral(" +%1").arg(workItems.size() - 1);
                    if (branch.isEmpty())
                        branch = localAgentBranchForTask(id);
                    item->setData(kBoardBranchRole, branch);
                    item->setData(
                        kBoardAttachmentsRole,
                        int(task.value(QStringLiteral("attachments")).toArray().size()));
                    item->setData(kBoardQaStateRole,
                                  qaStatus == QLatin1String("passed") ||
                                          qaStatus == QLatin1String("failed")
                                      ? qaStatus
                                  : qaRequested ? QStringLiteral("requested")
                                                : QString());
                    item->setData(kBoardQaRole,
                                  qaStatus == QLatin1String("passed")
                                      ? QStringLiteral("QA passed")
                                  : qaStatus == QLatin1String("failed")
                                      ? QStringLiteral("QA failed")
                                  : qaRequested ? QStringLiteral("In QA")
                                                : QString());
                    item->setData(
                        kBoardAgeRole,
                        taskCreatedAge(qint64(
                            task.value(QStringLiteral("createdAt")).toDouble())));
                    item->setToolTip(taskRowTooltipHtml(task, statusKey, department,
                                                        repository, qaStatus,
                                                        qaRequested));
                    lane.list->addItem(item);
                }
                lane.list->verticalScrollBar()->setValue(scroll);
            }
            ui->syncing = false;
            syncBoardSelection();
        };
    }

    if (m_store && m_store->loaded()) {
        const qint64 fetchedAt = m_store->loadedAt();
        applyOrganizationTasks(m_store->payload());
        m_organizationTasksLoadedAt = fetchedAt;
    }
    updateOrganizationTaskRefreshAge();

    renderOrganizationTaskDetail();
    updateOrganizationTaskActions();
    return page;
}

void TasksPage::requestOrganizationTasks(
    const QByteArray &method, const QString &path, const QJsonObject &body,
    TaskReplyHandler handler, const QList<int> &expectedHttpStatuses)
{
    if (!m_store) {
        handler(false, {}, QStringLiteral(
            "Sign in to an organization account to use private tasks."));
        return;
    }
    const QPointer<TasksPage> page(this);
    m_store->request(
        method, path, body,
        [page, handler = std::move(handler)](bool ok, const QJsonObject &data,
                                             const QString &error) {
            if (page)
                handler(ok, data, error);
        },
        expectedHttpStatuses);
}

void TasksPage::requestOrganizationTaskAttachment(
    const QString &taskId, const QString &attachmentId,
    TaskAttachmentHandler handler)
{
    if (!m_store) {
        handler(false, {}, {}, QStringLiteral("Task image unavailable"));
        return;
    }
    const QPointer<TasksPage> page(this);
    m_store->requestAttachment(
        taskId, attachmentId,
        [page, handler = std::move(handler)](bool ok, const QByteArray &bytes,
                                             const QString &mime,
                                             const QString &error) {
            if (page)
                handler(ok, bytes, mime, error);
        });
}

void TasksPage::refreshOrganizationTasks()
{
    if (!m_store || m_store->loading() || !m_organizationTasksStatus)
        return;
    m_organizationTaskHistory.clear();
    m_organizationTaskHistoryAt.clear();
    m_store->refresh();
}

void TasksPage::onCatalogLoaded(const QJsonObject &payload)
{
    m_organizationTaskHistory.clear();
    m_organizationTaskHistoryAt.clear();
    if (m_organizationTasksStatus) {
        m_organizationTasksStatus->clear();
        applyOrganizationTasks(payload);
    }
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
    if (m_host.searchIndexChanged)
        m_host.searchIndexChanged();
}

void TasksPage::onCatalogFailed(const QString &error)
{
    if (!m_organizationTasksStatus)
        return;
    m_organizationTasksStatus->setText(
        QStringLiteral("Tasks unavailable: %1").arg(error));
    updateOrganizationTaskRefreshAge();
    updateOrganizationTaskActions();
}

void TasksPage::updateOrganizationTaskRefreshAge()
{
    if (!m_organizationTaskRefreshButton)
        return;
    if (m_store && m_store->loading()) {
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

void TasksPage::setOrganizationTaskBadge(int openCount)
{
    if (m_store)
        m_store->setBadgeCount(openCount);
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

void TasksPage::applyOrganizationTasks(const QJsonObject &payload)
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
    const int openCount = openTaskCount(m_organizationTasks);
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

    if (!missingLinkedTaskId.isEmpty()) {
        requestOrganizationTasks(
            QByteArrayLiteral("GET"),
            QStringLiteral("/api/tasks/") + missingLinkedTaskId, {},
            [this, missingLinkedTaskId](bool ok, const QJsonObject &payload,
                                        const QString &error) {
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

bool TasksPage::replaceOrganizationTask(const QJsonObject &task)
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

void TasksPage::overlayOrganizationTaskLocalEdits()
{
    for (int index = 0; index < m_organizationTasks.size() &&
                        !m_organizationTaskLocalEdits.isEmpty();
         ++index) {
        const QJsonObject listed = m_organizationTasks.at(index).toObject();
        const QString id = taskText(listed, QStringLiteral("id"));
        auto local = m_organizationTaskLocalEdits.find(id);
        if (local == m_organizationTaskLocalEdits.end())
            continue;
        if (!m_organizationTaskEditsInFlight.contains(id) &&
            qint64(listed.value(QStringLiteral("updatedAt")).toDouble()) >=
                qint64(local->value(QStringLiteral("updatedAt")).toDouble())) {
            m_organizationTaskLocalEdits.erase(local);
            continue;
        }
        m_organizationTasks.replace(index, *local);
    }
}

void TasksPage::renderOrganizationTaskRows(const QString &selectTaskId)
{
    if (!m_organizationTasksTable)
        return;
    m_localAgentsStale = true;
    publishRepositoryLogos();
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
        const bool closed = taskIsClosed(task);
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
    runTasksNextHook(m_organizationTasksTable, true);
}

void TasksPage::appendOrganizationTaskRows(int count)
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
        if (branch.isEmpty())
            branch = localAgentBranchForTask(taskText(task, QStringLiteral("id")));
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

void TasksPage::refreshOrganizationTaskRowThumbnails(const QString &taskId)
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

bool TasksPage::organizationTaskAgentFace(const QJsonObject &task,
                                          QPixmap *face, bool *working) const
{
    const LocalAgent *session =
        localAgentForTask(taskText(task, QStringLiteral("id")));
    LocalAgent identity;
    bool known = false;
    if (session) {
        identity = *session;
        known = true;
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
        *face = known ? agentFaceIcon(identity.provider, identity.model)
                            .pixmap(kTaskAgentFaceSource, kTaskAgentFaceSource)
                      : QPixmap();
    if (working)
        *working = known && !identity.merged &&
                   identity.status == QLatin1String("running") &&
                   !taskIsClosed(task);
    return known;
}

void TasksPage::applyOrganizationTaskAgentFace(QTableWidgetItem *item,
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

void TasksPage::applyOrganizationTaskPeopleFaces(QTableWidgetItem *item)
{
    if (!item)
        return;
    const auto apply = [this, item](int nameRole, int pictureRole) {
        const QString name = item->data(nameRole).toString().trimmed();
        const QPixmap picture =
            name.isEmpty() ? QPixmap()
                           : accountPicture(name);
        const QPixmap current = item->data(pictureRole).value<QPixmap>();
        if (current.cacheKey() == picture.cacheKey())
            return;
        item->setData(pictureRole,
                      picture.isNull() ? QVariant() : QVariant(picture));
    };
    apply(kTaskCreatorRole, kTaskCreatorPictureRole);
    apply(kTaskAssigneeRole, kTaskAssigneePictureRole);
}

void TasksPage::refreshOrganizationTaskPeopleFaces()
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

void TasksPage::rebuildOrganizationTaskPeopleStrip()
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

void TasksPage::updateOrganizationTaskPeopleStrip(
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
                QVariant(accountPicture(name)), name, 24,
                0.5);
        }
        button->setIcon(QIcon(face));
        button->setText(QString::number(open));
        button->setAccessibleName(label);
        button->setToolTip(QStringLiteral("%1: %2 open").arg(label).arg(open));
        button->setChecked(key == m_organizationTaskAssigneeFilter);
    }
}

void TasksPage::setOrganizationTaskAssigneeFilter(const QString &filter)
{
    m_organizationTaskAssigneeFilter =
        filter.isEmpty() ? QStringLiteral("all") : filter;
    if (!m_organizationTaskPeopleGroup)
        return;
    for (QAbstractButton *button : m_organizationTaskPeopleGroup->buttons())
        button->setChecked(button->property("assigneeFilter").toString() ==
                           m_organizationTaskAssigneeFilter);
}

QJsonObject TasksPage::organizationTaskForRow(int row) const
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

int TasksPage::organizationTaskRowForId(const QString &taskId) const
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

void TasksPage::refreshOrganizationTaskAgentRows()
{
    m_localAgentsStale = true;
    if (!m_organizationTasksTable)
        return;
    for (int row = 0; row < m_organizationTasksTable->rowCount(); ++row)
        applyOrganizationTaskAgentFace(m_organizationTasksTable->item(row, 0),
                                       organizationTaskForRow(row));
    updateOrganizationTaskSpinTimer();
}

bool TasksPage::organizationTaskRowsHaveWorkingAgent() const
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

void TasksPage::updateOrganizationTaskSpinTimer()
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
                [this] { tickOrganizationTaskSpinners(); });
    }
    if (!m_organizationTaskSpinTimer->isActive())
        m_organizationTaskSpinTimer->start();
}

void TasksPage::setOrganizationTaskListView(forkmesh::ui::ListViewMode view)
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

void TasksPage::tickOrganizationTaskSpinners()
{
    if (!m_organizationTasksTable || !m_organizationTasksTable->isVisible()) {
        if (m_organizationTaskSpinTimer)
            m_organizationTaskSpinTimer->stop();
        return;
    }
    m_organizationTaskSpinAngle = std::fmod(
        m_organizationTaskSpinAngle + kTaskAgentSpinStepDegrees, 360.0);
    m_localAgentsStale = true;
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

QPixmap TasksPage::organizationTaskCreatorPicture(
    const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row < 0 ? QPixmap()
                   : m_organizationTasksTable->item(row, 0)
                         ->data(kTaskCreatorPictureRole)
                         .value<QPixmap>();
}

QPixmap TasksPage::organizationTaskAssigneePicture(
    const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row < 0 ? QPixmap()
                   : m_organizationTasksTable->item(row, 0)
                         ->data(kTaskAssigneePictureRole)
                         .value<QPixmap>();
}

QPixmap TasksPage::organizationTaskAgentFacePicture(const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row < 0 ? QPixmap()
                   : m_organizationTasksTable->item(row, 0)
                         ->data(kTaskAgentFaceRole)
                         .value<QPixmap>();
}

bool TasksPage::organizationTaskAgentWorking(const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    return row >= 0 && m_organizationTasksTable->item(row, 0)
                           ->data(kTaskAgentWorkingRole)
                           .toBool();
}

bool TasksPage::organizationTaskSpinTimerActive() const
{
    return m_organizationTaskSpinTimer &&
           m_organizationTaskSpinTimer->isActive();
}

QImage TasksPage::organizationTaskRowImage(const QString &taskId) const
{
    const int row = organizationTaskRowForId(taskId);
    if (row < 0)
        return {};
    const QRect rect = m_organizationTasksTable->visualItemRect(
        m_organizationTasksTable->item(row, 0));
    return m_organizationTasksTable->viewport()->grab(rect).toImage();
}

void TasksPage::updateOrganizationTaskPageLabel()
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

void TasksPage::openCreatedOrganizationTask(const QJsonObject &task)
{
    const QString taskId = taskText(task, QStringLiteral("id"));
    ensureBuilt();
    if (!m_organizationTasksTable || taskId.isEmpty()) {
        showPage();
        if (m_organizationTasksLoaded)
            refreshOrganizationTasks();
        return;
    }

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

    m_organizationTasksSearchMirror.clear();
    if (m_organizationTaskMineFilterButton) {
        const QSignalBlocker blocker(m_organizationTaskMineFilterButton);
        m_organizationTaskMineFilterButton->setChecked(false);
    }
    setOrganizationTaskAssigneeFilter(QStringLiteral("all"));
    revealOrganizationTaskStatus(task);
    m_organizationTaskShownRows = 0;
    renderOrganizationTaskRows(taskId);

    const int openCount = openTaskCount(m_organizationTasks);
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
    showPage();
    if (QTableWidgetItem *item = m_organizationTasksTable->currentItem())
        m_organizationTasksTable->scrollToItem(item);
}

void TasksPage::renderOrganizationTaskDetail()
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
                       m_organizationTaskHistory.value(taskId),
                       Theme::isNext() && m_organizationTaskFields &&
                               m_organizationTaskFields->isHidden()
                           ? taskFieldsHtml(task,
                                            localAgentRunLabelForTask(taskId),
                                            sessionId)
                           : QString()));
    if (m_organizationTaskFields) {
        m_organizationTaskFields->setHtml(taskFieldsHtml(
            task, localAgentRunLabelForTask(taskId), sessionId));
        if (m_organizationTaskSide)
            m_organizationTaskSide->setVisible(!task.isEmpty());
    }
    syncOrganizationTaskProperties(task);
    if (m_organizationTaskQuickComment &&
        taskId != m_organizationTaskQuickCommentTaskId) {
        const QString draft = m_organizationTaskQuickComment->toPlainText();
        if (!m_organizationTaskQuickCommentTaskId.isEmpty()) {
            if (draft.trimmed().isEmpty())
                m_organizationTaskCommentDrafts.remove(
                    m_organizationTaskQuickCommentTaskId);
            else
                m_organizationTaskCommentDrafts.insert(
                    m_organizationTaskQuickCommentTaskId, draft);
        }
        m_organizationTaskQuickCommentTaskId = taskId;
        m_organizationTaskQuickComment->setPlainText(
            m_organizationTaskCommentDrafts.value(taskId));
    }
    if (!taskId.isEmpty() &&
        (!m_organizationTaskHistory.contains(taskId) ||
         QDateTime::currentMSecsSinceEpoch() -
                 m_organizationTaskHistoryAt.value(taskId) >
             kOrganizationTaskHistoryStaleMs))
        loadOrganizationTaskHistory(taskId);
    runTasksNextHook(m_organizationTasksTable, false);
}

QList<TasksPage::LocalAgent> TasksPage::localAgents() const
{
    if (!m_localAgentsStale)
        return m_localAgents;
    m_localAgentsStale = false;
    m_localAgents.clear();
    for (const QJsonValue &value :
         windowQuery(QStringLiteral("task.agentSessions"), {}).toArray()) {
        const QJsonObject row = value.toObject();
        LocalAgent agent;
        agent.id = row.value(QStringLiteral("id")).toInt();
        agent.orgTaskId = row.value(QStringLiteral("orgTaskId")).toString();
        agent.createdAtMs =
            qint64(row.value(QStringLiteral("createdAtMs")).toDouble());
        agent.provider = row.value(QStringLiteral("provider")).toString();
        agent.providerName = row.value(QStringLiteral("providerName")).toString();
        agent.model = row.value(QStringLiteral("model")).toString();
        agent.status = row.value(QStringLiteral("status")).toString();
        agent.branch = row.value(QStringLiteral("branch")).toString();
        agent.merged = row.value(QStringLiteral("merged")).toBool();
        m_localAgents.append(agent);
    }
    return m_localAgents;
}

const TasksPage::LocalAgent *TasksPage::localAgentForTask(
    const QString &taskId) const
{
    if (taskId.isEmpty())
        return nullptr;
    localAgents();
    const LocalAgent *newest = nullptr;
    for (const LocalAgent &session : std::as_const(m_localAgents)) {
        if (session.orgTaskId != taskId || session.id <= 0)
            continue;
        if (!newest || session.createdAtMs > newest->createdAtMs)
            newest = &session;
    }
    return newest;
}

int TasksPage::localAgentSessionIdForTask(const QString &taskId) const
{
    const LocalAgent *session = localAgentForTask(taskId);
    return session ? session->id : 0;
}

QString TasksPage::localAgentBranchForTask(const QString &taskId) const
{
    const LocalAgent *session = localAgentForTask(taskId);
    return session ? session->branch : QString();
}

void TasksPage::loadOrganizationTaskHistory(const QString &taskId)
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

QString TasksPage::localAgentRunLabelForTask(const QString &taskId) const
{
    const LocalAgent *session = localAgentForTask(taskId);
    if (!session)
        return QString();
    QStringList parts;
    parts << QStringLiteral("#%1").arg(session->id);
    if (!session->providerName.isEmpty())
        parts << session->providerName;
    if (!session->status.isEmpty())
        parts << session->status;
    return parts.join(QStringLiteral(", ")) +
           QStringLiteral(" (this desktop)");
}

void TasksPage::openOrganizationTask(const QString &taskId)
{
    if (taskId.isEmpty())
        return;
    m_pendingOrganizationTaskSelection = taskId;
    ensureBuilt();
    showPage();
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

void TasksPage::revealOrganizationTaskStatus(const QJsonObject &task)
{
    const bool closed = taskIsClosed(task);
    if (m_organizationTaskStatusFilter == QLatin1String("all") ||
        (m_organizationTaskStatusFilter == QLatin1String("closed")) == closed)
        return;
    m_organizationTaskStatusFilter = QStringLiteral("all");
    if (m_organizationTaskAllFilterButton)
        m_organizationTaskAllFilterButton->setChecked(true);
}

void TasksPage::updateOrganizationTaskActions()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const bool selected = !task.isEmpty();
    const bool done = taskIsClosed(task);
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
            if (taskIsClosed(value.toObject()))
                ++closedCount;
        }
        m_organizationTaskDeleteClosedButton->setVisible(
            m_organizationTasksCanManage);
        m_organizationTaskDeleteClosedButton->setEnabled(
            m_organizationTasksCanManage && closedCount > 0 &&
            !m_organizationTaskClosedDeleteInFlight &&
            !(m_store && m_store->loading()));
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
    if (m_organizationTaskCommentButton)
        m_organizationTaskCommentButton->setEnabled(selected);
    if (m_organizationTaskQuickComment)
        m_organizationTaskQuickComment->setEnabled(selected);
    if (m_organizationTaskQuickCommentSend)
        m_organizationTaskQuickCommentSend->setEnabled(selected);
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

void TasksPage::addOrganizationTaskToPrompt()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    if (task.isEmpty())
        return;
    if (!windowCommand(QStringLiteral("prompt.append"),
                       {{QStringLiteral("text"), organizationTaskPromptText(task)}}))
        return;
    if (m_organizationTasksStatus)
        m_organizationTasksStatus->setText(
            QStringLiteral("Task added to the prompt box."));
}

void TasksPage::startOrganizationTaskAgentFromPrompt()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    if (task.isEmpty()) {
        showStatus(QStringLiteral("Select a task to start an agent."));
        flashMessage(QStringLiteral("Select a task to start an agent."), true);
        return;
    }
    if (!m_host.windowRequest)
        return;
    const QPointer<TasksPage> page(this);
    m_host.windowRequest(
        QStringLiteral("task.startAgent"),
        {{QStringLiteral("task"), task},
         {QStringLiteral("prompt"), organizationTaskPromptText(task)}},
        [page](bool ok, const QJsonObject &reply, const QString &error) {
            if (!page)
                return;
            const QString message =
                ok ? reply.value(QStringLiteral("message")).toString() : error;
            if (!message.isEmpty())
                page->showStatus(message);
            if (ok)
                page->renderOrganizationTaskDetail();
        });
}

void TasksPage::attachExistingWorktreeToOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString taskId = taskText(task, QStringLiteral("id"));
    if (taskId.isEmpty())
        return;
    windowCommand(QStringLiteral("task.attachWorktree"),
                  {{QStringLiteral("taskId"), taskId},
                   {QStringLiteral("repository"),
                    taskText(task, QStringLiteral("repository"))}});
}

void TasksPage::createOrganizationTask()
{
    composeOrganizationTask(QStringLiteral("New organization task"), QString(),
                            QString(), nullptr);
}

void TasksPage::composeOrganizationTask(const QString &windowTitle,
                                         const QString &prefillTitle,
                                         const QString &prefillDetails,
                                         std::function<void()> onCreated)
{
    QDialog dialog(this);
    dialog.setWindowTitle(windowTitle);
    auto *layout = new QFormLayout(&dialog);
    auto *title = new QLineEdit(prefillTitle);
    title->setMaxLength(kTaskTitleLimit);
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
                if (onCreated)
                    reportError(failure);
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

void TasksPage::createOrganizationTaskFollowUp()
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

void TasksPage::moveOrganizationTaskToIssue()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString taskId = taskText(task, QStringLiteral("id"));
    if (taskId.isEmpty() || !m_organizationTasksCanManage ||
        m_organizationTaskIssueMoveInFlight || !m_host.windowRequest)
        return;

    const QString status = taskText(task, QStringLiteral("status"));
    if (status == QLatin1String("active") ||
        task.value(QStringLiteral("completedAt")).toDouble() > 0 ||
        status == QLatin1String("done")) {
        reportError(QStringLiteral(
            "Stop or reopen the task before moving it into an issue."));
        return;
    }

    auto removeTask = [this, taskId](int issueNumber,
                                     const QString &issueNotice) {
        requestOrganizationTasks(
            QByteArrayLiteral("DELETE"),
            QStringLiteral("/api/tasks/") + taskId, {},
            [this, taskId, issueNumber, issueNotice](
                bool ok, const QJsonObject &data, const QString &error) {
                m_organizationTaskIssueMoveInFlight = false;
                if (ok)
                    cleanupOrganizationTaskWork(
                        taskId,
                        data.value(QStringLiteral("workItems")).toArray(), 0);
                if (!ok) {
                    const QString failure =
                        QStringLiteral("%1 The task could not be removed: %2")
                            .arg(issueNotice, error);
                    if (m_organizationTasksStatus)
                        m_organizationTasksStatus->setText(failure);
                    reportError(failure);
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
                if (issueNumber > 0)
                    windowCommand(QStringLiteral("issue.show"),
                                  {{QStringLiteral("number"), issueNumber}});
            });
    };

    m_organizationTaskIssueMoveInFlight = true;
    updateOrganizationTaskActions();
    const QPointer<TasksPage> page(this);
    m_host.windowRequest(
        QStringLiteral("task.fileAsIssue"),
        {{QStringLiteral("task"), task},
         {QStringLiteral("body"), organizationTaskIssueBody(task)}},
        [page, removeTask](bool ok, const QJsonObject &reply,
                           const QString &error) {
            if (!page)
                return;
            if (!ok) {
                page->m_organizationTaskIssueMoveInFlight = false;
                if (!reply.value(QStringLiteral("cancelled")).toBool()) {
                    if (page->m_organizationTasksStatus)
                        page->m_organizationTasksStatus->setText(error);
                    page->reportError(error);
                }
                page->updateOrganizationTaskActions();
                return;
            }
            removeTask(reply.value(QStringLiteral("issueNumber")).toInt(),
                       reply.value(QStringLiteral("notice")).toString());
        });
}

void TasksPage::editOrganizationTask()
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
    QJsonObject body{
        {QStringLiteral("title"), title->text().trimmed()},
        {QStringLiteral("details"), details->toPlainText().trimmed()},
        {QStringLiteral("repository"), repository->text().trimmed()},
        {QStringLiteral("priority"), priority->value()},
        {QStringLiteral("assigneeKind"),
         assignment == QLatin1String("agent") ? QStringLiteral("agent")
                     : assignment == QLatin1String("unassigned")
                         ? QStringLiteral("unassigned")
                         : QStringLiteral("user")},
    };
    if (assignment.startsWith(QLatin1String("user:")))
        body.insert(QStringLiteral("assignee"), assignment.mid(5));
    patchOrganizationTask(task, body);
}

void TasksPage::patchOrganizationTask(const QJsonObject &task,
                                       const QJsonObject &body)
{
    const QString taskId = taskText(task, QStringLiteral("id"));
    if (taskId.isEmpty() || body.isEmpty())
        return;
    const bool assignAgent =
        body.value(QStringLiteral("assigneeKind")).toString() ==
        QLatin1String("agent");
    const QString path = QStringLiteral("/api/tasks/") + taskId;

    QJsonObject optimistic = task;
    for (auto it = body.constBegin(); it != body.constEnd(); ++it)
        optimistic.insert(it.key(), it.value());
    if (body.contains(QStringLiteral("assigneeKind")) &&
        !body.contains(QStringLiteral("assignee")))
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

void TasksPage::syncOrganizationTaskProperties(const QJsonObject &task)
{
    if (!m_organizationTaskProperties)
        return;
    const QString taskId = taskText(task, QStringLiteral("id"));
    const bool sameTask = taskId == m_organizationTaskPropertiesTaskId;
    if (!sameTask && !m_organizationTaskPropertiesTaskId.isEmpty()) {
        for (const QString &field :
             {QStringLiteral("team"), QStringLiteral("repository"),
              QStringLiteral("parent")})
            commitOrganizationTaskProperty(field);
    }
    m_organizationTaskPropertiesTaskId = taskId;
    if (task.isEmpty())
        return;
    const QString statusKey = taskStatusKey(task);
    const bool done = statusKey == QLatin1String("done");
    const bool active = statusKey == QLatin1String("active");
    const QString actor = m_organizationTaskActor;
    const QString createdBy = taskText(task, QStringLiteral("createdBy"));
    const bool mine = organizationTaskAssignedTo(task, actor);
    const bool creator =
        !actor.isEmpty() && createdBy.compare(actor, Qt::CaseInsensitive) == 0;
    const bool reviewer =
        !actor.isEmpty() &&
        taskText(task.value(QStringLiteral("qa")).toObject(),
                 QStringLiteral("reviewer"))
                .compare(actor, Qt::CaseInsensitive) == 0;
    const bool manage = m_organizationTasksCanManage;
    const bool editable = manage && !active && !done;
    const QString locked =
        !manage ? QStringLiteral("Only task managers can change this")
        : active ? QStringLiteral("Stop the task to change this")
        : done   ? QStringLiteral("Mark the task undone to change this")
                 : QString();

    {
        const QSignalBlocker blocker(m_organizationTaskStatusEdit);
        const QString shown = done ? QStringLiteral("done")
                              : active ? QStringLiteral("active")
                                       : QStringLiteral("ready");
        m_organizationTaskStatusEdit->setCurrentIndex(
            qMax(0, m_organizationTaskStatusEdit->findData(shown)));
        auto *model = qobject_cast<QStandardItemModel *>(
            m_organizationTaskStatusEdit->model());
        const bool canUndo = done && (manage || mine || creator || reviewer);
        const bool canStart = !done && mine;
        const bool canComplete = !done && (mine || manage);
        if (model) {
            model->item(0)->setEnabled(shown == QLatin1String("ready") ||
                                       canUndo || (active && canStart));
            model->item(1)->setEnabled(shown == QLatin1String("active") ||
                                       canStart);
            model->item(2)->setEnabled(done || canComplete);
        }
        m_organizationTaskStatusEdit->setEnabled(canUndo || canStart ||
                                                 canComplete);
        m_organizationTaskStatusEdit->setToolTip(
            QStringLiteral("Start, stop, complete or reopen the task"));
    }
    if (!sameTask || !m_organizationTaskPriorityEdit->hasFocus()) {
        const QSignalBlocker blocker(m_organizationTaskPriorityEdit);
        m_organizationTaskPriorityEdit->setValue(
            task.value(QStringLiteral("priority")).toInt(50));
    }
    m_organizationTaskPriorityEdit->setEnabled(manage);
    if (!sameTask || !m_organizationTaskProgressEdit->hasFocus()) {
        const QSignalBlocker blocker(m_organizationTaskProgressEdit);
        m_organizationTaskProgressEdit->setValue(
            done ? 100 : qBound(0, task.value(QStringLiteral("progress")).toInt(),
                                100));
    }
    m_organizationTaskProgressEdit->setEnabled(!done &&
                                               (manage || mine || creator));

    {
        const QSignalBlocker blocker(m_organizationTaskAssigneeEdit);
        m_organizationTaskAssigneeEdit->clear();
        m_organizationTaskAssigneeEdit->addItem(QStringLiteral("Unassigned"),
                                                QStringLiteral("unassigned"));
        m_organizationTaskAssigneeEdit->addItem(QStringLiteral("Bot"),
                                                QStringLiteral("agent"));
        QStringList members = m_organizationTaskMembers;
        const QString assignee =
            taskText(task, QStringLiteral("assignee")).toLower();
        const QString kind = taskText(task, QStringLiteral("assigneeKind"));
        if (kind == QLatin1String("user") && !assignee.isEmpty() &&
            !members.contains(assignee))
            members.append(assignee);
        for (const QString &member : std::as_const(members))
            m_organizationTaskAssigneeEdit->addItem(
                QStringLiteral("@") + member, QStringLiteral("user:") + member);
        const QString current =
            kind == QLatin1String("agent") ? QStringLiteral("agent")
            : kind == QLatin1String("user") && !assignee.isEmpty()
                ? QStringLiteral("user:") + assignee
                : QStringLiteral("unassigned");
        m_organizationTaskAssigneeEdit->setCurrentIndex(
            qMax(0, m_organizationTaskAssigneeEdit->findData(current)));
    }
    {
        const QSignalBlocker blocker(m_organizationTaskDepartmentEdit);
        m_organizationTaskDepartmentEdit->clear();
        QStringList departments =
            m_organizationTaskDepartments.isEmpty()
                ? QStringList{QStringLiteral("general"),
                              QStringLiteral("engineering")}
                : m_organizationTaskDepartments;
        const QString department =
            taskText(task, QStringLiteral("department")).toLower();
        if (!department.isEmpty() && !departments.contains(department))
            departments.append(department);
        m_organizationTaskDepartmentEdit->addItems(departments);
        m_organizationTaskDepartmentEdit->setCurrentIndex(qMax(
            0, m_organizationTaskDepartmentEdit->findText(
                   department.isEmpty() ? QStringLiteral("general")
                                        : department)));
    }
    const auto syncText = [sameTask](QLineEdit *edit, const QString &value) {
        if (sameTask && edit->hasFocus())
            return;
        edit->setText(value);
    };
    syncText(m_organizationTaskTeamEdit, taskText(task, QStringLiteral("team")));
    syncText(m_organizationTaskRepositoryEdit,
             taskText(task, QStringLiteral("repository")));
    syncText(m_organizationTaskParentEdit,
             taskText(task, QStringLiteral("parentTaskId")));
    for (QWidget *editor :
         {static_cast<QWidget *>(m_organizationTaskAssigneeEdit),
          static_cast<QWidget *>(m_organizationTaskDepartmentEdit),
          static_cast<QWidget *>(m_organizationTaskTeamEdit),
          static_cast<QWidget *>(m_organizationTaskRepositoryEdit),
          static_cast<QWidget *>(m_organizationTaskParentEdit)}) {
        editor->setEnabled(editable);
        editor->setToolTip(locked);
    }
}

void TasksPage::commitOrganizationTaskProperty(const QString &field)
{
    if (field.isEmpty() || m_organizationTaskPropertiesTaskId.isEmpty() ||
        !m_organizationTaskProperties)
        return;
    QJsonObject task;
    for (const QJsonValue &value : std::as_const(m_organizationTasks)) {
        if (taskText(value.toObject(), QStringLiteral("id")) ==
            m_organizationTaskPropertiesTaskId) {
            task = value.toObject();
            break;
        }
    }
    if (task.isEmpty())
        return;
    const QString id = m_organizationTaskPropertiesTaskId;
    if (field == QLatin1String("status")) {
        const QString key = taskStatusKey(task);
        const QString current =
            key == QLatin1String("done") || key == QLatin1String("active")
                ? key
                : QStringLiteral("ready");
        const QString target =
            m_organizationTaskStatusEdit->currentData().toString();
        if (target == current)
            return;
        sendOrganizationTaskAction(
            id,
            target == QLatin1String("done")     ? QStringLiteral("complete")
            : current == QLatin1String("done")  ? QStringLiteral("undo")
            : target == QLatin1String("active") ? QStringLiteral("start")
                                                : QStringLiteral("stop"),
            {});
        return;
    }
    QJsonObject body;
    const auto changeText = [&](const QString &key, QString value) {
        value = value.trimmed();
        if (value != taskText(task, key))
            body.insert(key, value);
    };
    if (field == QLatin1String("priority")) {
        const int value = m_organizationTaskPriorityEdit->value();
        if (value != task.value(QStringLiteral("priority")).toInt(50))
            body.insert(QStringLiteral("priority"), value);
    } else if (field == QLatin1String("progress")) {
        const int value = m_organizationTaskProgressEdit->value();
        if (!taskIsClosed(task) &&
            value != task.value(QStringLiteral("progress")).toInt())
            body.insert(QStringLiteral("progress"), value);
    } else if (field == QLatin1String("assignee")) {
        const QString assignment =
            m_organizationTaskAssigneeEdit->currentData().toString();
        const QString kind = taskText(task, QStringLiteral("assigneeKind"));
        const QString assignee =
            taskText(task, QStringLiteral("assignee")).toLower();
        const QString current =
            kind == QLatin1String("agent") ? QStringLiteral("agent")
            : kind == QLatin1String("user") && !assignee.isEmpty()
                ? QStringLiteral("user:") + assignee
                : QStringLiteral("unassigned");
        if (!assignment.isEmpty() && assignment != current) {
            body.insert(QStringLiteral("assigneeKind"),
                        assignment.startsWith(QLatin1String("user:"))
                            ? QStringLiteral("user")
                            : assignment);
            if (assignment.startsWith(QLatin1String("user:")))
                body.insert(QStringLiteral("assignee"), assignment.mid(5));
        }
    } else if (field == QLatin1String("department")) {
        changeText(QStringLiteral("department"),
                   m_organizationTaskDepartmentEdit->currentText());
    } else if (field == QLatin1String("team")) {
        changeText(QStringLiteral("team"),
                   m_organizationTaskTeamEdit->text().toLower());
    } else if (field == QLatin1String("repository")) {
        changeText(QStringLiteral("repository"),
                   m_organizationTaskRepositoryEdit->text());
    } else if (field == QLatin1String("parent")) {
        changeText(QStringLiteral("parentTaskId"),
                   m_organizationTaskParentEdit->text().toLower());
    }
    patchOrganizationTask(task, body);
}

bool TasksPage::settleOrganizationTaskEdit(const QString &taskId,
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
    *updated = !lastEdit;
    if (lastEdit && !changed.isEmpty()) {
        m_organizationTaskLocalEdits.insert(taskId, changed);
        *updated = replaceOrganizationTask(changed);
    } else if (lastEdit) {
        m_organizationTaskLocalEdits.remove(taskId);
    }
    return true;
}

bool TasksPage::organizationTaskCheckboxToggleable(int index) const
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    return !task.isEmpty() && m_organizationTasksCanManage &&
           !taskIsClosed(task) &&
           taskText(task, QStringLiteral("status")) !=
               QLatin1String("active") &&
           !taskCheckboxToggle(task, index).text.isNull();
}

void TasksPage::toggleOrganizationTaskCheckbox(int index)
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

void TasksPage::assignOrganizationTaskToAgent()
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

void TasksPage::queueOrganizationTaskAgent(const QJsonObject &task)
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

void TasksPage::runOrganizationTaskAction(const QString &action)
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
    sendOrganizationTaskAction(id, action, body);
}

void TasksPage::sendOrganizationTaskAction(const QString &id,
                                            const QString &action,
                                            const QJsonObject &body)
{
    if (id.isEmpty())
        return;
    requestOrganizationTasks(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/tasks/%1/%2").arg(id, action), body,
        [this, action](bool ok, const QJsonObject &, const QString &error) {
            m_organizationTasksStatus->setText(
                ok ? QStringLiteral("Task action completed: %1.").arg(action)
                   : QStringLiteral("Task action failed: %1").arg(error));
            if (ok)
                refreshOrganizationTasks();
            else
                renderOrganizationTaskDetail();
        });
}

void TasksPage::commentOnOrganizationTask()
{
    if (m_organizationTaskQuickComment)
        m_organizationTaskQuickComment->setFocus();
}

void TasksPage::submitOrganizationTaskQuickComment()
{
    if (!m_organizationTaskQuickComment)
        return;
    const QString body = m_organizationTaskQuickComment->toPlainText().trimmed();
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    if (body.isEmpty() || task.isEmpty())
        return;
    m_organizationTaskQuickComment->clear();
    m_organizationTaskCommentDrafts.remove(taskText(task, QStringLiteral("id")));
    postOrganizationTaskComment(body);
}

void TasksPage::postOrganizationTaskComment(const QString &body)
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty() || body.trimmed().isEmpty())
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
                if (m_organizationTaskQuickComment &&
                    m_organizationTaskQuickCommentTaskId == id &&
                    m_organizationTaskQuickComment->toPlainText()
                        .trimmed()
                        .isEmpty())
                    m_organizationTaskQuickComment->setPlainText(body);
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

void TasksPage::checkInOnOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return;
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

void TasksPage::remindOrganizationTaskAssignee()
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

void TasksPage::deleteOrganizationTask()
{
    const QJsonObject task = selectedOrganizationTask(
        m_organizationTasksTable, m_organizationTasks);
    const QString id = taskText(task, QStringLiteral("id"));
    if (id.isEmpty())
        return;
    if (!m_host.windowRequest) {
        sendOrganizationTaskDelete(id, 0);
        return;
    }
    const QString title = taskText(task, QStringLiteral("title"));
    if (m_organizationTasksStatus)
        m_organizationTasksStatus->setText(QString::fromUtf8(
            "Checking this task's worktrees\xE2\x80\xA6"));
    const QPointer<TasksPage> page(this);
    m_host.windowRequest(
        QStringLiteral("task.workPlan"),
        {{QStringLiteral("taskId"), id},
         {QStringLiteral("workItems"),
          task.value(QStringLiteral("workItems")).toArray()}},
        [page, id, title](bool, const QJsonObject &plan, const QString &) {
            if (!page)
                return;
            page->showWorkPlanForDelete(id, title, plan);
        });
}

void TasksPage::showWorkPlanForDelete(const QString &id, const QString &title,
                                      const QJsonObject &plan)
{
    if (m_organizationTasksStatus)
        m_organizationTasksStatus->clear();
    if (plan.value(QStringLiteral("items")).toInt() <= 0) {
        sendOrganizationTaskDelete(id, 0);
        return;
    }
    QStringList losses;
    QStringList removable;
    for (const QJsonValue &value : plan.value(QStringLiteral("losses")).toArray())
        losses.append(value.toString());
    for (const QJsonValue &value :
         plan.value(QStringLiteral("removable")).toArray())
        removable.append(value.toString());
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
}

void TasksPage::sendOrganizationTaskDelete(const QString &taskId,
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
                && data.contains(kTaskBackoffKey)
                && m_deleteRetries.value(taskId)
                       < kOrganizationTaskDeleteMaxRetries) {
                m_deleteRetries[taskId] += 1;
                const qint64 waitMs = qBound(
                    qint64(1000),
                    qint64(data.value(kTaskBackoffKey).toDouble()) + 500,
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
            m_deleteRetries.remove(taskId);
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
                cleanupOrganizationTaskWork(taskId, items, cleanupMode);
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

void TasksPage::deleteClosedOrganizationTasks()
{
    if (!m_organizationTasksCanManage ||
        m_organizationTaskClosedDeleteInFlight)
        return;

    QStringList taskIds;
    QStringList awaitingQaIds;
    for (const QJsonValue &value : std::as_const(m_organizationTasks)) {
        const QJsonObject task = value.toObject();
        if (!taskIsClosed(task))
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

void TasksPage::deleteNextClosedOrganizationTask()
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
                    cleanupOrganizationTaskWork(
                        id, data.value(QStringLiteral("workItems")).toArray(), 0);
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
            if (data.contains(kTaskBackoffKey) &&
                m_organizationTaskClosedDeleteRetries <
                    kOrganizationTaskDeleteMaxRetries) {
                ++m_organizationTaskClosedDeleteRetries;
                m_organizationTaskClosedDeleteIds.prepend(id);
                const qint64 waitMs = qBound(
                    qint64(1000),
                    qint64(data.value(kTaskBackoffKey).toDouble()) +
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

void TasksPage::finishClosedOrganizationTaskDelete()
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
    if (m_organizationTasksStatus && m_organizationTasksStatus->isVisible()) {
        if (m_host.logActivity)
            m_host.logActivity(summary);
    } else {
        flashMessage(summary, moved == 0 && kept > 0);
    }
    renderOrganizationTaskRows();
    if (m_store)
        m_store->handleEvent();
    updateOrganizationTaskActions();
}

TasksPage::TasksPage(const forkmesh::market::NativePackageHost &host,
                     QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host),
      m_store(OrganizationTaskStore::current())
{
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(0);
    if (!m_store)
        return;
    connect(m_store, &OrganizationTaskStore::catalogLoaded, this,
            [this](const QJsonObject &payload) { onCatalogLoaded(payload); });
    connect(m_store, &OrganizationTaskStore::catalogFailed, this,
            [this](const QString &error) { onCatalogFailed(error); });
    connect(m_store, &OrganizationTaskStore::loadingChanged, this, [this] {
        updateOrganizationTaskActions();
        updateOrganizationTaskRefreshAge();
    });
    connect(m_store, &OrganizationTaskStore::taskChanged, this,
            [this](const QJsonObject &task) {
                const QString id = taskText(task, QStringLiteral("id"));
                for (int index = 0; index < m_organizationTasks.size(); ++index) {
                    if (taskText(m_organizationTasks.at(index).toObject(),
                                 QStringLiteral("id")) != id)
                        continue;
                    QJsonObject updated = m_organizationTasks.at(index).toObject();
                    updated.insert(QStringLiteral("workItems"),
                                   task.value(QStringLiteral("workItems")));
                    m_organizationTasks.replace(index, updated);
                    return;
                }
            });
}

void TasksPage::ensureBuilt()
{
    if (m_section)
        return;
    m_section = buildOrganizationTasksSection();
    m_layout->addWidget(m_section);
}

void TasksPage::refresh()
{
    ensureBuilt();
    if (!m_organizationTasksLoaded)
        refreshOrganizationTasks();
    refreshOrganizationTaskAgentRows();
    if (m_host.userDirectory)
        m_host.userDirectory();
}

void TasksPage::setHostSearchQuery(const QString &query)
{
    const QString trimmed = query.trimmed();
    updateOrganizationTaskRefreshAge();
    if (m_organizationTasksSearchMirror == trimmed)
        return;
    m_organizationTasksSearchMirror = trimmed;
    m_organizationTaskShownRows = 0;
    renderOrganizationTaskRows();
}

QList<forkmesh::market::NativePackageSearchHit>
TasksPage::searchHits(const QString &rawQuery, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString query = rawQuery.trimmed();
    if (query.isEmpty())
        return hits;
    const auto contains = [&query](const QString &text) {
        return !text.isEmpty() && text.contains(query, Qt::CaseInsensitive);
    };
    const auto snippet = [&query](const QString &text) {
        QString flat = text;
        flat.replace(QLatin1Char('\n'), QLatin1Char(' '));
        flat = flat.simplified();
        const int at = int(flat.indexOf(query, 0, Qt::CaseInsensitive));
        if (at < 0)
            return flat.left(120);
        const int start = qMax(0, at - 40);
        QString out = flat.mid(start, 140);
        if (start > 0)
            out.prepend(QString::fromUtf8("\xE2\x80\xA6"));
        if (start + 140 < flat.size())
            out.append(QString::fromUtf8("\xE2\x80\xA6"));
        return out;
    };
    static const char *const fields[] = {
        "title", "details", "assignee", "repository", "completionNote",
        "department", "status", "id"};
    const QJsonArray tasks =
        m_section ? m_organizationTasks : (m_store ? m_store->tasks() : QJsonArray());
    const QString dot = QString::fromUtf8("  \xC2\xB7  ");
    for (const QJsonValue &value : tasks) {
        if (hits.size() >= limit)
            break;
        const QJsonObject task = value.toObject();
        const QString id = taskText(task, QStringLiteral("id"));
        if (id.isEmpty())
            continue;
        int field = -1;
        for (int index = 0; index < int(std::size(fields)); ++index) {
            if (contains(task.value(QLatin1String(fields[index])).toString())) {
                field = index;
                break;
            }
        }
        if (field < 0 &&
            contains(task.value(QStringLiteral("qa"))
                         .toObject()
                         .value(QStringLiteral("howToTest"))
                         .toString()))
            field = 8;
        if (field < 0)
            continue;
        QString title = taskText(task, QStringLiteral("title"));
        if (title.isEmpty())
            title = id;
        QStringList detail;
        if (field == 1 || field == 4)
            detail << snippet(task.value(QLatin1String(
                                             field == 1 ? "details"
                                                        : "completionNote"))
                                  .toString());
        const QString status = taskText(task, QStringLiteral("status"));
        if (!status.isEmpty())
            detail << status;
        hits.append({id, title, detail.join(dot)});
    }
    return hits;
}

void TasksPage::activateSearchHit(const QString &id)
{
    if (id.isEmpty())
        showPage();
    else
        openOrganizationTask(id);
}

void TasksPage::prepareSearch()
{
    if (m_store)
        m_store->refreshOnce();
}

int TasksPage::navigationBadge() const
{
    return m_store ? m_store->badgeCount() : 0;
}

bool TasksPage::acceptsHostCommand(const QString &command) const
{
    return command == QLatin1String("task.open") ||
           command == QLatin1String("task.created") ||
           command == QLatin1String("task.composeFromText");
}

bool TasksPage::hostCommand(const QString &command, const QJsonObject &args)
{
    if (command == QLatin1String("task.open")) {
        openOrganizationTask(args.value(QStringLiteral("id")).toString());
        return true;
    }
    if (command == QLatin1String("task.created")) {
        openCreatedOrganizationTask(args.value(QStringLiteral("task")).toObject());
        return true;
    }
    if (command == QLatin1String("task.composeFromText")) {
        ensureBuilt();
        const QString done = args.value(QStringLiteral("done")).toString();
        composeOrganizationTask(
            args.value(QStringLiteral("heading")).toString(),
            args.value(QStringLiteral("title")).toString(),
            args.value(QStringLiteral("details")).toString(),
            [this, done] {
                if (!done.isEmpty())
                    flashMessage(done);
                showPage();
            });
        return true;
    }
    return false;
}

void TasksPage::agentSessionsChanged()
{
    refreshOrganizationTaskAgentRows();
}

void TasksPage::accountPicturesChanged()
{
    refreshOrganizationTaskPeopleFaces();
}

void TasksPage::themeChanged()
{
    renderOrganizationTaskDetail();
}

void TasksPage::reloadOrganizationTaskListView()
{
    setOrganizationTaskListView(forkmesh::ui::loadListViewMode(
        QStringLiteral("tasks/listView"), forkmesh::ui::ListViewMode::Full));
}

QPixmap TasksPage::accountPicture(const QString &account) const
{
    return m_host.accountPicture ? m_host.accountPicture(account) : QPixmap();
}

void TasksPage::flashMessage(const QString &text, bool error)
{
    if (m_host.flashMessage)
        m_host.flashMessage(text, error);
}

void TasksPage::reportError(const QString &text)
{
    if (m_host.reportError)
        m_host.reportError(text);
}

void TasksPage::showStatus(const QString &text)
{
    if (m_organizationTasksStatus)
        m_organizationTasksStatus->setText(text);
}

void TasksPage::showPage()
{
    if (m_host.showPage)
        m_host.showPage();
}

bool TasksPage::windowCommand(const QString &command, const QJsonObject &args)
{
    return m_host.windowCommand && m_host.windowCommand(command, args);
}

QJsonValue TasksPage::windowQuery(const QString &command,
                                  const QJsonObject &args) const
{
    return m_host.windowQuery ? m_host.windowQuery(command, args) : QJsonValue();
}

void TasksPage::publishRepositoryLogos()
{
    publishTaskRepositoryLogos(
        windowQuery(QStringLiteral("task.repositoryLogos"), {}).toArray());
}

void TasksPage::cleanupOrganizationTaskWork(const QString &taskId,
                                            const QJsonArray &workItems,
                                            int mode)
{
    windowCommand(QStringLiteral("task.cleanupWork"),
                  {{QStringLiteral("taskId"), taskId},
                   {QStringLiteral("workItems"), workItems},
                   {QStringLiteral("mode"), mode}});
}

namespace {

const forkmesh::market::NativePackageRegistration tasksRegistration({
    QStringLiteral("builtin:tasks"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Tasks"),
    QStringLiteral("list-unordered"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new TasksPage(host, parent);
    },
    QString(),
    nullptr,
    {},
    {},
});

}

}
