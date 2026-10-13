#include "MirrorActionsSummary.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cerrno>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forkmesh::mirror_actions {
namespace {

constexpr qint64 kMaximumSafeJsonInteger = (qint64(1) << 53) - 1;
constexpr qsizetype kMaximumSummaryBytes = 256 * 1024;
constexpr qsizetype kMaximumStateBytes = 4 * 1024;
constexpr qint64 kMaximumSummaryClockSkewMilliseconds = 60 * 1000;
constexpr auto kSummaryType = "forkmesh.mirror-actions-summary";
constexpr auto kSummaryFileName = "actions-summary.json";
constexpr auto kStateType = "forkmesh.mirror-actions-state";
constexpr auto kStateFileName = "actions-state.json";

const QRegularExpression kNode(
    QStringLiteral("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$"));
const QRegularExpression kSegment(
    QStringLiteral("^[A-Za-z0-9._-]{1,100}$"));
const QRegularExpression kCommit(
    QStringLiteral("^(?:[0-9a-f]{40}|[0-9a-f]{64})$"));
const QRegularExpression kRef(
    QStringLiteral(
        "^refs/(?:heads|tags)/(?!/)(?!.*(?:\\.\\.|//))"
        "[A-Za-z0-9._/-]{1,160}(?<!/)$"));

QString boundedSingleLine(const QString &value, qsizetype maximum)
{
    QString result;
    result.reserve(qMin(value.size(), maximum));
    for (QChar character : value) {
        const ushort code = character.unicode();
        if (character == QLatin1Char('\r') ||
            character == QLatin1Char('\n') ||
            character == QLatin1Char('\t')) {
            if (!result.isEmpty() &&
                !result.endsWith(QLatin1Char(' '))) {
                result += QLatin1Char(' ');
            }
        } else if (code >= 0x20 && !(code >= 0x7f && code <= 0x9f)) {
            result += character;
        }
        if (result.size() >= maximum)
            break;
    }
    return result.simplified();
}

QString boundedLogTail(
    const QString &value,
    qsizetype maximumBytes = kMaximumSummaryLogTailBytes)
{
    QString sanitized;
    sanitized.reserve(value.size());
    for (QChar character : value) {
        const ushort code = character.unicode();
        if (character == QLatin1Char('\r') ||
            character == QLatin1Char('\n')) {
            sanitized += QLatin1Char('\n');
        } else if (character == QLatin1Char('\t') ||
                   (code >= 0x20 && !(code >= 0x7f && code <= 0x9f))) {
            sanitized += character;
        }
    }

    QByteArray encoded = sanitized.toUtf8();
    if (encoded.size() > maximumBytes) {
        encoded = encoded.right(maximumBytes);
        while (!encoded.isEmpty() &&
               (static_cast<unsigned char>(encoded.front()) & 0xc0) == 0x80) {
            encoded.remove(0, 1);
        }
    }
    QString tail = QString::fromUtf8(encoded);
    encoded.fill('\0');
    encoded.clear();
    while (tail.toUtf8().size() > maximumBytes)
        tail.remove(0, 1);
    return tail;
}

bool validTimestamp(qint64 value, bool allowZero)
{
    return value >= (allowZero ? 0 : 1) &&
           value <= kMaximumSafeJsonInteger;
}

struct SummaryPathInspection {
    detail::SummaryWritePolicy policy =
        detail::SummaryWritePolicy::Reject;
#if defined(Q_OS_UNIX)
    gid_t parentGroupId = 0;
    dev_t parentDevice = 0;
    ino_t parentInode = 0;
#endif
};

SummaryPathInspection inspectSummaryPath(const QString &path)
{
    SummaryPathInspection inspection;
    if (path.size() < 2 || path.size() > 4096 ||
        path.contains(QChar::Null) ||
        path.contains(QLatin1Char('\r')) ||
        path.contains(QLatin1Char('\n'))) {
        return inspection;
    }
    const QFileInfo target(path);
    if (!target.isAbsolute() ||
        QDir::cleanPath(path) != target.absoluteFilePath() ||
        (target.fileName() != QLatin1String(kSummaryFileName) &&
         target.fileName() != QLatin1String(kStateFileName)) ||
        target.isSymLink()) {
        return inspection;
    }
    const QFileInfo parent(target.absolutePath());
    if (!parent.isDir() || parent.isSymLink() ||
        parent.canonicalFilePath() !=
            QDir::cleanPath(parent.absoluteFilePath())) {
        return inspection;
    }

#if defined(Q_OS_UNIX)
    struct stat parentState {};
    const QByteArray parentPath =
        QFile::encodeName(parent.absoluteFilePath());
    if (::lstat(parentPath.constData(), &parentState) != 0)
        return inspection;
    struct stat targetState {};
    const QByteArray targetPath = QFile::encodeName(path);
    const bool targetExists =
        ::lstat(targetPath.constData(), &targetState) == 0;
    if (!targetExists && errno != ENOENT)
        return inspection;
    inspection.policy = detail::classifySummaryWritePolicy(
        path,
        static_cast<quint64>(::geteuid()),
        static_cast<quint64>(parentState.st_uid),
        static_cast<quint64>(parentState.st_gid),
        static_cast<quint32>(parentState.st_mode & 07777),
        S_ISDIR(parentState.st_mode),
        targetExists,
        targetExists && S_ISREG(targetState.st_mode),
        targetExists && S_ISLNK(targetState.st_mode),
        targetExists ? static_cast<quint64>(targetState.st_uid) : 0,
        targetExists ? static_cast<quint64>(targetState.st_gid) : 0,
        targetExists
            ? static_cast<quint32>(targetState.st_mode & 07777)
            : 0,
        targetExists ? static_cast<quint64>(targetState.st_nlink) : 0);
    if (inspection.policy != detail::SummaryWritePolicy::Reject) {
        inspection.parentGroupId = parentState.st_gid;
        inspection.parentDevice = parentState.st_dev;
        inspection.parentInode = parentState.st_ino;
    }
#else
    const QFileDevice::Permissions unsafe =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    if (target.exists() &&
        (!target.isFile() || (target.permissions() & unsafe))) {
        return inspection;
    }
    inspection.policy = detail::SummaryWritePolicy::SameAccount;
#endif
    return inspection;
}

bool sameParentAfterWrite(const QString &path,
                          const SummaryPathInspection &before)
{
#if defined(Q_OS_UNIX)
    const QByteArray parentPath =
        QFile::encodeName(QFileInfo(path).absolutePath());
    struct stat parentState {};
    return ::lstat(parentPath.constData(), &parentState) == 0 &&
           S_ISDIR(parentState.st_mode) &&
           parentState.st_dev == before.parentDevice &&
           parentState.st_ino == before.parentInode &&
           parentState.st_gid == before.parentGroupId;
#else
    Q_UNUSED(path);
    Q_UNUSED(before);
    return true;
#endif
}

} // namespace

namespace detail {

SummaryWritePolicy classifySummaryWritePolicy(
    const QString &path,
    quint64 effectiveUserId,
    quint64 parentUserId,
    quint64 parentGroupId,
    quint32 parentMode,
    bool parentIsProtectedRealDirectory,
    bool targetExists,
    bool targetIsRegular,
    bool targetIsSymbolicLink,
    quint64 targetUserId,
    quint64 targetGroupId,
    quint32 targetMode,
    quint64 targetLinkCount)
{
    if (!parentIsProtectedRealDirectory || parentMode != 0700 ||
        targetIsSymbolicLink ||
        (targetExists &&
         (!targetIsRegular || targetLinkCount != 1))) {
        return SummaryWritePolicy::Reject;
    }

    if (parentUserId == effectiveUserId) {
        if (targetExists &&
            (targetUserId != effectiveUserId || targetMode != 0600)) {
            return SummaryWritePolicy::Reject;
        }
        return SummaryWritePolicy::SameAccount;
    }

    const bool fixedRootHandoffPath =
        path == QLatin1String(kSystemSummaryPath) ||
        path == QLatin1String(kSystemStatePath);
    const bool fixedRootHandoff =
        effectiveUserId == 0 &&
        parentUserId != 0 &&
        parentGroupId != 0 &&
        fixedRootHandoffPath;
    if (!fixedRootHandoff)
        return SummaryWritePolicy::Reject;
    if (targetExists &&
        (targetUserId != 0 || targetGroupId != parentGroupId ||
         targetMode != 0640)) {
        return SummaryWritePolicy::Reject;
    }
    return SummaryWritePolicy::RootGatewayHandoff;
}

} // namespace detail

QJsonObject buildSummary(const QString &node,
                         const QList<ActionRun> &runs,
                         const ActionStore &store,
                         qint64 nowMs)
{
    if (!kNode.match(node).hasMatch() ||
        !validTimestamp(nowMs, false) ||
        nowMs > kMaximumSafeJsonInteger - kSummaryLeaseMilliseconds) {
        return {};
    }

    QList<ActionRun> ordered = runs;
    std::sort(ordered.begin(), ordered.end(),
              [](const ActionRun &left, const ActionRun &right) {
                  if (left.createdAtMs != right.createdAtMs)
                      return left.createdAtMs > right.createdAtMs;
                  return left.id > right.id;
              });
    const QSet<QString> statuses{
        ActionStatus::AwaitingApproval,
        ActionStatus::Queued,
        ActionStatus::Running,
        ActionStatus::Success,
        ActionStatus::Failed,
        ActionStatus::Rejected,
        ActionStatus::Cancelled,
        ActionStatus::Skipped,
    };
    QJsonArray serialized;
    for (const ActionRun &run : std::as_const(ordered)) {
        if (serialized.size() >= kMaximumSummaryRuns)
            break;
        const QString workflow =
            boundedSingleLine(run.workflowName, 160);
        const QString ref = run.ref.trimmed();
        const QString commit = run.commit.trimmed().toLower();
        if (run.id <= 0 ||
            !kSegment.match(run.owner).hasMatch() ||
            !kSegment.match(run.name).hasMatch() ||
            workflow.isEmpty() ||
            !kCommit.match(commit).hasMatch() ||
            !kRef.match(ref).hasMatch() ||
            !statuses.contains(run.status) ||
            !validTimestamp(run.createdAtMs, false) ||
            !validTimestamp(run.startedAtMs, true) ||
            !validTimestamp(run.finishedAtMs, true) ||
            run.createdAtMs >
                nowMs + kMaximumSummaryClockSkewMilliseconds ||
            run.startedAtMs >
                nowMs + kMaximumSummaryClockSkewMilliseconds ||
            run.finishedAtMs >
                nowMs + kMaximumSummaryClockSkewMilliseconds) {
            continue;
        }
        serialized.append(QJsonObject{
            {QStringLiteral("id"), run.id},
            {QStringLiteral("owner"), run.owner},
            {QStringLiteral("repository"), run.name},
            {QStringLiteral("workflow"), workflow},
            {QStringLiteral("commit"), commit},
            {QStringLiteral("ref"), ref},
            {QStringLiteral("status"), run.status},
            {QStringLiteral("createdAt"), run.createdAtMs},
            {QStringLiteral("startedAt"), run.startedAtMs},
            {QStringLiteral("finishedAt"), run.finishedAtMs},
            {QStringLiteral("logTail"),
             boundedLogTail(store.readLog(run))},
        });
    }

    QJsonObject result{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("type"), QLatin1String(kSummaryType)},
        {QStringLiteral("node"), node},
        {QStringLiteral("updatedAt"), nowMs},
        {QStringLiteral("expiresAt"),
         nowMs + kSummaryLeaseMilliseconds},
        {QStringLiteral("runs"), serialized},
    };
    QByteArray encoded =
        QJsonDocument(result).toJson(QJsonDocument::Compact);
    QJsonArray fittedRuns = serialized;
    for (int index = fittedRuns.size() - 1;
         encoded.size() + 1 > kMaximumSummaryBytes && index >= 0;
         --index) {
        QJsonObject item = fittedRuns.at(index).toObject();
        const QString current =
            item.value(QStringLiteral("logTail")).toString();
        const qsizetype currentBytes = current.toUtf8().size();
        const qsizetype excess =
            encoded.size() + 1 - kMaximumSummaryBytes;
        const qsizetype keep =
            qMax<qsizetype>(0, currentBytes - excess - 64);
        item.insert(QStringLiteral("logTail"),
                    boundedLogTail(current, keep));
        fittedRuns.replace(index, item);
        result.insert(QStringLiteral("runs"), fittedRuns);
        encoded = QJsonDocument(result).toJson(QJsonDocument::Compact);
    }
    encoded.fill('\0');
    if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() + 1 >
        kMaximumSummaryBytes) {
        return {};
    }
    return result;
}

static bool writeProtectedJsonFile(const QString &path,
                                   QByteArray encoded,
                                   qsizetype maximumBytes)
{
    const SummaryPathInspection before = inspectSummaryPath(path);
    if (before.policy == detail::SummaryWritePolicy::Reject) {
        encoded.fill('\0');
        return false;
    }
    encoded.append('\n');
    if (encoded.size() > maximumBytes) {
        encoded.fill('\0');
        return false;
    }

    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        encoded.fill('\0');
        return false;
    }
#if defined(Q_OS_UNIX)
    const mode_t outputMode =
        before.policy == detail::SummaryWritePolicy::RootGatewayHandoff
            ? 0640
            : 0600;
    const gid_t outputGroup =
        before.policy == detail::SummaryWritePolicy::RootGatewayHandoff
            ? before.parentGroupId
            : static_cast<gid_t>(-1);
    if (file.handle() < 0 ||
        (before.policy == detail::SummaryWritePolicy::RootGatewayHandoff &&
         (::geteuid() != 0 ||
          ::fchown(file.handle(), 0, outputGroup) != 0)) ||
        ::fchmod(file.handle(), outputMode) != 0) {
        encoded.fill('\0');
        file.cancelWriting();
        return false;
    }
#else
    if (!file.setPermissions(QFileDevice::ReadOwner |
                             QFileDevice::WriteOwner)) {
        encoded.fill('\0');
        file.cancelWriting();
        return false;
    }
#endif
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        encoded.fill('\0');
        return false;
    }
    encoded.fill('\0');
    const SummaryPathInspection after = inspectSummaryPath(path);
    return after.policy == before.policy &&
           sameParentAfterWrite(path, before);
}

bool writeSummaryFile(const QString &path,
                      const QString &node,
                      const QList<ActionRun> &runs,
                      const ActionStore &store,
                      qint64 nowMs)
{
    const QJsonObject summary = buildSummary(node, runs, store, nowMs);
    if (summary.isEmpty())
        return false;
    return writeProtectedJsonFile(
        path,
        QJsonDocument(summary).toJson(QJsonDocument::Compact),
        kMaximumSummaryBytes);
}

bool writeStateFile(const QString &path,
                    const QString &node,
                    const QString &state,
                    qint64 nowMs)
{
    static const QSet<QString> states{
        QStringLiteral("disabled"),
        QStringLiteral("enabled"),
        QStringLiteral("running"),
    };
    if (!kNode.match(node).hasMatch() ||
        !states.contains(state) ||
        !validTimestamp(nowMs, false) ||
        nowMs > kMaximumSafeJsonInteger - kSummaryLeaseMilliseconds) {
        return false;
    }
    const QJsonObject lease{
        {QStringLiteral("schemaVersion"), 1},
        {QStringLiteral("type"), QLatin1String(kStateType)},
        {QStringLiteral("node"), node},
        {QStringLiteral("state"), state},
        {QStringLiteral("updatedAt"), nowMs},
        {QStringLiteral("expiresAt"),
         nowMs + kSummaryLeaseMilliseconds},
    };
    return writeProtectedJsonFile(
        path,
        QJsonDocument(lease).toJson(QJsonDocument::Compact),
        kMaximumStateBytes);
}

} // namespace forkmesh::mirror_actions
