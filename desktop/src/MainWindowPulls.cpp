
#include "MainWindow.h"
#include "ProcessReaper.h"
#include "MainWindowInternal.h"
#include "PullDecision.h"
#include "SafeDelete.h"
#include "AgentCliLocator.h"
#include "SystemPressure.h"
#include "FederatedThreadView.h"
#include "KebabHeaderView.h"
#include "PrivateMirrorRuntime.h"
#include "PromptSpellCheckHighlighter.h"
#include "PullAiReview.h"
#include "PullBadgeWidget.h"

#include <QTextTable>

using namespace forkmesh::ui;

namespace {
constexpr int kCommitShaRole = Qt::UserRole;
constexpr int kCommitMessageRole = Qt::UserRole + 1;
constexpr int kCommitCopyShaRole = Qt::UserRole + 2;
constexpr int kPullFileAgentRole = Qt::UserRole + 1;

constexpr int kPullCreatedAtRole = Qt::UserRole + 31;
constexpr int kPullModifiedAtRole = Qt::UserRole + 32;
constexpr int kPullFilesChangedRole = Qt::UserRole + 33;
constexpr int kPullAdditionsRole = Qt::UserRole + 34;
constexpr int kPullDeletionsRole = Qt::UserRole + 35;
// Branch state for the leading column, mirroring the agents list's branch
// button: conflicts with base, commits ahead/behind it (-1 = not measured
// yet), the head branch's live worktree, and that worktree's dirty count.
constexpr int kPullConflictRole = Qt::UserRole + 36;
constexpr int kPullAheadRole = Qt::UserRole + 37;
constexpr int kPullBehindRole = Qt::UserRole + 38;
constexpr int kPullWorktreeRole = Qt::UserRole + 39;
constexpr int kPullDirtyRole = Qt::UserRole + 40;
constexpr int kPullBaseBranchRole = Qt::UserRole + 41;
// Everything in the row's hover text that does not depend on branch state.
// The conflict probe lands one pull at a time, long after the table was
// built, and rebuilding every row to restate one line of hover text is what
// refreshPullList() is written to avoid; keeping the stable half here lets
// setPullConflictBadge() re-compose just that row's tooltip.
constexpr int kPullTipPrefixRole = Qt::UserRole + 42;
constexpr int kPullViewedFilesRole = Qt::UserRole + 43;
constexpr auto kPendingPullSubmissionsSetting = "pulls/pendingSubmissionsV1";
constexpr qint64 kPullSubmissionRetryBaseMs = 5LL * 1000;
constexpr qint64 kPullSubmissionRetryCapMs = 5LL * 60 * 1000;

QString pullSubmissionKey(const QJsonObject &entry)
{
    const QJsonObject pull = entry.value("pull").toObject();
    const QString sig = pull.value("sig").toString();
    if (!sig.isEmpty())
        return sig;
    const QJsonObject identity{{"owner", entry.value("owner")},
                               {"repo", entry.value("repo")},
                               {"pull", pull}};
    return QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256).toHex());
}

bool isTransientPullSubmissionFailure(int status)
{
    return status <= 0 || status == 408 || status == 425 || status == 429 ||
           status >= 500;
}

QString pullSubmissionErrorDetail(int status, const QByteArray &body,
                                  const QString &errorString)
{
    QStringList parts;
    if (status > 0)
        parts << QStringLiteral("HTTP %1").arg(status);
    const QString reported =
        QJsonDocument::fromJson(body).object().value("error").toString();
    if (!reported.isEmpty())
        parts << reported;
    else if (!body.trimmed().isEmpty())
        parts << QString::fromUtf8(body).trimmed().left(200);
    if (parts.isEmpty())
        parts << errorString;
    return parts.join(QStringLiteral(" - "));
}

// A background pull load reparses independently allocated strings, so comparing
// the new list to m_currentPulls on the GUI thread still walks every patch and
// mbox. Hash the complete UI-visible snapshot while it is on the worker instead.
// The digest includes the repository identity and base tip because an unchanged
// ledger still needs fresh merge/conflict state after either of those changes.
QByteArray pullSnapshotDigest(const QList<PullRequest> &pulls,
                              const QString &baseTip,
                              const QString &repositoryKey)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const auto addBytes = [&hash](const QByteArray &bytes) {
        hash.addData(QByteArray::number(bytes.size()));
        hash.addData(QByteArrayLiteral(":"));
        hash.addData(bytes);
    };
    const auto addText = [&addBytes](const QString &text) {
        addBytes(text.toUtf8());
    };
    const auto addNumber = [&addBytes](qint64 number) {
        addBytes(QByteArray::number(number));
    };

    addText(repositoryKey);
    addText(baseTip);
    addNumber(pulls.size());
    for (const PullRequest &pull : pulls) {
        addNumber(pull.number);
        addText(pull.title);
        addText(pull.description);
        addText(pull.base);
        addText(pull.head);
        addText(pull.status);
        addNumber(pull.ts);
        addText(pull.author);
        addText(pull.authorName);
        addText(pull.sig);
        addText(pull.patch);
        addText(pull.commits);
        addNumber(pull.branchBacked);
        addText(pull.creationBaseOid);
        addText(pull.creationHeadOid);
        addText(pull.mergeBase);
        addText(pull.mergeHead);
        addText(pull.client);
        addText(pull.clientSig);
        addNumber(pull.rewardEstimateLamports);
        addText(pull.rewardRecipient);
        addText(pull.rewardId);
        addText(pull.rewardCondition);
        addText(pull.rewardStatus);
        addText(pull.rewardTxSignature);
        addNumber(pull.filesChanged);
        addNumber(pull.additions);
        addNumber(pull.deletions);
        addNumber(pull.archivedOnCodeBranch);
        addNumber(pull.events.size());
        for (const PullEvent &event : pull.events) {
            addBytes(QJsonDocument(event.toJson()).toJson(
                QJsonDocument::Compact));
        }
    }
    return hash.result();
}

// Every conversation card sits behind an avatar column and a gutter
// (addConversationCard). Panels that have no avatar of their own — linked
// issues, checks, conflicts, the merge report, the federated thread — are
// indented by the same amount so their left edge lands on the card column
// instead of hanging out under the avatars.
constexpr int kPullThreadAvatarPx = 36;
constexpr int kPullThreadGutterPx = 14;

QString pullLineNoteKey(const QString &path, const QString &side, int line)
{
    return path + QLatin1Char('\x1f') + side + QLatin1Char(':') +
           QString::number(line);
}

bool parsePullLineAnchor(const QString &href, QString *path, QString *side,
                         int *line)
{
    const QUrl url(href);
    if (url.scheme() != QLatin1String("cmt"))
        return false;
    const QUrlQuery query(url);
    const QString parsedPath = url.path();
    const QString parsedSide = query.queryItemValue(QStringLiteral("s"));
    const int parsedLine = query.queryItemValue(QStringLiteral("l")).toInt();
    if (parsedPath.isEmpty() || parsedLine <= 0 ||
        (parsedSide != QLatin1String("old") &&
         parsedSide != QLatin1String("new")))
        return false;
    if (path)
        *path = parsedPath;
    if (side)
        *side = parsedSide;
    if (line)
        *line = parsedLine;
    return true;
}

bool pullLineAnchorAt(const QTextBrowser *view, const QPoint &pos,
                      const QString &side, QString *path, int *line)
{
    QString foundSide;
    if (parsePullLineAnchor(view->anchorAt(pos), path, &foundSide, line) &&
        foundSide == side)
        return true;
    const QTextCursor cursor = view->cursorForPosition(pos);
    QTextTable *table = cursor.currentTable();
    if (!table)
        return false;
    const QTextTableCell cell = table->cellAt(cursor);
    if (!cell.isValid())
        return false;
    for (int column = 0; column < table->columns(); ++column) {
        const QTextTableCell candidate = table->cellAt(cell.row(), column);
        if (!candidate.isValid())
            continue;
        for (auto it = candidate.begin(); !it.atEnd(); ++it) {
            const QTextBlock block = it.currentBlock();
            for (auto f = block.begin(); !f.atEnd(); ++f) {
                const QTextFragment fragment = f.fragment();
                if (fragment.isValid() && fragment.charFormat().isAnchor() &&
                    parsePullLineAnchor(fragment.charFormat().anchorHref(), path,
                                        &foundSide, line) &&
                    foundSide == side)
                    return true;
            }
        }
    }
    return false;
}

// The wrapper mirrors the panel's own visibility. QBoxLayout drops hidden
// widgets and the spacing around them, but a *visible* wrapper holding a hidden
// panel is not hidden — it would leave a blank band in the conversation — so
// hiding the panel has to take its row with it.
class ThreadCardRow final : public QWidget
{
public:
    explicit ThreadCardRow(QWidget *content) : m_content(content)
    {
        // Deliberately not an "issueTimelineRow": this carries no avatar and no
        // card header, and testPullThreadCardHeaders() walks the thread by that
        // name to enumerate real conversation cards.
        setObjectName(QStringLiteral("issueTimelineIndent"));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(kPullThreadAvatarPx + kPullThreadGutterPx, 0,
                                   0, 0);
        layout->setSpacing(0);
        layout->addWidget(content);
        content->installEventFilter(this);
        setVisible(!content->isHidden());
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_content && (event->type() == QEvent::ShowToParent ||
                                     event->type() == QEvent::HideToParent))
            setVisible(event->type() == QEvent::ShowToParent);
        return QWidget::eventFilter(watched, event);
    }

private:
    QWidget *m_content = nullptr;
};

QString normalizedIssueSyncPath(const QString &path)
{
    const QFileInfo info(path.trimmed());
    if (info.filePath().isEmpty())
        return QString();
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                               : canonical);
}

QString normalizedRelayOrigin(QUrl url)
{
    url.setUserInfo(QString());
    url.setPath(QString());
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString(QUrl::RemoveUserInfo | QUrl::StripTrailingSlash);
}

QString pullListStatusIcon(const QString &status)
{
    if (status == QLatin1String("merged"))
        return QStringLiteral("git-merge");
    if (status == QLatin1String("closed"))
        return QStringLiteral("circle-slash");
    return QStringLiteral("git-pull-request");
}

QColor pullListStatusColor(const QString &status)
{
    if (status == QLatin1String("merged"))
        return Theme::nextOr(Theme::Role::AccentFg, QColor("#a371f7"));
    if (status == QLatin1String("closed"))
        return QColor("#f85149");
    return QColor("#3fb950");
}

QString pullListStatusLabel(const QString &status)
{
    if (status == QLatin1String("merged"))
        return QStringLiteral("Merged");
    if (status == QLatin1String("closed"))
        return QStringLiteral("Closed");
    return QStringLiteral("Open");
}

// One pull's branch pair, handed to the worker that measures how the head
// stands against its base.
struct PullHeadProbe {
    int number = 0;
    QString base;
    QString head;
};

// The hover lines that describe a pull's head branch, composed from the roles
// the row already carries so the conflict probe can restate them for one row
// without a table rebuild. Empty until the branch has actually been measured.
QString pullBranchStatusTip(const QTableWidgetItem *item)
{
    if (!item)
        return QString();
    const bool conflicted = item->data(kPullConflictRole).toBool();
    const int ahead = item->data(kPullAheadRole).toInt();
    const int behind = item->data(kPullBehindRole).toInt();
    const QString worktree = item->data(kPullWorktreeRole).toString();
    const int dirty = item->data(kPullDirtyRole).toInt();
    const QString base = item->data(kPullBaseBranchRole).toString();
    const QString baseName =
        base.isEmpty() ? QStringLiteral("base") : base;
    QStringList lines;
    if (conflicted) {
        lines << QStringLiteral("Conflicts with %1 — merge %1 in and resolve "
                                "before this can be merged")
                     .arg(baseName);
    } else if (behind > 0) {
        lines << QStringLiteral("%1 commit%2 behind %3 — needs %3 merged in "
                                "to catch up")
                     .arg(behind)
                     .arg(behind == 1 ? QString() : QStringLiteral("s"))
                     .arg(baseName);
    }
    if (behind >= 0)
        lines << QString::fromUtf8("%1 ahead \xC2\xB7 %2 behind %3")
                     .arg(ahead)
                     .arg(behind)
                     .arg(baseName);
    if (!worktree.isEmpty()) {
        lines << QStringLiteral("Worktree: %1").arg(worktree);
        if (dirty > 0)
            lines << QStringLiteral("%1 uncommitted change%2 in that worktree")
                         .arg(dirty)
                         .arg(dirty == 1 ? QString() : QStringLiteral("s"));
        else if (dirty == 0)
            lines << QStringLiteral("That worktree is clean");
    } else if (behind >= 0) {
        lines << QStringLiteral("No worktree has this branch checked out");
    }
    if (lines.isEmpty())
        return QString();
    return lines.join(QLatin1Char('\n')).prepend(QLatin1Char('\n'));
}

class PullCompactMetaDelegate final : public SelectionBorderRowDelegate
{
public:
    explicit PullCompactMetaDelegate(QAbstractItemView *view)
        : SelectionBorderRowDelegate(view)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = SelectionBorderRowDelegate::sizeHint(option, index);
        const QFont compact = compactFont(option);
        const QFontMetrics fm(compact);
        const int tail = 2 * ageSlotWidth(fm) + kAgeDividerWidth +
                         kAgeToFilesGap + kBranchIconSize + kIconTextGap +
                         2 * (kStatusIconSize + kStatusGap) +
                         viewedSlotWidth(fm) + filesSlotWidth(fm) +
                         kFilesToBarsGap + kDiffBarWidth +
                         kTrailingPad;
        size.rwidth() = qMax(size.width(),
                             option.fontMetrics.horizontalAdvance(QStringLiteral("#9999")) +
                                 18 + tail);
        size.rheight() = qMax(size.height(), kDiffBarHeight + 8);
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        SelectionBorderRowDelegate::paint(painter, option, index);

        const bool dark = currentThemeIsDark();
        const QColor muted = Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        const QFont compact = compactFont(option);
        const QFontMetrics fm(compact);
        const QRect cell = option.rect;
        const int added = index.data(kPullAdditionsRole).toInt();
        const int removed = index.data(kPullDeletionsRole).toInt();

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::SmoothPixmapTransform);

        const int barsLeft = cell.right() - kDiffBarWidth + 1;
        const int baseline = cell.center().y() + kDiffBarHeight / 2;
        painter->setPen(Qt::NoPen);
        const int addHeight = diffBarHeight(added);
        if (addHeight > 0) {
            painter->setBrush(QColor(dark ? "#3fb950" : "#1a7f37"));
            painter->drawRect(barsLeft, baseline - addHeight, kDiffBarThickness,
                              addHeight);
        }
        const int delHeight = diffBarHeight(removed);
        if (delHeight > 0) {
            painter->setBrush(QColor(dark ? "#f85149" : "#cf222e"));
            painter->drawRect(barsLeft + kDiffBarThickness + kDiffBarGap,
                              baseline - delHeight, kDiffBarThickness, delHeight);
        }

        const int files = index.data(kPullFilesChangedRole).toInt();
        const QString filesText = files > 999 ? QStringLiteral("999+")
                                               : QString::number(qMax(0, files));
        const int viewed = qMin(index.data(kPullViewedFilesRole).toInt(), files);
        const int filesRight = barsLeft - kFilesToBarsGap;
        const int filesLeft = filesRight - filesSlotWidth(fm) - viewedSlotWidth(fm);
        // Right to left: the conflict/behind mark, the branch icon, then the
        // worktree mark. Both marks keep their slot on rows that have nothing
        // to say there, so the counts and ages stay in a column down the list.
        const int stateIconLeft = filesLeft - kIconTextGap - kStatusIconSize;
        const int fileIconLeft = stateIconLeft - kStatusGap - kBranchIconSize;
        const int worktreeIconLeft = fileIconLeft - kStatusGap - kStatusIconSize;
        const int stateIconTop = cell.center().y() - kStatusIconSize / 2;
        const int branchIconTop = cell.center().y() - kBranchIconSize / 2;

        const bool conflicted = index.data(kPullConflictRole).toBool();
        const bool behind = index.data(kPullBehindRole).toInt() > 0;
        const QString worktree = index.data(kPullWorktreeRole).toString();
        const QColor accent = Theme::nextOr(
            Theme::Role::Warning, QColor(dark ? "#e3742f" : "#bc4c00"));
        const QColor live = Theme::nextOr(
            Theme::Role::AccentFg, QColor(dark ? "#58a6ff" : "#0969da"));
        if (!worktree.isEmpty())
            painter->drawPixmap(worktreeIconLeft, stateIconTop,
                                tintedOcticonPixmap(QStringLiteral("worktree"),
                                                    live, kStatusIconSize));
        if (conflicted || behind)
            painter->drawPixmap(
                stateIconLeft, stateIconTop,
                tintedOcticonPixmap(conflicted ? QStringLiteral("alert")
                                               : QStringLiteral("download"),
                                    accent, kStatusIconSize));
        // The branch icon carries the summary the way the agents list's branch
        // button does: amber once the head needs base merged in, blue while a
        // checkout of it is still on disk, muted otherwise.
        painter->drawPixmap(
            fileIconLeft, branchIconTop,
            tintedOcticonPixmap(QStringLiteral("git-branch"),
                                (conflicted || behind)
                                    ? accent
                                    : (worktree.isEmpty() ? muted : live),
                                kBranchIconSize));
        if (index.data(kPullDirtyRole).toInt() > 0) {
            painter->setPen(QPen(Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff")), 1.5));
            painter->setBrush(QColor(dark ? "#d29922" : "#bf8700"));
            painter->drawEllipse(QPointF(fileIconLeft + kBranchIconSize - 1.5,
                                         branchIconTop + 1.5),
                                 2.5, 2.5);
        }
        painter->setFont(compact);
        painter->setPen(muted);
        painter->drawText(QRect(filesLeft, cell.top(), filesRight - filesLeft,
                                cell.height()),
                          Qt::AlignVCenter | Qt::AlignRight, filesText);
        if (viewed > 0) {
            const QString viewedText = (viewed > 999 ? QStringLiteral("999+")
                                                     : QString::number(viewed)) +
                                       QLatin1Char('/');
            const int viewedRight = filesRight - fm.horizontalAdvance(filesText);
            painter->setPen(viewed >= files ? QColor(dark ? "#3fb950" : "#1a7f37")
                                            : QColor(dark ? "#58a6ff" : "#0969da"));
            painter->drawText(QRect(filesLeft, cell.top(), viewedRight - filesLeft,
                                    cell.height()),
                              Qt::AlignVCenter | Qt::AlignRight, viewedText);
            painter->setPen(muted);
        }

        const qint64 created = index.data(kPullCreatedAtRole).toLongLong();
        const qint64 modified = index.data(kPullModifiedAtRole).toLongLong();
        const QString createdText = created > 0
                                        ? formatShortRelativeTime(created / 1000)
                                        : QStringLiteral("-");
        const QString modifiedText = modified > 0
                                         ? formatShortRelativeTime(modified / 1000)
                                         : QStringLiteral("-");
        const int modifiedRight = worktreeIconLeft - kAgeToFilesGap;
        const int modifiedLeft = modifiedRight - ageSlotWidth(fm);
        const int dividerLeft = modifiedLeft - kAgeDividerWidth;
        const int createdLeft = dividerLeft - ageSlotWidth(fm);
        painter->drawText(QRect(createdLeft, cell.top(), ageSlotWidth(fm),
                                cell.height()),
                          Qt::AlignVCenter | Qt::AlignRight, createdText);
        painter->drawText(QRect(dividerLeft, cell.top(), kAgeDividerWidth,
                                cell.height()),
                          Qt::AlignCenter, QString::fromUtf8("·"));
        painter->drawText(QRect(modifiedLeft, cell.top(), ageSlotWidth(fm),
                                cell.height()),
                          Qt::AlignVCenter | Qt::AlignRight, modifiedText);
        painter->restore();
    }

private:
    static constexpr int kBranchIconSize = 14;
    static constexpr int kStatusIconSize = 12; // worktree / conflict marks
    static constexpr int kStatusGap = 3;
    static constexpr int kIconTextGap = 4;
    static constexpr int kAgeDividerWidth = 9;
    static constexpr int kAgeToFilesGap = 9;
    static constexpr int kFilesToBarsGap = 8;
    static constexpr int kTrailingPad = 16;
    static constexpr int kDiffBarThickness = 2;
    static constexpr int kDiffBarGap = 2;
    static constexpr int kDiffBarWidth = 2 * kDiffBarThickness + kDiffBarGap;
    static constexpr int kDiffBarHeight = 14;
    static constexpr double kDiffBarFullScaleLines = 800.0;

    static int ageSlotWidth(const QFontMetrics &fm)
    {
        return fm.horizontalAdvance(QStringLiteral("999y"));
    }

    static int filesSlotWidth(const QFontMetrics &fm)
    {
        return fm.horizontalAdvance(QStringLiteral("999+"));
    }

    static int viewedSlotWidth(const QFontMetrics &fm)
    {
        return fm.horizontalAdvance(QStringLiteral("999/"));
    }

    static QFont compactFont(const QStyleOptionViewItem &option)
    {
        QFont font = option.font;
        if (font.pixelSize() > 0)
            font.setPixelSize(qMax(9, font.pixelSize() - 3));
        else
            font.setPointSizeF(qMax(7.0, font.pointSizeF() - 2.0));
        font.setWeight(QFont::DemiBold);
        return font;
    }

    static int diffBarHeight(int lines)
    {
        if (lines <= 0)
            return 0;
        const double scale =
            std::log1p(lines) / std::log1p(kDiffBarFullScaleLines);
        return qBound(2, qRound(qMin(1.0, scale) * kDiffBarHeight),
                      kDiffBarHeight);
    }
};

VerticalIconButton *pullActionButton(const QString &caption, const char *icon,
                                     const QString &tooltip = QString())
{
    auto *b = new VerticalIconButton(caption, VerticalIconButton::Bare);
    b->setObjectName("repoActionStack");
    b->setCursor(Qt::PointingHandCursor);
    b->setOcticonName(QString::fromLatin1(icon));
    if (!tooltip.isEmpty())
        b->setToolTip(tooltip);
    return b;
}

void setPullActionBadge(QPushButton *button, int count)
{
    if (auto *tile = dynamic_cast<VerticalIconButton *>(button))
        tile->setBadgeCount(qMax(0, count));
}

void setPullChecksAttention(QPushButton *button, bool running)
{
    if (auto *tile = dynamic_cast<VerticalIconButton *>(button))
        tile->setAttentionLight(running);
}

bool pullHasRunningChecks(const PullRequest &pull,
                          const QList<ActionRun> &runs,
                          const QString &owner, const QString &name)
{
    return std::any_of(runs.cbegin(), runs.cend(),
                       [&pull, &owner, &name](const ActionRun &run) {
                           return run.owner == owner && run.name == name &&
                                  run.pullNumber == pull.number &&
                                  run.status == ActionStatus::Running;
                       });
}

QString pendingInboxAuthor(const QString &kind, const QJsonObject &item)
{
    QJsonObject object;
    if (kind == QLatin1String("pulls") && item.contains("pull"))
        object = item.value("pull").toObject();
    else
        object = item.value("event").toObject();
    const QString name = object.value("authorName").toString().trimmed();
    const QString author = object.value("author").toString().trimmed();
    return !name.isEmpty() ? name
                           : (!author.isEmpty() ? author.left(12)
                                                : QStringLiteral("Unknown"));
}

QString pendingInboxSummary(const QString &kind, const QJsonObject &item)
{
    const int number = item.value("number").toInt();
    if (kind == QLatin1String("issues")) {
        const QJsonObject event = item.value("event").toObject();
        const QString type = event.value("type").toString();
        QString title = event.value("title").toString().trimmed();
        if (title.isEmpty())
            title = item.value("titleIfNew").toString().trimmed();
        if (type == QLatin1String("open"))
            return title.isEmpty() ? QStringLiteral("New issue") : title;
        const QString action = type == QLatin1String("comment")
            ? QStringLiteral("Comment")
            : (type.isEmpty() ? QStringLiteral("Issue update")
                              : type.left(1).toUpper() + type.mid(1));
        return number > 0 ? QStringLiteral("%1 on issue #%2").arg(action).arg(number)
                          : action;
    }
    if (kind == QLatin1String("pulls")) {
        if (item.contains("pull")) {
            const QJsonObject pull = item.value("pull").toObject();
            const int pullNumber = pull.value("number").toInt(number);
            const QString title = pull.value("title").toString().trimmed();
            return pullNumber > 0
                ? QStringLiteral("PR #%1: %2").arg(pullNumber).arg(
                      title.isEmpty() ? QStringLiteral("New pull request") : title)
                : (title.isEmpty() ? QStringLiteral("New pull request") : title);
        }
        const QString type = item.value("event").toObject()
                                 .value("type").toString().trimmed();
        return number > 0
            ? QStringLiteral("%1 on PR #%2")
                  .arg(type.isEmpty() ? QStringLiteral("Review") : type)
                  .arg(number)
            : QStringLiteral("Pull request update");
    }

    const QJsonObject event = item.value("event").toObject();
    const QString type = event.value("type").toString();
    QString title = event.value("title").toString().trimmed();
    if (title.isEmpty())
        title = item.value("titleIfNew").toString().trimmed();
    if (type == QLatin1String("open"))
        return title.isEmpty() ? QStringLiteral("New discussion") : title;
    return number > 0 ? QStringLiteral("Comment on discussion #%1").arg(number)
                      : QStringLiteral("Discussion update");
}

} // namespace

QString MainWindow::autolinkReferencesWithStatus(const QString &markdown) const
{
    // Markdown image syntax, not <img>: QLabel's markdown importer drops inline
    // HTML images along with the rest of the line, and only creates an image
    // when the alt text is non-empty. The PNG is at logical size because the
    // document lays a data image out at its pixel size.
    auto glyph = [](const QString &icon, const QColor &color, const QString &alt) {
        static QHash<QString, QString> cache;
        const QString key = icon + QLatin1Char('|') +
                            QString::number(Theme::remapColor(color).rgba(), 16) +
                            QLatin1Char('|') + alt;
        auto it = cache.find(key);
        if (it == cache.end()) {
            constexpr int kSize = 14;
            const QImage image =
                tintedOcticonPixmap(icon, color, kSize)
                    .toImage()
                    .scaled(kSize, kSize, Qt::KeepAspectRatio,
                            Qt::SmoothTransformation);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            it = cache.insert(key, QStringLiteral("![%1](data:image/png;base64,%2) ")
                                       .arg(alt, QString::fromLatin1(png.toBase64())));
        }
        return it.value();
    };
    auto statusGlyph = [&](const QString &kind, int number) -> QString {
        // A bare #N opens a pull when one has that number (openCommitReference),
        // so its glyph must describe the same record the click will open.
        if (kind != QLatin1String("issue")) {
            for (const PullRequest &pull : m_currentPulls) {
                if (pull.number == number)
                    return glyph(pullListStatusIcon(pull.status),
                                 pullListStatusColor(pull.status),
                                 pull.status);
            }
        }
        if (kind != QLatin1String("pull")) {
            for (const Issue &issue : m_currentIssues) {
                if (issue.number != number)
                    continue;
                const bool closed = issue.status == QLatin1String("closed");
                const bool deleted = issue.status == QLatin1String("deleted");
                return glyph(closed ? QStringLiteral("check-circle")
                                    : QStringLiteral("issue-opened"),
                             closed ? Theme::nextOr(Theme::Role::AccentFg,
                                                    QColor("#8250df"))
                                    : Theme::remapColor(
                                          QColor(deleted ? "#8b949e"
                                                         : "#1a7f37")),
                             issue.status);
            }
        }
        return QString();
    };
    return autolinkReferences(markdown, statusGlyph);
}


QWidget *MainWindow::buildPullsTab()
{
    auto *page = new QWidget;


    auto *listPane = buildPullListPane(page);

    m_pullDetail = new QWidget;
    m_pullTitle = new QLabel("Select a pull request");
    m_pullTitle->setObjectName("channelTitle");
    m_pullTitle->setWordWrap(true);
    m_pullTitle->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_pullTitle->setMinimumWidth(0);
    m_pullTitle->ensurePolished();
    m_pullTitle->setMaximumHeight(m_pullTitle->fontMetrics().lineSpacing() * 2 + 2);
    m_pullStatusBadge = new QLabel;
    m_pullStatusBadge->setObjectName(QStringLiteral("pullDetailStatusBadge"));
    m_pullStatusBadge->setAlignment(Qt::AlignCenter);
    m_pullStatusBadge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_pullStatusBadge->setMinimumWidth(72);
    m_pullStatusBadge->hide();
    auto *pullHeaderRow = buildPullHeaderRow();
    m_pullMeta = new QLabel;
    m_pullMeta->setObjectName("statusLine");
    m_pullMeta->setTextFormat(Qt::RichText);
    m_pullMeta->setWordWrap(true);
    m_pullMeta->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                        Qt::LinksAccessibleByMouse);
    connect(m_pullMeta, &QLabel::linkActivated, this, [this](const QString &href) {
        if (href.startsWith(kAgentLinkScheme))
            switchToAgentsTab(href.mid(kAgentLinkScheme.size()).toInt());
        else if (href.startsWith(kBranchLinkScheme)) {
            const QString branch = QUrl::fromPercentEncoding(
                href.mid(kBranchLinkScheme.size()).toUtf8());
            bool isCurrentPullHead = false;
            for (const PullRequest &pr : std::as_const(m_currentPulls)) {
                if (pr.number == m_currentPullNumber && pr.head == branch) {
                    isCurrentPullHead = true;
                    break;
                }
            }
            if (isCurrentPullHead)
                openPullDiffInGitView(m_currentPullNumber);
            else
                switchToBranch(branch);
        }
    });
    m_pullMergeStatus = new QLabel;
    m_pullMergeStatus->setObjectName("statusLine");
    m_pullMergeStatus->setTextFormat(Qt::RichText);
    m_pullMergeStatus->setWordWrap(true);
    m_pullMergeStatus->hide();

    auto *filesPage = buildPullFilesPage();

    buildPullCommitsList();

    auto *checksPage = buildPullChecksPage();

    buildPullConversation();

    auto *subTabRow = buildPullSubTabs(checksPage, filesPage);

    auto *detailLayout = new QVBoxLayout(m_pullDetail);
    detailLayout->setContentsMargins(18, 18, 18, 18);
    detailLayout->setSpacing(8);
    auto *pullTitleRow = new QHBoxLayout;
    pullTitleRow->setContentsMargins(0, 0, 0, 0);
    pullTitleRow->setSpacing(10);
    pullTitleRow->addWidget(m_pullTitle, 1);
    pullTitleRow->addWidget(m_pullStatusBadge, 0, Qt::AlignTop);
    detailLayout->addLayout(pullTitleRow);
    detailLayout->addLayout(pullHeaderRow);
    detailLayout->addWidget(m_pullMeta);
    detailLayout->addWidget(m_pullMergeStatus);
    detailLayout->addLayout(subTabRow);
    detailLayout->addWidget(m_pullSubStack, 1);

    m_pullDetail->hide();

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(listPane);
    splitter->addWidget(m_pullDetail);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({460, 620});

    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(splitter);

    connect(m_pullTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QModelIndexList rows = m_pullTable->selectionModel()->selectedRows();
        if (rows.isEmpty())
            return;
        if (QTableWidgetItem *first = m_pullTable->item(rows.first().row(), 0)) {
            const int number = first->data(Qt::UserRole).toInt();
            if (number != m_pullOpenTarget)
                m_pullOpenTarget = -1;
            showPull(number);
        }
    });
    connect(m_pullNewButton, &QPushButton::clicked, this, &MainWindow::promptNewPull);
    connect(m_pullChooseDirButton, &QPushButton::clicked,
            this, &MainWindow::promptNewPullFromDirectory);
    connect(m_pullSyncButton, &QPushButton::clicked, this, &MainWindow::syncPullsInbox);
    connect(m_pullRefreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshPullsFromMesh);
    const auto changePullListFilter = [this](bool checked) {
        if (!checked)
            return;
        // Archived records can carry full patches and commit mailboxes. Fetch
        // them for every history filter; Open immediately restores the
        // lightweight current-metadata list.
        reloadPullsInBackground();
        refreshPullList();
    };
    connect(m_pullShowOpenButton, &QPushButton::toggled, this,
            changePullListFilter);
    connect(m_pullShowClosedButton, &QPushButton::toggled, this,
            changePullListFilter);
    connect(m_pullShowMergedButton, &QPushButton::toggled, this,
            changePullListFilter);
    connect(m_pullShowAllButton, &QPushButton::toggled, this,
            changePullListFilter);
    connect(m_pullDeleteClosedAndMergedButton, &QPushButton::clicked, this,
            &MainWindow::deleteAllClosedAndMergedPullsAndBranches);
    connect(m_pullUpdateButton, &QPushButton::clicked,
            this, &MainWindow::updateCurrentPullBranch);
    connect(m_pullMergeButton, &QPushButton::clicked, this, &MainWindow::mergeCurrentPull);
    connect(m_pullMergeDeleteButton, &QPushButton::clicked, this,
            &MainWindow::mergeAndDeleteCurrentPull);
    connect(m_pullCloseButton, &QPushButton::clicked, this, &MainWindow::closeCurrentPull);
    connect(m_pullReopenButton, &QPushButton::clicked, this, &MainWindow::reopenCurrentPull);
    connect(m_pullRestoreButton, &QPushButton::clicked, this,
            &MainWindow::restoreCurrentArchivedPull);
    connect(m_pullSendToSourceButton, &QPushButton::clicked, this,
            &MainWindow::sendCurrentPullToSource);
    connect(m_pullDeleteButton, &QPushButton::clicked, this, &MainWindow::deleteCurrentPull);
    connect(m_pullDeleteBranchButton, &QPushButton::clicked, this,
            &MainWindow::deleteCurrentPullAndBranch);

    // This tab is built lazily, long after openRepo() started the first load.
    // Rendering the rows here would block the switch itself — and a repository
    // with a long history has plenty of them — so the page comes up on the
    // spinner and applyLoadedPulls() fills it as soon as the worker answers.
    setPullListLoading(m_pullBackgroundLoadInFlight);
    return page;
}

QWidget *MainWindow::buildPullListPane(QWidget *page)
{
    auto *listPane = new QWidget;
    listPane->setMinimumWidth(320);
    m_pullNewButton = pullActionButton(
        QStringLiteral("New"), "plus",
        QStringLiteral("Open a new pull request in this repository"));
    m_pullChooseDirButton = pullActionButton(
        QStringLiteral("Directory"), "file-directory",
        QStringLiteral("Create a pull request from another local checkout of "
                       "this repository"));
    m_pullImportButton = pullActionButton(
        QStringLiteral("Import"), "download",
        QStringLiteral("Open a .patch/.diff file (e.g. a downloaded commit) as "
                       "a pull request"));
    m_pullSyncButton = pullActionButton(
        QStringLiteral("Inbox"), "sync",
        QStringLiteral("Pull PR submissions filed by other nodes and merge them"));
    m_pullRefreshButton = pullActionButton(
        QStringLiteral("Refresh"), "sync",
        QStringLiteral("Refresh the list, automatically import pending PRs, and "
                       "check mirrors for pull requests"));
    m_pullRefreshButton->setObjectName(QStringLiteral("pullListRefreshButton"));
    m_pullShowOpenButton = pullActionButton(
        QStringLiteral("Open"), "git-pull-request",
        QStringLiteral("Show open pull requests"));
    m_pullShowOpenButton->setObjectName(QStringLiteral("pullListShowOpenButton"));
    m_pullShowOpenButton->setCheckable(true);
    m_pullShowClosedButton = pullActionButton(
        QStringLiteral("Closed"), "circle-slash",
        QStringLiteral("Show closed pull requests"));
    m_pullShowClosedButton->setObjectName(
        QStringLiteral("pullListShowClosedButton"));
    m_pullShowClosedButton->setCheckable(true);
    m_pullShowMergedButton = pullActionButton(
        QStringLiteral("Merged"), "git-merge",
        QStringLiteral("Show merged pull requests"));
    m_pullShowMergedButton->setObjectName(
        QStringLiteral("pullListShowMergedButton"));
    m_pullShowMergedButton->setCheckable(true);
    m_pullShowAllButton = pullActionButton(
        QStringLiteral("All"), "list-unordered",
        QStringLiteral("Show every registered pull request, including closed and "
                       "merged ones"));
    m_pullShowAllButton->setObjectName(QStringLiteral("pullListShowAllButton"));
    m_pullShowAllButton->setCheckable(true);
    auto *pullListFilter = new QButtonGroup(page);
    pullListFilter->setExclusive(true);
    pullListFilter->addButton(m_pullShowOpenButton);
    pullListFilter->addButton(m_pullShowClosedButton);
    pullListFilter->addButton(m_pullShowMergedButton);
    pullListFilter->addButton(m_pullShowAllButton);
    m_pullShowOpenButton->setChecked(true);
    m_pullDeleteClosedAndMergedButton = pullActionButton(
        QStringLiteral("Delete closed and merged"), "trash",
        QStringLiteral("Delete every closed or merged pull request in this repo "
                       "and its head branch"));
    m_pullDeleteClosedAndMergedButton->setObjectName(
        QStringLiteral("pullDeleteClosedAndMergedButton"));
    m_pullMergeAllAiButton = pullActionButton(
        QStringLiteral("Merge all"), "sparkle",
        QStringLiteral("Start an AI agent in this repository's own checkout "
                       "(no branch, no permission prompts) that reviews every "
                       "open pull request for security issues, bugs and code "
                       "that does not fit, merges the safe ones into their base "
                       "cleanly, then deletes the merged pull requests and "
                       "their branches"));
    m_pullMergeAllAiButton->setObjectName(QStringLiteral("pullMergeAllAiButton"));
    connect(m_pullMergeAllAiButton, &QPushButton::clicked, this,
            &MainWindow::mergeAllPullsWithAgent);
    connect(m_pullImportButton, &QPushButton::clicked, this,
            &MainWindow::importPatchAsPull);
    auto *actionBar = new QFrame;
    actionBar->setObjectName(QStringLiteral("pullListActionBar"));
    auto *toolbar = new QHBoxLayout(actionBar);
    toolbar->setContentsMargins(6, 4, 6, 4);
    toolbar->setSpacing(2);
    toolbar->addWidget(m_pullNewButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullChooseDirButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullImportButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullSyncButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullRefreshButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullShowOpenButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullShowClosedButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullShowMergedButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullShowAllButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullMergeAllAiButton, 0, Qt::AlignTop);
    toolbar->addWidget(m_pullDeleteClosedAndMergedButton, 0, Qt::AlignTop);
    toolbar->addStretch(1);
    auto *actionBarDivider = new QFrame;
    actionBarDivider->setFrameShape(QFrame::HLine);
    actionBarDivider->setObjectName(QStringLiteral("pullListActionBarDivider"));

    m_pullTable = new QTableWidget(0, 2);
    m_pullTable->setObjectName("issueTable");
    m_pullTable->setItemDelegate(new SelectionBorderRowDelegate(m_pullTable));
    m_pullTable->setStyleSheet(Theme::remapSheet(
        "#issueTable { selection-background-color: transparent; border: none; }"
        "#issueTable::item:selected { background: transparent; }"));
    m_pullTable->setFrameShape(QFrame::NoFrame);
    m_pullTable->setHorizontalHeaderLabels({"#", "Title"});
    m_pullTable->verticalHeader()->setVisible(false);
    m_pullTable->horizontalHeader()->setVisible(false);
    m_pullTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pullTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pullTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pullTable->setShowGrid(false);
    m_pullTable->setWordWrap(false);
    m_pullTable->setTextElideMode(Qt::ElideNone);
    m_pullTable->setIconSize(QSize(18, 18));
    m_pullTable->setSortingEnabled(true);
    QHeaderView *ph = m_pullTable->horizontalHeader();
    ph->setHighlightSections(false);
    ph->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ph->setSectionResizeMode(1, QHeaderView::Stretch);
    m_pullTable->setItemDelegateForColumn(0,
                                          new PullCompactMetaDelegate(m_pullTable));
    m_pullTable->verticalHeader()->setDefaultSectionSize(
        qMax(m_pullTable->verticalHeader()->defaultSectionSize(), 28));

    // Reading the ledger is a worker round trip, so landing on the tab shows
    // this row until the first snapshot arrives instead of an empty list that
    // looks like a repository with no pull requests.
    m_pullListLoading = new QWidget;
    m_pullListLoading->setObjectName(QStringLiteral("pullListLoadingRow"));
    auto *loadingRow = new QHBoxLayout(m_pullListLoading);
    loadingRow->setContentsMargins(10, 8, 10, 8);
    loadingRow->setSpacing(8);
    loadingRow->addWidget(new BusySpinner(m_pullListLoading, 14), 0,
                          Qt::AlignVCenter);
    auto *loadingLabel =
        new QLabel(QString::fromUtf8("Loading pull requests\xE2\x80\xA6"));
    loadingLabel->setObjectName(QStringLiteral("pullListLoadingLabel"));
    loadingRow->addWidget(loadingLabel, 0, Qt::AlignVCenter);
    loadingRow->addStretch(1);
    m_pullListLoading->hide();

    auto *listLayout = new QVBoxLayout(listPane);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(0);
    listLayout->addWidget(actionBar);
    listLayout->addWidget(actionBarDivider);
    listLayout->addWidget(m_pullListLoading);
    listLayout->addWidget(m_pullTable, 1);
    listLayout->addWidget(buildMergeQueuePanel());
    return listPane;
}

QHBoxLayout *MainWindow::buildPullHeaderRow()
{
    m_pullUpdateButton = pullActionButton(
        QStringLiteral("Update"), "sync",
        QStringLiteral("Merge the base branch into this pull request branch"));
    m_pullMergeButton = pullActionButton(QStringLiteral("Merge"), "check-circle");
    m_pullResolveButton = pullActionButton(QStringLiteral("Resolve"),
                                           "git-pull-request");
    m_pullFixButton = pullActionButton(QStringLiteral("Fix"), "rocket");
    m_pullFixConflictsButton = pullActionButton(QStringLiteral("Agent fix"),
                                                "git-merge");
    m_pullEditFileButton = pullActionButton(QStringLiteral("Edit file"), "pencil");
    m_pullDeleteFileButton =
        pullActionButton(QStringLiteral("Delete file"), "trash");
    m_pullCloseButton = pullActionButton(QStringLiteral("Close"), "circle-slash");
    m_pullReopenButton = pullActionButton(QStringLiteral("Reopen"),
                                          "issue-reopened");
    m_pullRestoreButton = pullActionButton(
        QStringLiteral("Restore"), "history",
        QStringLiteral("Restore this archived pull request to the current pull "
                       "ledger so it can be reviewed, closed, merged, or deleted"));
    m_pullRestoreButton->setObjectName(
        QStringLiteral("pullRestoreArchivedButton"));
    m_pullSendToSourceButton = pullActionButton(QStringLiteral("Send"), "upload");
    m_pullDeleteButton = pullActionButton(QStringLiteral("Delete PR"), "trash");
    m_pullDeleteBranchButton =
        pullActionButton(QStringLiteral("Delete + branch"), "trash");
    m_pullMergeDeleteButton =
        pullActionButton(QStringLiteral("Merge + delete"), "check-circle");
    m_pullQueueButton = pullActionButton(QStringLiteral("Queue"), "list-unordered");
    m_pullQueueButton->hide();
    connect(m_pullQueueButton, &QPushButton::clicked, this,
            &MainWindow::toggleCurrentPullInMergeQueue);
    m_pullPreviewButton = pullActionButton(QStringLiteral("Preview"),
                                           "device-desktop");
    m_pullLinkIssueButton = pullActionButton(
        QStringLiteral("Link issue"), "link",
        QStringLiteral("Link an issue to this pull request"));
    m_pullSplitButton = pullActionButton(QString(), "diff");
    m_pullSplitButton->setCheckable(true);
    m_pullSplitButton->setChecked(diffSplitPref());
    updateDiffSplitButton(m_pullSplitButton);
    connect(m_pullSplitButton, &QPushButton::clicked, this, [this](bool on) {
        setDiffSplitPref(on);
        updateDiffSplitButton(m_pullSplitButton);
        for (QPushButton *b : {m_commitSplitButton, m_branchSplitButton}) {
            if (b) {
                b->setChecked(on);
                updateDiffSplitButton(b);
            }
        }
        if (m_pullFiles && m_pullFiles->count() > 0)
            renderPullDiff();
    });
    connect(m_pullLinkIssueButton, &QPushButton::clicked, this,
            &MainWindow::linkIssueToPullFromPullPage);
    auto *pullCopyLinkButton = pullActionButton(
        QStringLiteral("Copy link"), "copy",
        QStringLiteral("Copy a link to this pull request you can paste into an "
                       "issue or PR comment"));
    connect(pullCopyLinkButton, &QPushButton::clicked, this, [this] {
        if (m_currentPullNumber > 0)
            copyReferenceLink(QStringLiteral("pull"),
                              QString::number(m_currentPullNumber));
    });
    auto *pullViewWebsiteButton = pullActionButton(
        QStringLiteral("Website"), "link",
        QStringLiteral("Open this pull request on the public website"));
    connect(pullViewWebsiteButton, &QPushButton::clicked, this, [this] {
        if (m_currentPullNumber <= 0 || m_repoDetailIndex < 0 ||
            m_repoDetailIndex >= m_repositories.size())
            return;
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        QDesktopServices::openUrl(QUrl(repositoryWebUrl(repo) + "/pulls/" +
                                       QString::number(m_currentPullNumber)));
    });
    m_pullCloseButton->setToolTip("Close this pull request without merging it");
    m_pullReopenButton->setToolTip("Reopen this pull request");
    m_pullReopenButton->hide(); // only shown when the PR is closed or merged
    m_pullRestoreButton->hide(); // only shown for code-branch-only records
    m_pullSendToSourceButton->setToolTip(
        "Deliver this pull request to the repository owner's inbox. The relay "
        "holds it, so it reaches the source of truth even while that node is "
        "offline.");
    m_pullSendToSourceButton->hide(); // only shown on mirror nodes (can't merge here)
    m_pullDeleteButton->setToolTip("Permanently delete this pull request");
    m_pullDeleteBranchButton->setToolTip(
        "Permanently delete this pull request and its head branch");
    m_pullMergeDeleteButton->setToolTip(
        "Merge this pull request, then permanently delete it and its head branch");
    m_pullPreviewButton->setToolTip(
        "Check out this pull request, build the app from it, and launch the result "
        "as an isolated preview \xE2\x80\x94 try the change running before merging");
    m_pullPreviewButton->hide(); // only shown for buildable ForkMesh checkouts
    connect(m_pullPreviewButton, &QPushButton::clicked, this,
            &MainWindow::buildAndPreviewCurrentPull);
    m_pullUpdateButton->setToolTip("Merge the base branch into this pull request branch");
    m_pullResolveButton->setToolTip(
        "Open a merge editor to resolve this pull request's conflicts and commit "
        "the fix to its branch (the PR stays open, ready to merge)");
    m_pullResolveButton->hide(); // only shown when the PR has conflicts
    connect(m_pullResolveButton, &QPushButton::clicked, this,
            &MainWindow::resolveCurrentPullConflicts);
    m_pullFixButton->setToolTip(
        "Start the agent chosen beside this button on this pull request's own "
        "branch, in a checkout of its code, to resolve the conflicts and commit "
        "the fix there");
    m_pullFixButton->hide(); // only shown when the PR has conflicts
    connect(m_pullFixButton, &QPushButton::clicked, this,
            &MainWindow::fixCurrentPullWithAgent);
    // Which agent takes the job. Only the ten most-run agent+model pairs are
    // offered: the point of the button is one click on a conflicted PR, and
    // that is spoiled by scrolling the whole model catalog for it.
    m_pullFixModelCombo = new QComboBox;
    m_pullFixModelCombo->setObjectName(QStringLiteral("issueControlSm"));
    m_pullFixModelCombo->setCursor(Qt::PointingHandCursor);
    m_pullFixModelCombo->setToolTip(
        "Which agent and model \"Fix\" starts on this pull request's branch. A "
        "pull request whose own agent is still around is asked to fix it "
        "itself, since that session is the one holding the branch.");
    m_pullFixModelCombo->hide(); // shown with the Fix button it belongs to
    refreshPullFixModelCombo();
    connect(m_pullFixModelCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                if (!m_pullFixModelCombo)
                    return;
                QSettings().setValue(kPullFixAgentModelSetting,
                                     m_pullFixModelCombo->currentData().toString());
            });
    m_pullFixConflictsButton->hide();
    connect(m_pullFixConflictsButton, &QPushButton::clicked, this,
            &MainWindow::fixCurrentPullConflictsWithOriginatingAgent);
    m_pullEditFileButton->setToolTip(
        "Edit the selected file and commit the change to this pull request's "
        "branch (the PR stays open, ready to merge)");
    connect(m_pullEditFileButton, &QPushButton::clicked, this,
            &MainWindow::editCurrentPullFile);
    m_pullDeleteFileButton->setToolTip(
        "Delete the selected file and commit the deletion to this pull request's "
        "branch (the PR stays open, ready to merge)");
    connect(m_pullDeleteFileButton, &QPushButton::clicked, this,
            &MainWindow::deleteCurrentPullFile);
    m_pullReviewAiButton = pullActionButton(
        QStringLiteral("AI review"), "eye",
        QStringLiteral("Ask an AI to review this pull request's diff. Each "
                       "finding is posted as a review thread on the lines of "
                       "code it concerns; findings with a safe mechanical fix "
                       "get a one-click \"Apply fix & commit\"."));
    m_pullFixAllAiButton = pullActionButton(QStringLiteral("Fix all"), "rocket");
    connect(m_pullReviewAiButton, &QPushButton::clicked, this,
            &MainWindow::reviewCurrentPullWithAi);
    m_pullFixAllAiButton->setToolTip(
        "Let a Claude Code agent work through every unresolved review finding "
        "\xE2\x80\x94 including the ones without a quick fix \xE2\x80\x94 and "
        "commit the fixes to this pull request's branch");
    m_pullFixAllAiButton->hide(); // only shown when unresolved findings exist
    connect(m_pullFixAllAiButton, &QPushButton::clicked, this,
            &MainWindow::fixCurrentPullFindingsWithAgent);
    auto *pullHeaderRow = new QHBoxLayout;
    pullHeaderRow->setContentsMargins(0, 0, 0, 0);
    pullHeaderRow->setSpacing(10);
    pullHeaderRow->addWidget(m_pullSplitButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullReviewAiButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullFixAllAiButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullPreviewButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullUpdateButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullResolveButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullFixConflictsButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullFixButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullFixModelCombo, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullEditFileButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullDeleteFileButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullMergeButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullQueueButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullMergeDeleteButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullReopenButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullRestoreButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullSendToSourceButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullLinkIssueButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(pullCopyLinkButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(pullViewWebsiteButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullCloseButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullDeleteButton, 0, Qt::AlignTop);
    pullHeaderRow->addWidget(m_pullDeleteBranchButton, 0, Qt::AlignTop);
    pullHeaderRow->addStretch(1);
    return pullHeaderRow;
}

QWidget *MainWindow::buildPullFilesPage()
{
    m_pullFiles = new QListWidget;
    m_pullFiles->setObjectName("overviewList");
    enableHoverRowHighlight(m_pullFiles);
    m_pullFiles->setMinimumWidth(180);
    connect(m_pullFiles, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item, QListWidgetItem *) {
                if (item && !m_pullSuppressFileScroll)
                    scrollPullDiffToFile(item->data(Qt::UserRole).toString());
                if (!item) {
                    if (m_pullEditFileButton)
                        m_pullEditFileButton->setEnabled(false);
                    if (m_pullDeleteFileButton)
                        m_pullDeleteFileButton->setEnabled(false);
                }
            });

    m_pullPrevButton = new QPushButton;
    m_pullPrevButton->setToolTip("Previous change");
    setOcticon(m_pullPrevButton, "chevron-up", 14);
    connect(m_pullPrevButton, &QPushButton::clicked, this,
            [this] { pullSelectAdjacentChange(-1); });
    m_pullNextButton = new QPushButton;
    m_pullNextButton->setToolTip("Next change");
    setOcticon(m_pullNextButton, "chevron-down", 14);
    connect(m_pullNextButton, &QPushButton::clicked, this,
            [this] { pullSelectAdjacentChange(1); });
    m_diffFontPt = qBound(8, QSettings().value(kDiffFontPtSetting, 12).toInt(), 28);
    auto *diffZoomOut = new QPushButton(QString::fromUtf8("\xE2\x88\x92")); // −
    diffZoomOut->setToolTip("Smaller diff text");
    connect(diffZoomOut, &QPushButton::clicked, this, [this] { adjustDiffFont(-1); });
    auto *diffZoomIn = new QPushButton(QStringLiteral("+"));
    diffZoomIn->setToolTip("Larger diff text");
    connect(diffZoomIn, &QPushButton::clicked, this, [this] { adjustDiffFont(1); });
    m_pullAutoViewedButton = new QPushButton;
    m_pullAutoViewedButton->setCheckable(true);
    m_pullAutoViewedButton->setChecked(autoMarkViewedOnScrollPref());
    m_pullAutoViewedButton->hide();
    setOcticon(m_pullAutoViewedButton, "eye", 14);
    m_pullAutoViewedButton->setToolTip(
        "Automatically mark files as viewed while scrolling");
    connect(m_pullAutoViewedButton, &QPushButton::clicked, this, [this](bool on) {
        setAutoMarkViewedOnScrollPref(on);
        if (on)
            applyAutoMarkViewedOnScroll(); // catch up on the current scroll position
    });
    for (QPushButton *b : {m_pullPrevButton, m_pullNextButton, diffZoomOut, diffZoomIn,
                          m_pullAutoViewedButton}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    m_pullViewedLabel = new QLabel;
    m_pullViewedLabel->setObjectName("statusLine");
    m_pullViewedLabel->setTextFormat(Qt::RichText);
    m_pullViewedLabel->setToolTip(
        "A green check means every file is viewed as it stands in the latest push");
    m_pullViewedLabel->hide();
    auto *filesHeader = new QHBoxLayout;
    filesHeader->setContentsMargins(0, 0, 0, 0);
    auto *filesHeaderLabel = new QLabel("FILES");
    filesHeaderLabel->setObjectName("sectionLabel");
    filesHeader->addWidget(filesHeaderLabel);
    filesHeader->addStretch();
    filesHeader->addWidget(diffZoomOut);
    filesHeader->addWidget(diffZoomIn);
    filesHeader->addWidget(m_pullPrevButton);
    filesHeader->addWidget(m_pullNextButton);

    m_pullFileAuthorFilter = new QComboBox;
    m_pullFileAuthorFilter->addItem(QStringLiteral("All authors"));
    m_pullFileAuthorFilter->addItem(QStringLiteral("Agent-authored"));
    m_pullFileAuthorFilter->addItem(QStringLiteral("Human-authored"));
    m_pullFileAuthorFilter->setToolTip(
        QStringLiteral("Filter changed files by whether an agent authored them"));
    m_pullFileAuthorFilter->hide();
    connect(m_pullFileAuthorFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { applyPullFileAuthorFilter(); });

    auto *filesPane = new QWidget;
    auto *filesPaneLayout = new QVBoxLayout(filesPane);
    filesPaneLayout->setContentsMargins(0, 0, 0, 0);
    filesPaneLayout->setSpacing(6);
    filesPaneLayout->addLayout(filesHeader);
    filesPaneLayout->addWidget(m_pullViewedLabel);
    filesPaneLayout->addWidget(m_pullFileAuthorFilter);
    filesPaneLayout->addWidget(m_pullFiles, 1);

    m_pullDiff = new QTextBrowser;
    m_pullDiff->setObjectName("diffView");
    m_pullDiff->setOpenExternalLinks(false);
    m_pullDiff->setOpenLinks(false); // we handle "cmt:" anchors ourselves
    connect(m_pullDiff, &QTextBrowser::anchorClicked, this,
            &MainWindow::onPullDiffAnchorClicked);
    registerDiffView(m_pullDiff);

    buildPullDiffOverlays();

    auto *diffSplit = new QSplitter(Qt::Horizontal);
    diffSplit->setChildrenCollapsible(false);
    diffSplit->addWidget(filesPane);
    diffSplit->addWidget(m_pullDiff);
    diffSplit->setStretchFactor(0, 0);
    diffSplit->setStretchFactor(1, 1);
    diffSplit->setSizes({240, 600});

    m_pullDiffSearchInput = new QLineEdit;
    m_pullDiffSearchInput->setObjectName("issueSearch");
    m_pullDiffSearchInput->setPlaceholderText("Find in diff\xE2\x80\xA6");
    m_pullDiffSearchInput->setClearButtonEnabled(true);
    connect(m_pullDiffSearchInput, &QLineEdit::textChanged, this,
            [this] { pullDiffSearchRecompute(); });
    connect(m_pullDiffSearchInput, &QLineEdit::returnPressed, this, [this] {
        pullDiffSearchGoTo(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier
                               ? -1
                               : 1);
    });
    m_pullDiffSearchCount = new QLabel;
    m_pullDiffSearchCount->setObjectName("hintLabel");
    auto *searchPrev = new QPushButton;
    searchPrev->setToolTip("Previous match");
    setOcticon(searchPrev, "chevron-up", 14);
    connect(searchPrev, &QPushButton::clicked, this,
            [this] { pullDiffSearchGoTo(-1); });
    auto *searchNext = new QPushButton;
    searchNext->setToolTip("Next match");
    setOcticon(searchNext, "chevron-down", 14);
    connect(searchNext, &QPushButton::clicked, this,
            [this] { pullDiffSearchGoTo(1); });
    auto *searchClose = new QPushButton;
    searchClose->setToolTip("Close find bar");
    setOcticon(searchClose, "x", 14);
    connect(searchClose, &QPushButton::clicked, this,
            [this] { togglePullDiffSearch(false); });
    for (QPushButton *b : {searchPrev, searchNext, searchClose}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    m_pullDiffSearchBar = new QWidget;
    auto *searchBarLayout = new QHBoxLayout(m_pullDiffSearchBar);
    searchBarLayout->setContentsMargins(0, 0, 0, 6);
    searchBarLayout->addWidget(m_pullDiffSearchInput, 1);
    searchBarLayout->addWidget(m_pullDiffSearchCount);
    searchBarLayout->addWidget(searchPrev);
    searchBarLayout->addWidget(searchNext);
    searchBarLayout->addWidget(searchClose);
    m_pullDiffSearchBar->setVisible(false);

    m_pullInlineBatchBar = new QFrame;
    m_pullInlineBatchBar->setObjectName("issueCommentCard");
    auto *batchLayout = new QHBoxLayout(m_pullInlineBatchBar);
    batchLayout->setContentsMargins(10, 6, 10, 6);
    m_pullInlineBatchLabel = new QLabel;
    m_pullInlineBatchLabel->setObjectName("hintLabel");
    m_pullInlineBatchSubmitButton =
        new QPushButton(QStringLiteral("Submit review"));
    m_pullInlineBatchDiscardButton = new QPushButton(QStringLiteral("Discard"));
    for (QPushButton *button : {m_pullInlineBatchDiscardButton,
                                m_pullInlineBatchSubmitButton}) {
        button->setObjectName("ghostButton");
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_pullInlineBatchSubmitButton, "paper-airplane", 14);
    setOcticon(m_pullInlineBatchDiscardButton, "trash", 14);
    batchLayout->addWidget(m_pullInlineBatchLabel, 1);
    batchLayout->addWidget(m_pullInlineBatchDiscardButton);
    batchLayout->addWidget(m_pullInlineBatchSubmitButton);
    connect(m_pullInlineBatchSubmitButton, &QPushButton::clicked, this,
            &MainWindow::submitPullInlineReviewBatch);
    connect(m_pullInlineBatchDiscardButton, &QPushButton::clicked, this,
            &MainWindow::discardPullInlineReviewBatch);
    m_pullInlineBatchBar->hide();

    auto *filesPage = new QWidget;
    auto *filesPageLayout = new QVBoxLayout(filesPage);
    filesPageLayout->setContentsMargins(0, 0, 0, 0);
    filesPageLayout->addWidget(m_pullDiffSearchBar);
    filesPageLayout->addWidget(m_pullInlineBatchBar);
    filesPageLayout->addWidget(diffSplit);

    auto *closeSearchShortcut = new QShortcut(QKeySequence(Qt::Key_Escape),
                                              m_pullDiffSearchInput);
    closeSearchShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(closeSearchShortcut, &QShortcut::activated, this,
            [this] { togglePullDiffSearch(false); });
    return filesPage;
}

void MainWindow::buildPullDiffOverlays()
{
    // A real editor over a reserved row in the QTextDocument: visually and
    // spatially this lives between code rows, while remaining an ordinary Qt
    // widget with reliable keyboard, clipboard and accessibility behaviour.
    m_pullInlineComposerFrame = new QFrame(m_pullDiff->viewport());
    m_pullInlineComposerFrame->setObjectName("pullInlineComposer");
    m_pullInlineComposerFrame->setAutoFillBackground(true);
    m_pullInlineComposerFrame->setStyleSheet(Theme::remapSheet(
        QStringLiteral("#pullInlineComposer{background:palette(base);"
                       "border:1px solid palette(mid);border-radius:6px;}")));
    auto *inlineLayout = new QVBoxLayout(m_pullInlineComposerFrame);
    inlineLayout->setContentsMargins(10, 8, 10, 8);
    inlineLayout->setSpacing(6);
    m_pullInlineComposerTitle = new QLabel;
    m_pullInlineComposerTitle->setObjectName("issueCommentTitle");
    inlineLayout->addWidget(m_pullInlineComposerTitle);
    m_pullInlineCommentEdit = new QPlainTextEdit;
    m_pullInlineCommentEdit->setPlaceholderText(
        QStringLiteral("Leave feedback for the agent. Markdown is supported."));
    m_pullInlineCommentEdit->setMinimumHeight(66);
    (new PromptSpellCheckHighlighter(m_pullInlineCommentEdit->document()))
        ->attachTo(m_pullInlineCommentEdit);
    inlineLayout->addWidget(m_pullInlineCommentEdit, 1);
    auto *inlineButtons = new QHBoxLayout;
    inlineButtons->setContentsMargins(0, 0, 0, 0);
    auto *inlineCancel = new QPushButton(QStringLiteral("Cancel"));
    m_pullInlineAddButton = new QPushButton(QStringLiteral("Add to review"));
    m_pullInlineNowButton = new QPushButton(QStringLiteral("Comment now"));
    m_pullInlineSubmitBatchButton =
        new QPushButton(QStringLiteral("Submit review"));
    for (QPushButton *button : {inlineCancel, m_pullInlineAddButton,
                                m_pullInlineNowButton,
                                m_pullInlineSubmitBatchButton}) {
        button->setObjectName("ghostButton");
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_pullInlineAddButton, "list-unordered", 14);
    setOcticon(m_pullInlineNowButton, "comment", 14);
    setOcticon(m_pullInlineSubmitBatchButton, "paper-airplane", 14);
    inlineButtons->addWidget(inlineCancel);
    inlineButtons->addStretch();
    inlineButtons->addWidget(m_pullInlineSubmitBatchButton);
    inlineButtons->addWidget(m_pullInlineAddButton);
    inlineButtons->addWidget(m_pullInlineNowButton);
    inlineLayout->addLayout(inlineButtons);
    connect(inlineCancel, &QPushButton::clicked, this,
            &MainWindow::cancelPullInlineComposer);
    connect(m_pullInlineAddButton, &QPushButton::clicked, this,
            &MainWindow::queuePullInlineComment);
    connect(m_pullInlineNowButton, &QPushButton::clicked, this,
            &MainWindow::submitPullInlineCommentNow);
    connect(m_pullInlineSubmitBatchButton, &QPushButton::clicked, this,
            &MainWindow::submitPullInlineReviewBatch);
    m_pullInlineComposerFrame->setFixedHeight(166);
    m_pullInlineComposerFrame->hide();

    m_pullStickyHeader = new QFrame(diffOverlayParent(m_pullDiff));
    m_pullStickyHeader->setObjectName("diffStickyHeader");
    {
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        m_pullStickyHeader->setStyleSheet(Theme::remapSheet(
            QStringLiteral("#diffStickyHeader{background:%1;border:none;}"
                           "#diffStickyHeader QLabel{background:transparent;}")
                .arg(dark ? "#161b22" : "#f6f8fa")));
        auto *sl = new QHBoxLayout(m_pullStickyHeader);
        sl->setContentsMargins(12, 9, 12, 9);
        sl->setSpacing(0);
        m_pullStickyPath = new QLabel(m_pullStickyHeader);
        m_pullStickyPath->setTextFormat(Qt::RichText);
        m_pullStickyPath->setTextInteractionFlags(Qt::NoTextInteraction);
        configureDiffStickyPathLabel(m_pullStickyPath);
        sl->addWidget(m_pullStickyPath, 1);
        m_pullStickyControls = new QLabel(m_pullStickyHeader);
        m_pullStickyControls->setTextFormat(Qt::RichText);
        m_pullStickyControls->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
        m_pullStickyControls->setCursor(Qt::PointingHandCursor);
        connect(m_pullStickyControls, &QLabel::linkActivated, this,
                [this](const QString &link) {
                    const QUrl url(link);
                    // "expand:" only folds the file the bar is already on, so
                    // it goes through the plain handler: the advance-to-next
                    // landing below belongs to finishing a file, not reopening
                    // one.
                    if (url.scheme() != QLatin1String("viewed") ||
                        m_currentPullNumber < 0) {
                        onPullDiffAnchorClicked(url);
                        return;
                    }
                    const QString file = url.path();
                    const QString context =
                        QStringLiteral("pull/") + QString::number(m_currentPullNumber);
                    const bool nowViewed = !loadDiffViewed(context).contains(file);
                    const int at = m_pullFileOrder.indexOf(file);
                    const QString next =
                        nowViewed && at >= 0 && at + 1 < m_pullFileOrder.size()
                            ? m_pullFileOrder.at(at + 1)
                            : file;
                    setDiffViewed(context, file, nowViewed);
                    if (!swapPullDiffFileInPlace(file))
                        renderPullDiff();
                    scrollPullDiffToFile(next);
                });
        sl->addWidget(m_pullStickyControls, 0);
        m_pullStickyHeader->hide();
    }

    m_pullAutoViewedDebounce = new QTimer(this);
    m_pullAutoViewedDebounce->setSingleShot(true);
    m_pullAutoViewedDebounce->setInterval(400);
    connect(m_pullAutoViewedDebounce, &QTimer::timeout, this,
            &MainWindow::applyAutoMarkViewedOnScroll);
    connect(m_pullDiff->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this] {
                updatePullDiffScrollState();
                positionPullInlineComposer();
                m_pullAutoViewedDebounce->start();
            });
}

void MainWindow::buildPullCommitsList()
{
    m_pullCommitsList = new QListWidget;
    m_pullCommitsList->setObjectName("overviewList");
    enableHoverRowHighlight(m_pullCommitsList);
    connect(m_pullCommitsList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                const QString sha = item ? item->data(kCommitShaRole).toString()
                                         : QString();
                if (sha.isEmpty())
                    return;
                showOverviewCommits();
                showCommit(sha);
            });
    m_pullCommitsList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_pullCommitsList, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                QListWidgetItem *item = m_pullCommitsList->itemAt(pos);
                if (!item)
                    return;
                QString sha = item->data(kCommitShaRole).toString();
                if (sha.isEmpty())
                    sha = item->data(kCommitCopyShaRole).toString();
                const QString message = item->data(kCommitMessageRole).toString();
                QMenu menu(m_pullCommitsList);
                QAction *copyHash =
                    sha.isEmpty() ? nullptr
                                  : menu.addAction(QStringLiteral("Copy commit hash"));
                QAction *copyMessage =
                    message.isEmpty()
                        ? nullptr
                        : menu.addAction(QStringLiteral("Copy commit message"));
                if (!copyHash && !copyMessage)
                    return;
                QAction *chosen =
                    menu.exec(m_pullCommitsList->viewport()->mapToGlobal(pos));
                if (chosen && chosen == copyHash) {
                    QApplication::clipboard()->setText(sha);
                    flashMessage("Commit hash copied.");
                } else if (chosen && chosen == copyMessage) {
                    QApplication::clipboard()->setText(message);
                    flashMessage("Commit message copied.");
                }
            });
}

QWidget *MainWindow::buildPullChecksPage()
{
    m_pullRunChecksButton = new QPushButton("Run checks against this PR");
    m_pullRunChecksButton->setObjectName("ghostButton");
    m_pullRunChecksButton->setProperty("buttonSize", "sm");
    m_pullRunChecksButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_pullRunChecksButton, "workflow", 16);
    m_pullRunChecksButton->setToolTip(
        "Publish this pull request's head to the runner and queue this "
        "repository's workflows against it now");
    connect(m_pullRunChecksButton, &QPushButton::clicked, this,
            &MainWindow::runChecksForCurrentPull);
    m_pullApproveChecksButton = new QPushButton("Approve and run");
    m_pullApproveChecksButton->setObjectName("ghostButton");
    m_pullApproveChecksButton->setProperty("buttonSize", "sm");
    m_pullApproveChecksButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_pullApproveChecksButton, "check-circle", 16);
    m_pullApproveChecksButton->setToolTip(
        "Let this pull request's held checks run on this node");
    m_pullApproveChecksButton->hide();
    connect(m_pullApproveChecksButton, &QPushButton::clicked, this,
            &MainWindow::approveChecksForCurrentPull);
    m_pullChecksNotice = new QLabel;
    m_pullChecksNotice->setObjectName("hintLabel");
    m_pullChecksNotice->setWordWrap(true);
    m_pullChecksNotice->hide();
    auto *checksToolbar = new QHBoxLayout;
    checksToolbar->setContentsMargins(0, 0, 0, 0);
    checksToolbar->addWidget(m_pullRunChecksButton);
    checksToolbar->addWidget(m_pullApproveChecksButton);
    checksToolbar->addWidget(m_pullChecksNotice, 1);
    checksToolbar->addStretch();
    m_pullChecksTable = new QTableWidget(0, 4);
    m_pullChecksTable->setObjectName("issueTable");
    installColumnHeaderMenu(m_pullChecksTable);
    enableHoverRowHighlight(m_pullChecksTable);
    m_pullChecksTable->setHorizontalHeaderLabels(
        {"Status", "Workflow", "Commit", "Duration"});
    m_pullChecksTable->verticalHeader()->setVisible(false);
    m_pullChecksTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_pullChecksTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_pullChecksTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pullChecksTable->setShowGrid(false);
    m_pullChecksTable->setWordWrap(false);
    QHeaderView *ch = m_pullChecksTable->horizontalHeader();
    ch->setHighlightSections(false);
    ch->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    ch->setSectionResizeMode(1, QHeaderView::Stretch);
    ch->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    ch->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_pullChecksTable);
    connect(m_pullChecksTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QModelIndexList rows = m_pullChecksTable->selectionModel()->selectedRows();
        if (rows.isEmpty())
            return;
        if (QTableWidgetItem *first = m_pullChecksTable->item(rows.first().row(), 0))
            showPullCheckLog(first->data(Qt::UserRole).toInt());
    });
    m_pullChecksLog = new QPlainTextEdit;
    m_pullChecksLog->setObjectName("actionLog");
    m_pullChecksLog->setReadOnly(true);
    m_pullChecksLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    applyLogFont(m_pullChecksLog);
    auto *checksSplit = new QSplitter(Qt::Vertical);
    checksSplit->setChildrenCollapsible(false);
    checksSplit->addWidget(m_pullChecksTable);
    checksSplit->addWidget(m_pullChecksLog);
    checksSplit->setStretchFactor(0, 1);
    checksSplit->setStretchFactor(1, 1);
    auto *checksPage = new QWidget;
    auto *checksPageLayout = new QVBoxLayout(checksPage);
    checksPageLayout->setContentsMargins(0, 0, 0, 0);
    checksPageLayout->setSpacing(8);
    checksPageLayout->addLayout(checksToolbar);
    checksPageLayout->addWidget(checksSplit, 1);
    return checksPage;
}

void MainWindow::buildPullConversation()
{
    m_pullThreadContainer = new QWidget;
    m_pullThreadLayout = new QVBoxLayout(m_pullThreadContainer);
    m_pullThreadLayout->setContentsMargins(0, 0, 0, 0);
    m_pullThreadLayout->setSpacing(10);
    m_pullThreadLayout->addStretch();

    m_pullChecksSummary = new QLabel;
    m_pullChecksSummary->setObjectName("issueTimelineCard");
    m_pullChecksSummary->setTextFormat(Qt::RichText);
    m_pullChecksSummary->setWordWrap(true);
    m_pullChecksSummary->setContentsMargins(16, 12, 16, 12);
    m_pullChecksSummary->hide();
    connect(m_pullChecksSummary, &QLabel::linkActivated, this, [this](const QString &) {
        if (m_pullTabChecks)
            m_pullTabChecks->setChecked(true);
        if (m_pullSubStack)
            m_pullSubStack->setCurrentIndex(2);
    });

    m_pullLinksValue = new QLabel;
    m_pullLinksValue->setObjectName("issueTimelineCard");
    m_pullLinksValue->setTextFormat(Qt::RichText);
    m_pullLinksValue->setWordWrap(true);
    m_pullLinksValue->setContentsMargins(16, 12, 16, 12);
    m_pullLinksValue->setOpenExternalLinks(false);
    m_pullLinksValue->hide();
    connect(m_pullLinksValue, &QLabel::linkActivated, this, [this](const QString &href) {
        const int n = href.section(QLatin1Char(':'), 1).toInt();
        if (n <= 0)
            return;
        if (m_repoDetailTabs && m_repoDetailTabs->button(2))
            m_repoDetailTabs->button(2)->click();
        reloadIssues();
        showIssue(n);
    });

    auto *composerBlock = buildPullComposerBlock();

    auto *conversationActions = buildPullConversationActions();

    buildPullConflictCards();

    auto *conversationInner = new QWidget;
    auto *conversationInnerLayout = new QVBoxLayout(conversationInner);
    conversationInnerLayout->setContentsMargins(0, 0, 0, 0);
    conversationInnerLayout->setSpacing(10);
    conversationInnerLayout->addWidget(m_pullThreadContainer);
    conversationInnerLayout->addWidget(new ThreadCardRow(m_pullLinksValue));
    conversationInnerLayout->addWidget(new ThreadCardRow(m_pullChecksSummary));
    conversationInnerLayout->addWidget(new ThreadCardRow(m_pullConflictCard));
    conversationInnerLayout->addWidget(composerBlock);

    m_pullAgentRevisionRow = new QWidget;
    m_pullAgentRevisionRow->setObjectName("agentRevisionRow");
    auto *agentRevisionLayout = new QHBoxLayout(m_pullAgentRevisionRow);
    agentRevisionLayout->setContentsMargins(0, 4, 0, 0);
    agentRevisionLayout->setSpacing(6);
    m_pullAgentRevisionEdit = new QLineEdit;
    m_pullAgentRevisionEdit->setPlaceholderText(
        "Describe the revision for the agent\xE2\x80\xA6");
    m_pullSendToAgentButton = new QPushButton("Send to agent");
    m_pullSendToAgentButton->setObjectName("primaryButton");
    m_pullSendToAgentButton->setProperty("buttonSize", "sm");
    m_pullSendToAgentButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_pullSendToAgentButton, "rocket", 16);
    m_pullSendToAgentButton->setMinimumWidth(
        m_pullSendToAgentButton->sizeHint().width());
    m_pullSendToAgentButton->setSizePolicy(QSizePolicy::Minimum,
                                           QSizePolicy::Fixed);
    m_pullSendToAgentButton->setToolTip(
        "Post this note as a PR comment and re-queue the agent with the revision "
        "instructions so it continues work on the same branch");
    agentRevisionLayout->addWidget(m_pullAgentRevisionEdit, 1);
    agentRevisionLayout->addWidget(m_pullSendToAgentButton);
    connect(m_pullSendToAgentButton, &QPushButton::clicked,
            this, &MainWindow::sendPullRevisionToAgent);
    connect(m_pullAgentRevisionEdit, &QLineEdit::returnPressed,
            this, &MainWindow::sendPullRevisionToAgent);
    m_pullAgentRevisionRow->hide(); // only visible when this PR has a linked agent
    conversationInnerLayout->addWidget(new ThreadCardRow(m_pullAgentRevisionRow));
    conversationInnerLayout->addWidget(new ThreadCardRow(m_pullMergeBlockers));
    conversationInnerLayout->addLayout(conversationActions);

    conversationInnerLayout->addStretch();

    m_pullThreadScroll = new QScrollArea;
    m_pullThreadScroll->setWidgetResizable(true);
    m_pullThreadScroll->setWidget(conversationInner);
    m_pullThreadScroll->setObjectName("issuePageScroll");
    m_pullThreadScroll->setFrameShape(QFrame::NoFrame);
}

QWidget *MainWindow::buildPullComposerBlock()
{
    m_pullComposer = new MarkdownEditor;
    m_pullComposer->setObjectName("issueCommentEditor");
    m_pullComposer->layout()->setContentsMargins(1, 1, 1, 1);
    m_pullComposer->setPlaceholderText("Leave a comment or review\xE2\x80\xA6");
    m_pullComposer->setMinimumHeight(90);
    m_pullCommentButton = new QPushButton("Comment");
    m_pullApproveButton = new QPushButton("Approve");
    m_pullRequestChangesButton = new QPushButton("Request changes");
    for (QPushButton *b : {m_pullCommentButton, m_pullApproveButton,
                           m_pullRequestChangesButton}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_pullCommentButton, "comment", 16);
    setOcticon(m_pullApproveButton, "check-circle", 16);
    setOcticon(m_pullRequestChangesButton, "alert", 16);
    connect(m_pullCommentButton, &QPushButton::clicked, this,
            &MainWindow::submitPullComment);
    connect(m_pullApproveButton, &QPushButton::clicked, this,
            [this] { submitPullReview(QStringLiteral("approved")); });
    connect(m_pullRequestChangesButton, &QPushButton::clicked, this,
            [this] { submitPullReview(QStringLiteral("changes_requested")); });
    auto *composerButtons = new QHBoxLayout;
    composerButtons->setContentsMargins(0, 0, 0, 0);
    composerButtons->addWidget(makeVoiceButton(m_pullComposer), 0, Qt::AlignLeft);
    composerButtons->addStretch();
    composerButtons->addWidget(m_pullRequestChangesButton);
    composerButtons->addWidget(m_pullApproveButton);
    composerButtons->addWidget(m_pullCommentButton);
    // The composer is the last card of the conversation, not a form bolted
    // under it: same circular avatar in the same avatar column, same card as
    // every comment above it.
    m_pullComposerAvatar = new QLabel("FM");
    m_pullComposerAvatar->setObjectName("issueAvatar");
    m_pullComposerAvatar->setAlignment(Qt::AlignCenter);
    m_pullComposerAvatar->setFixedSize(kPullThreadAvatarPx, kPullThreadAvatarPx);
    m_pullComposerAvatar->setScaledContents(true);
    m_pullComposerTitle = new QLabel;
    m_pullComposerTitle->setObjectName("issueCommentTitle");
    m_pullComposerTitle->setTextFormat(Qt::RichText);
    refreshPullComposerIdentity();
    auto *composerCard = new QWidget;
    composerCard->setObjectName("issueCommentCard");
    auto *composerCardLayout = new QVBoxLayout(composerCard);
    composerCardLayout->setContentsMargins(12, 10, 12, 12);
    composerCardLayout->setSpacing(8);
    composerCardLayout->addWidget(m_pullComposerTitle);
    composerCardLayout->addWidget(m_pullComposer);
    composerCardLayout->addLayout(composerButtons);
    auto *composerBlock = new QWidget;
    composerBlock->setObjectName("issueTimelineRow");
    auto *composerBlockLayout = new QHBoxLayout(composerBlock);
    composerBlockLayout->setContentsMargins(0, 0, 0, 0);
    composerBlockLayout->setSpacing(kPullThreadGutterPx);
    composerBlockLayout->addWidget(m_pullComposerAvatar, 0, Qt::AlignTop);
    composerBlockLayout->addWidget(composerCard, 1);
    return composerBlock;
}

QHBoxLayout *MainWindow::buildPullConversationActions()
{
    m_pullConversationMergeButton = new QPushButton("Merge pull request");
    m_pullConversationMergeButton->setObjectName("primaryButton");
    m_pullConversationUpdateButton = new QPushButton("Update branch");
    m_pullConversationUpdateButton->setObjectName("ghostButton");
    m_pullConversationCloseButton = new QPushButton("Close pull request");
    m_pullConversationCloseButton->setObjectName("ghostButton");
    m_pullConversationDeleteButton = new QPushButton("Delete pull request");
    m_pullConversationDeleteButton->setObjectName("dangerButton");
    for (QPushButton *b : {m_pullConversationMergeButton,
                           m_pullConversationUpdateButton,
                           m_pullConversationCloseButton,
                           m_pullConversationDeleteButton}) {
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_pullConversationMergeButton, "check-circle", 16);
    setOcticon(m_pullConversationUpdateButton, "git-merge", 16);
    setOcticon(m_pullConversationCloseButton, "circle-slash", 16);
    setOcticon(m_pullConversationDeleteButton, "trash", 16);
    connect(m_pullConversationMergeButton, &QPushButton::clicked, this,
            &MainWindow::mergeCurrentPull);
    connect(m_pullConversationUpdateButton, &QPushButton::clicked, this,
            &MainWindow::updateCurrentPullBranch);
    connect(m_pullConversationCloseButton, &QPushButton::clicked, this,
            &MainWindow::closeCurrentPull);
    connect(m_pullConversationDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteCurrentPull);
    auto *conversationActions = new QHBoxLayout;
    conversationActions->setContentsMargins(0, 0, 0, 0);
    conversationActions->setSpacing(6);
    conversationActions->addStretch();
    conversationActions->addWidget(m_pullConversationUpdateButton);
    conversationActions->addWidget(m_pullConversationMergeButton);
    conversationActions->addWidget(m_pullConversationCloseButton);
    conversationActions->addWidget(m_pullConversationDeleteButton);
    return conversationActions;
}

void MainWindow::buildPullConflictCards()
{
    m_pullConflictCard = new QFrame;
    m_pullConflictCard->setObjectName("issueTimelineCard");
    auto *conflictCardLayout = new QVBoxLayout(m_pullConflictCard);
    conflictCardLayout->setContentsMargins(16, 12, 16, 12);
    conflictCardLayout->setSpacing(8);
    m_pullConflictDetails = new QLabel;
    m_pullConflictDetails->setTextFormat(Qt::RichText);
    m_pullConflictDetails->setWordWrap(true);
    m_pullConflictDetails->setOpenExternalLinks(false);
    connect(m_pullConflictDetails, &QLabel::linkActivated, this,
            [this](const QString &) {
                openPullDiffInGitView(m_currentPullNumber);
            });
    // Shown only while nobody has asked the author to fix this conflict yet:
    // one click posts the same notice a freshly appeared conflict gets.
    m_pullConflictMessageAuthorButton =
        new QPushButton(QStringLiteral("Message author"));
    m_pullConflictMessageAuthorButton->setObjectName("ghostButton");
    m_pullConflictMessageAuthorButton->setProperty("buttonSize", "sm");
    m_pullConflictMessageAuthorButton->setCursor(Qt::PointingHandCursor);
    m_pullConflictMessageAuthorButton->setToolTip(QStringLiteral(
        "Comment on this pull request asking its author to merge the base "
        "branch in, resolve the conflicts and push"));
    setOcticon(m_pullConflictMessageAuthorButton, "comment", 16);
    connect(m_pullConflictMessageAuthorButton, &QPushButton::clicked, this,
            [this] {
                const int number = m_currentPullNumber;
                const auto cached = m_pullConflictCache.constFind(number);
                if (cached == m_pullConflictCache.constEnd() || !cached->conflict)
                    return;
                postPullConflictNotice(number, cached->fingerprint,
                                       cached->conflictFiles, false);
            });
    auto *conflictCardActions = new QHBoxLayout;
    conflictCardActions->setContentsMargins(0, 0, 0, 0);
    conflictCardActions->addWidget(m_pullConflictMessageAuthorButton);
    conflictCardActions->addStretch();
    conflictCardLayout->addWidget(m_pullConflictDetails);
    conflictCardLayout->addLayout(conflictCardActions);
    m_pullConflictCard->hide();

    // Why the merge button is dead, spelled out where the merge button is. The
    // banner under the title says the same thing, but it has scrolled far out of
    // sight by the time a reader reaches the bottom of a busy conversation.
    m_pullMergeBlockers = new QLabel;
    m_pullMergeBlockers->setObjectName("issueTimelineCard");
    m_pullMergeBlockers->setTextFormat(Qt::RichText);
    m_pullMergeBlockers->setWordWrap(true);
    m_pullMergeBlockers->setContentsMargins(16, 12, 16, 12);
    m_pullMergeBlockers->setOpenExternalLinks(false);
    m_pullMergeBlockers->hide();
    connect(m_pullMergeBlockers, &QLabel::linkActivated, this,
            [this](const QString &href) {
                if (href == QLatin1String("action:update"))
                    updateCurrentPullBranch();
                else
                    openPullDiffInGitView(m_currentPullNumber);
            });
}

QHBoxLayout *MainWindow::buildPullSubTabs(QWidget *checksPage, QWidget *filesPage)
{
    m_pullBadgeWidget = new PullBadgeWidget;
    auto *badgeScroll = new QScrollArea;
    badgeScroll->setWidgetResizable(true);
    badgeScroll->setWidget(m_pullBadgeWidget);
    badgeScroll->setObjectName("issuePageScroll");
    badgeScroll->setFrameShape(QFrame::NoFrame);

    m_pullSubStack = new QStackedWidget;
    m_pullSubStack->addWidget(m_pullThreadScroll); // 0 Conversation
    m_pullSubStack->addWidget(m_pullCommitsList);  // 1 Commits
    m_pullSubStack->addWidget(checksPage);         // 2 Checks
    m_pullSubStack->addWidget(filesPage);          // 3 Files changed
    m_pullSubStack->addWidget(badgeScroll);        // 4 Badge

    m_pullSubTabs = new QButtonGroup(this);
    m_pullSubTabs->setExclusive(true);
    auto *subTabRow = new QHBoxLayout;
    subTabRow->setContentsMargins(0, 0, 0, 0);
    subTabRow->setSpacing(10); // same rhythm as the repository tab row
    const QList<QPair<QString, const char *>> subTabs = {
        {QStringLiteral("Conversation"), "comment"},
        {QStringLiteral("Commits"), "git-branch"},
        {QStringLiteral("Checks"), "workflow"},
        {QStringLiteral("Changes in Git"), "file-diff"},
        {QStringLiteral("Badge"), "graph"}};
    for (int i = 0; i < subTabs.size(); ++i) {
        QPushButton *b =
            new VerticalIconButton(subTabs.at(i).first, VerticalIconButton::Tab);
        b->setObjectName("repoTab");
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        setOcticon(b, QString::fromLatin1(subTabs.at(i).second), 16);
        if (i == 0)
            b->setChecked(true);
        m_pullSubTabs->addButton(b, i);
        subTabRow->addWidget(b);
    }
    m_pullSubTabs->button(3)->setToolTip(
        QStringLiteral("Open this pull request's changes in the Git range view"));
    subTabRow->addStretch();
    m_pullTabConversation = qobject_cast<QPushButton *>(m_pullSubTabs->button(0));
    m_pullTabCommits = qobject_cast<QPushButton *>(m_pullSubTabs->button(1));
    m_pullTabChecks = qobject_cast<QPushButton *>(m_pullSubTabs->button(2));
    m_pullTabFiles = qobject_cast<QPushButton *>(m_pullSubTabs->button(3));
    m_pullTabBadge = qobject_cast<QPushButton *>(m_pullSubTabs->button(4));
    connect(m_pullSubTabs, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == 3) {
            openPullDiffInGitView(m_currentPullNumber);
            return;
        }
        m_pullSubStack->setCurrentIndex(id);
        if (id == 2) { // refresh the Checks table when it's brought forward
            PullRequest current;
            for (const PullRequest &pr : std::as_const(m_currentPulls))
                if (pr.number == m_currentPullNumber)
                    current = pr;
            if (current.number > 0)
                renderPullChecks(current);
        }
        scheduleNavRecord();
    });
    return subTabRow;
}


void MainWindow::setPullListLoading(bool loading)
{
    if (!m_pullListLoading)
        return;
    // Only while there is nothing to look at: the one-minute auto-refresh and
    // every post-action reload would otherwise flash a spinner over a list the
    // user is already reading.
    m_pullListLoading->setVisible(loading && m_pullTable &&
                                  m_pullTable->rowCount() == 0);
}

PullStore MainWindow::pullStoreForCurrentRepo() const
{
    return pullStoreForRepoIndex(m_repoDetailIndex);
}

PullStore MainWindow::pullStoreForRepoIndex(int index) const
{
    if (index < 0 || index >= m_repositories.size())
        return PullStore(QString(), QString(), &m_profileIdentity, m_userName);
    const RepositoryRecord &target = m_repositories.at(index);
    const RepositoryRecord &repo = writableRecordFor(target);
    PullStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity, m_userName);
    store.setSigningRepository(target.owner, target.name);
    return store;
}

bool MainWindow::currentAccountOwnsPullRepository()
{
    return accountOwnsPullRepositoryAt(m_repoDetailIndex);
}

bool MainWindow::accountOwnsPullRepositoryAt(int index)
{
    if (index < 0 || index >= m_repositories.size())
        return false;
    const RepositoryRecord &repository =
        writableRecordFor(m_repositories.at(index));
    if (PullMergePolicy::repositoryOwnedByAccount(
            repository.owner, accountOwner(), m_profileLinkedNodes)) {
        return true;
    }
    // Organization memberships are a relay-backed, account-scoped record and
    // Settings normally fetches them lazily. A PR can be opened before that
    // tab, though, so start the same signed read here and refresh the merge
    // controls when it completes instead of treating an org owner as a
    // contributor until they happen to visit Settings.
    if (!m_organizationsLoaded && !m_organizationsLoading) {
        m_organizationsLoaded = true;
        refreshOrganizations();
    }
    return PullMergePolicy::repositoryOwnedByOrganizationOwner(
        repository.owner, m_organizations);
}

QString MainWindow::pullPatchFingerprint(const QString &patch)
{
    return QString::number(patch.size()) + QLatin1Char(':') +
           QString::number(qHash(patch));
}

QString MainWindow::pullDetailDigest(const PullRequest &pr)
{
    QString digest = QString::number(pr.number) + QLatin1Char('|') + pr.status +
                     QLatin1Char('|') + pr.title + QLatin1Char('|') + pr.base +
                     QLatin1Char('|') + pr.head + QLatin1Char('|') +
                     pullPatchFingerprint(pr.patch);
    digest += pr.archivedOnCodeBranch ? QStringLiteral("|archived")
                                      : QStringLiteral("|ledger");
    for (const PullEvent &ev : pr.events) {
        digest += QLatin1Char('|') + ev.id + QLatin1Char(':') + ev.state +
                  QLatin1Char(':') + QString::number(ev.ts);
    }
    return digest;
}

void MainWindow::reloadPulls()
{
    if (m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    ++m_pullLoadGen; // supersede an older push-driven worker result
    const PullStore store = pullStoreForCurrentRepo();
    const bool includeArchived =
        (m_pullShowAllButton && m_pullShowAllButton->isChecked()) ||
        (m_pullShowClosedButton && m_pullShowClosedButton->isChecked()) ||
        (m_pullShowMergedButton && m_pullShowMergedButton->isChecked());
    applyLoadedPulls(store,
                     includeArchived ? store.loadAllIncludingArchived()
                                     : store.loadAll(),
                     store.baseTip());
}

void MainWindow::reloadPullsInBackground()
{
    if (m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const quint64 gen = ++m_pullLoadGen;
    setPullListLoading(true);
    if (m_pullBackgroundLoadInFlight) {
        m_pullBackgroundReloadQueued = true;
        return;
    }
    m_pullBackgroundLoadInFlight = true;
    const PullStore store = pullStoreForCurrentRepo();
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString repositoryKey =
        repo.owner + QLatin1Char('/') + repo.name + QLatin1Char('|') +
        repo.localPath + QLatin1Char('|') + repo.mirrorPath;
    const bool includeArchived =
        (m_pullShowAllButton && m_pullShowAllButton->isChecked()) ||
        (m_pullShowClosedButton && m_pullShowClosedButton->isChecked()) ||
        (m_pullShowMergedButton && m_pullShowMergedButton->isChecked());
    struct LoadedPulls {
        PullStore store;
        QList<PullRequest> pulls;
        QString baseTip;
        QByteArray snapshotDigest;
    };
    runOffThread<LoadedPulls>(
        [store, includeArchived, repositoryKey] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("pulls"), QStringLiteral("load pull metadata"),
                forkmesh::ActionTelemetry::Execution::Worker);
            QList<PullRequest> pulls =
                includeArchived ? store.loadAllIncludingArchived()
                                : store.loadAll();
            const QString baseTip = store.baseTip();
            const QByteArray digest =
                pullSnapshotDigest(pulls, baseTip, repositoryKey);
            return LoadedPulls{store, std::move(pulls), baseTip, digest};
        },
        [this, gen](LoadedPulls loaded) {
            m_pullBackgroundLoadInFlight = false;
            if (gen == m_pullLoadGen)
                applyLoadedPulls(loaded.store, std::move(loaded.pulls),
                                 loaded.baseTip, loaded.snapshotDigest);
            setPullListLoading(false);
            if (m_pullBackgroundReloadQueued) {
                m_pullBackgroundReloadQueued = false;
                reloadPullsInBackground();
            }
        });
}

void MainWindow::applyLoadedPulls(const PullStore &store,
                                  QList<PullRequest> pulls,
                                  const QString &baseTip,
                                  const QByteArray &snapshotDigest)
{
    // The auto-refresh timer fires every minute even when the ledger did not
    // move. Avoid rebuilding hundreds of rows, SVG icons, generated avatars,
    // fonts, and the open pull's controls for an identical worker snapshot —
    // but only once the list on screen actually shows that snapshot. The tab
    // is built lazily, so the first load usually lands while there is no
    // table at all, and skipping the next identical snapshot left the user
    // staring at an empty list until a filter button forced a rebuild.
    if (!snapshotDigest.isEmpty() && snapshotDigest == m_pullSnapshotDigest &&
        (!m_pullTable || m_pullListRenderedDigest == snapshotDigest))
        return;
    m_pullSnapshotDigest = snapshotDigest;
    m_currentPulls = std::move(pulls);
    m_pullRegisteredCount = qMax(m_pullRegisteredCount,
                                 int(m_currentPulls.size()));
    refreshBranchLinks();
    // Agent attribution for the list badges, off the GUI thread. Scanning a PR's
    // signed commit series for the ForkMesh-Agent trailer means walking the
    // whole mbox when there is no trailer to find, and refreshPullList() used to
    // do that for every visible row on every rebuild — including each search
    // keystroke. Compute it once per load here instead; until it lands the rows
    // simply carry no agent badge.
    {
        const quint64 provenanceGen = ++m_pullProvenanceGen;
        QList<QPair<int, QString>> series;
        series.reserve(m_currentPulls.size());
        for (const PullRequest &pr : std::as_const(m_currentPulls))
            series.append({pr.number, pr.commits}); // implicitly shared, cheap
        runOffThread<QHash<int, PullAgentProvenance>>(
            [series]() {
                QHash<int, PullAgentProvenance> map;
                for (const auto &entry : series) {
                    const PullAgentProvenance prov =
                        pullAgentProvenanceIn(entry.second);
                    if (prov.isAgent)
                        map.insert(entry.first, prov);
                }
                return map;
            },
            [this, provenanceGen](QHash<int, PullAgentProvenance> map) {
                if (provenanceGen != m_pullProvenanceGen)
                    return; // a newer load superseded this pass
                m_pullProvenance = std::move(map);
                if (m_pullTable)
                    refreshPullList(); // badge the rows now that we know
            });
    }
    const quint64 gen = ++m_pullConflictGen;
    m_pendingPullConflictChecks.clear();
    m_pullConflictByNumber.clear();
    if (store.canWrite()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        const QString cacheKey = repo.owner + QLatin1Char('/') + repo.name +
                                 QLatin1Char('@') + baseTip;
        if (cacheKey != m_pullConflictCacheBaseTip) {
            m_pullConflictCacheBaseTip = cacheKey;
            m_pullConflictCache.clear();
            m_pullBehindCache.clear();
        }
        QSet<int> openNumbers;
        for (const PullRequest &pr : std::as_const(m_currentPulls)) {
            if (pr.status != QLatin1String("open") ||
                pr.archivedOnCodeBranch)
                continue;
            openNumbers.insert(pr.number);
            const QString fingerprint = pullPatchFingerprint(pr.patch);
            const auto cached = m_pullConflictCache.constFind(pr.number);
            if (cached != m_pullConflictCache.constEnd() &&
                cached->fingerprint == fingerprint) {
                if (cached->conflict)
                    m_pullConflictByNumber.insert(pr.number, true);
            } else {
                m_pendingPullConflictChecks.append(qMakePair(pr.number, fingerprint));
            }
        }
        for (auto it = m_pullConflictCache.begin();
             it != m_pullConflictCache.end();) {
            if (openNumbers.contains(it.key()))
                ++it;
            else
                it = m_pullConflictCache.erase(it);
        }
    }
    refreshPullBranchState();
    updateRepoPullCount();
    if (m_pullTable) {
        refreshPullList();
        updatePullActionState();
    }
    if (m_repoDetailStack && m_repoDetailStack->currentIndex() == 3)
        refreshAgentTable();
    // Live detail: a reload that changed the pull shown in the detail pane
    // re-renders it in place, so a merge or new review lands without the user
    // reselecting the row. Skipped while the reply composer owns focus — a
    // re-render would yank the scroll position out from under a half-written
    // comment.
    if (m_currentPullNumber > 0 && m_pullDetail && m_pullDetail->isVisible() &&
        !m_pullDetailRenderedDigest.isEmpty()) {
        QWidget *focus = QApplication::focusWidget();
        const bool composing =
            m_pullComposer && focus &&
            (focus == m_pullComposer || m_pullComposer->isAncestorOf(focus));
        if (!composing) {
            for (const PullRequest &pr : std::as_const(m_currentPulls)) {
                if (pr.number != m_currentPullNumber)
                    continue;
                if (pullDetailDigest(pr) != m_pullDetailRenderedDigest)
                    showPull(m_currentPullNumber);
                break;
            }
        }
    }
    if (!m_pendingPullConflictChecks.isEmpty())
        QTimer::singleShot(0, this, [this, gen] { processPendingPullConflicts(gen); });
    // Every reload is a chance to notice a pull request whose head has never
    // been checked — one this node just created, one that arrived through the
    // inbox, or one whose contributor pushed a new commit.
    if (store.canWrite() && m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size()) {
        QList<PullRequest> ledgerPulls;
        ledgerPulls.reserve(m_currentPulls.size());
        for (const PullRequest &pull : std::as_const(m_currentPulls))
            if (!pull.archivedOnCodeBranch)
                ledgerPulls.append(pull);
        queueChecksForOpenPulls(
            writableRecordFor(m_repositories.at(m_repoDetailIndex)),
            ledgerPulls);
    }
    refreshMergeQueuePanel();
    scheduleMergeQueueRun(2000);
}

void MainWindow::processPendingPullConflicts(quint64 gen)
{
    if (gen != m_pullConflictGen || m_pendingPullConflictChecks.isEmpty())
        return;
    if (m_pullConflictCheckInFlight)
        return;
    const QPair<int, QString> item = m_pendingPullConflictChecks.takeFirst();
    const int number = item.first;
    const QString fingerprint = item.second;
    const PullStore store = pullStoreForCurrentRepo();
    if (!store.canWrite()) {
        if (gen == m_pullConflictGen && !m_pendingPullConflictChecks.isEmpty())
            QTimer::singleShot(0, this, [this, gen] { processPendingPullConflicts(gen); });
        return;
    }
    m_pullConflictCheckInFlight = true;
    auto clean = std::make_shared<bool>(false);
    auto mergeable = std::make_shared<bool>(false);
    auto conflictFiles = std::make_shared<QStringList>();
    QThread *worker = QThread::create([store, number, clean, mergeable,
                                       conflictFiles]() mutable {
        *mergeable = store.checkMergeable(number, clean.get(), conflictFiles.get(),
                                          nullptr, /*keepGuiAlive=*/false);
    });
    connect(worker, &QThread::finished, this,
            [this, worker, gen, number, fingerprint, clean, mergeable,
             conflictFiles]() {
                m_pullConflictCheckInFlight = false;
                worker->deleteLater();
                const bool conflict = *mergeable && !*clean;
                if (gen == m_pullConflictGen) {
                    m_pullConflictCache.insert(
                        number, {fingerprint, conflict, *conflictFiles});
                    if (conflict)
                        m_pullConflictByNumber.insert(number, true);
                    else
                        m_pullConflictByNumber.remove(number);
                    setPullConflictBadge(number, conflict);
                    // A probe that could not run says nothing either way, so
                    // only real answers move the clean/conflicted history.
                    if (*mergeable && m_repoDetailIndex >= 0 &&
                        m_repoDetailIndex < m_repositories.size()) {
                        const RepositoryRecord &repo =
                            m_repositories.at(m_repoDetailIndex);
                        const QString slug =
                            repo.owner + QLatin1Char('/') + repo.name;
                        const QString probeKey =
                            slug + QLatin1Char('#') + QString::number(number);
                        const auto last = m_pullConflictLastProbe.constFind(probeKey);
                        const bool appeared =
                            conflict && last != m_pullConflictLastProbe.constEnd() &&
                            !last.value();
                        m_pullConflictLastProbe.insert(probeKey, conflict);
                        if (appeared) {
                            // Off this handler: posting the comment runs git,
                            // which pumps the event loop.
                            QTimer::singleShot(
                                0, this,
                                [this, slug, number, fingerprint,
                                 files = *conflictFiles] {
                                    if (m_repoDetailIndex < 0 ||
                                        m_repoDetailIndex >= m_repositories.size())
                                        return;
                                    const RepositoryRecord &now =
                                        m_repositories.at(m_repoDetailIndex);
                                    if (now.owner + QLatin1Char('/') + now.name != slug)
                                        return;
                                    postPullConflictNotice(number, fingerprint,
                                                           files, true);
                                });
                        }
                    }
                    if (number == m_currentPullNumber)
                        updatePullActionState();
                }
                if (!m_pendingPullConflictChecks.isEmpty()) {
                    const quint64 current = m_pullConflictGen;
                    QTimer::singleShot(0, this, [this, current] {
                        processPendingPullConflicts(current);
                    });
                }
            });
    worker->start();
}

void MainWindow::queuePullConflictCheck(int number, const QString &fingerprint)
{
    for (const QPair<int, QString> &p : std::as_const(m_pendingPullConflictChecks))
        if (p.first == number)
            return;
    // Whenever the pending list is non-empty a drain is already running or
    // scheduled (reloadPulls / processPendingPullConflicts keep that invariant),
    // so only kick off a new pass when we're appending to an idle queue.
    const bool wasIdle = m_pendingPullConflictChecks.isEmpty();
    m_pendingPullConflictChecks.append(qMakePair(number, fingerprint));
    if (wasIdle) {
        const quint64 gen = m_pullConflictGen;
        QTimer::singleShot(0, this, [this, gen] { processPendingPullConflicts(gen); });
    }
}

QString MainWindow::pullBehindCacheKey(const PullRequest &pull) const
{
    return m_pullConflictCacheBaseTip + QLatin1Char('|') +
           QString::number(pull.number) + QLatin1Char('|') + pull.base +
           QLatin1Char('|') + pull.head + QLatin1Char('|') +
           pullPatchFingerprint(pull.patch);
}

void MainWindow::queuePullBehindCheck(int number, const QString &cacheKey)
{
    if (cacheKey.isEmpty() || m_pullBehindCache.contains(cacheKey) ||
        m_pullBehindChecksPending.contains(cacheKey)) {
        return;
    }
    const PullStore store = pullStoreForCurrentRepo();
    if (!store.canWrite())
        return;

    m_pullBehindChecksPending.insert(cacheKey);
    runOffThread<PullBehindEntry>(
        [store, number] {
            PullBehindEntry result;
            result.valid = store.isBranchBehindBase(
                number, &result.behind, nullptr, &result.count);
            return result;
        },
        [this, number, cacheKey](PullBehindEntry result) {
            m_pullBehindChecksPending.remove(cacheKey);
            if (!result.valid)
                return;
            m_pullBehindCache.insert(cacheKey, result);

            const auto current = std::find_if(
                m_currentPulls.cbegin(), m_currentPulls.cend(),
                [number](const PullRequest &pull) {
                    return pull.number == number;
                });
            if (current == m_currentPulls.cend() ||
                m_currentPullNumber != number ||
                pullBehindCacheKey(*current) != cacheKey) {
                return;
            }

            if (m_pullMeta) {
                const QString marker =
                    QStringLiteral("<!--forkmesh-pull-behind-->");
                QString meta = m_pullMeta->text();
                const qsizetype markerAt = meta.indexOf(marker);
                if (markerAt >= 0)
                    meta.truncate(markerAt);
                if (result.behind) {
                    meta += marker + QString::fromUtf8(
                        " \xC2\xB7 <span style='color:#d29922'>%1 commit%2 "
                        "behind %3</span>")
                                         .arg(result.count)
                                         .arg(result.count == 1 ? QString()
                                                                : QStringLiteral("s"),
                                              current->base.toHtmlEscaped());
                }
                m_pullMeta->setText(meta);
            }
            updatePullActionState();
        });
}

void MainWindow::setPullConflictBadge(int number, bool conflict)
{
    if (!m_pullTable)
        return;
    // The probe drains one open pull at a time, so this runs once per row of
    // the list. Repaint the row it answered for instead of rebuilding the
    // table — that is the cost refreshPullList() exists to avoid.
    for (int row = 0; row < m_pullTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_pullTable->item(row, 0);
        if (!item || item->data(Qt::UserRole).toInt() != number)
            continue;
        if (item->data(kPullConflictRole).toBool() == conflict)
            return;
        item->setData(kPullConflictRole, conflict);
        item->setToolTip(item->data(kPullTipPrefixRole).toString() +
                         pullBranchStatusTip(item));
        return;
    }
}

void MainWindow::setPullViewedCount(int number, int viewed)
{
    if (!m_pullTable)
        return;
    for (int row = 0; row < m_pullTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_pullTable->item(row, 0);
        if (!item || item->data(Qt::UserRole).toInt() != number)
            continue;
        if (item->data(kPullViewedFilesRole).toInt() != viewed)
            item->setData(kPullViewedFilesRole, viewed);
        return;
    }
}

namespace {
const QString kPullConflictNoticeMarker =
    QStringLiteral("<!-- forkmesh:conflict-notice key=");
}

QString MainWindow::pullConflictNoticeKey(const QString &patch)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(patch.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .left(16));
}

bool MainWindow::pullHasConflictNotice(const PullRequest &pull)
{
    const QString marker = kPullConflictNoticeMarker +
                           pullConflictNoticeKey(pull.patch) +
                           QStringLiteral(" -->");
    for (const PullEvent &ev : pull.events)
        if (ev.type == QLatin1String("comment") && ev.body.contains(marker))
            return true;
    return false;
}

QString MainWindow::pullConflictNoticeBody(const PullRequest &pull,
                                           const QStringList &conflictFiles)
{
    const QString base = pull.base.isEmpty() ? QStringLiteral("the base branch")
                                             : QStringLiteral("`%1`").arg(pull.base);
    const QString head = pull.head.isEmpty() ? QStringLiteral("this branch")
                                             : QStringLiteral("`%1`").arg(pull.head);
    QString text = pull.authorName.isEmpty()
                       ? QStringLiteral("This pull request")
                       : QStringLiteral("@%1 this pull request").arg(pull.authorName);
    if (conflictFiles.isEmpty()) {
        text += QStringLiteral(" no longer merges cleanly into %1.").arg(base);
    } else {
        static constexpr int kListed = 20;
        text += QStringLiteral(" now has merge conflicts with %1 in %2 file%3:\n\n")
                    .arg(base)
                    .arg(conflictFiles.size())
                    .arg(conflictFiles.size() == 1 ? QString() : QStringLiteral("s"));
        for (int i = 0; i < conflictFiles.size() && i < kListed; ++i)
            text += QStringLiteral("- `%1`\n").arg(conflictFiles.at(i));
        if (conflictFiles.size() > kListed)
            text += QStringLiteral("- and %1 more\n").arg(conflictFiles.size() - kListed);
    }
    text += QStringLiteral("\n\nPlease merge %1 into %2, resolve the conflicts and "
                           "push, so this can be merged.\n\n")
                .arg(base, head);
    return text + kPullConflictNoticeMarker + pullConflictNoticeKey(pull.patch) +
           QStringLiteral(" -->");
}

QString MainWindow::pullConflictNoticeSessionKey(const PullRequest &pull) const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    return repo.owner + QLatin1Char('/') + repo.name + QLatin1Char('#') +
           QString::number(pull.number) + QLatin1Char('@') +
           pullConflictNoticeKey(pull.patch);
}

bool MainWindow::postPullConflictNotice(int number, const QString &fingerprint,
                                        const QStringList &conflictFiles,
                                        bool automatic)
{
    // By value: addComment runs git, which pumps the event loop and can
    // rebuild m_currentPulls under a held reference.
    PullRequest pull;
    bool found = false;
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        if (pr.number == number) {
            pull = pr;
            found = true;
            break;
        }
    }
    if (!found || pull.status != QLatin1String("open") || pull.archivedOnCodeBranch)
        return false;
    if (!fingerprint.isEmpty() && pullPatchFingerprint(pull.patch) != fingerprint)
        return false; // the patch moved on; the probe will answer for the new one
    const QString sessionKey = pullConflictNoticeSessionKey(pull);
    if (sessionKey.isEmpty() || pullHasConflictNotice(pull) ||
        m_pullConflictNoticesPosted.contains(sessionKey))
        return false;
    // Nobody needs a notice about their own pull request.
    if (!pull.author.isEmpty() && pull.author == m_profileIdentity.publicKey())
        return false;
    PullStore store = pullStoreForCurrentRepo();
    if (!store.canWrite())
        return false;
    // Claimed before the post so a probe or click serviced during its git pump
    // cannot post a second copy.
    m_pullConflictNoticesPosted.insert(sessionKey);
    QString error;
    if (!store.addComment(number, pullConflictNoticeBody(pull, conflictFiles),
                          &error)) {
        m_pullConflictNoticesPosted.remove(sessionKey);
        logSystem(QStringLiteral("Pull request #%1: could not message the author "
                                 "about merge conflicts: %2")
                      .arg(number)
                      .arg(error));
        if (!automatic)
            flashMessage(error.isEmpty() ? QStringLiteral("Could not message the author.")
                                         : error,
                         true);
        return false;
    }
    logSystem(QStringLiteral("Pull request #%1: asked %2 to resolve merge conflicts")
                  .arg(number)
                  .arg(pull.authorName.isEmpty() ? QStringLiteral("the author")
                                                 : pull.authorName));
    reloadPulls();
    if (!automatic) {
        flashMessage(QStringLiteral("Asked the author to resolve the conflicts."));
        if (m_currentPullNumber == number)
            showPull(number);
    } else if (m_currentPullNumber == number) {
        updatePullActionState();
    }
    return true;
}

void MainWindow::refreshPullBranchState()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString workTree = writableRecordFor(repo).localPath;
    const QString repoKey = repo.owner + QLatin1Char('/') + repo.name +
                            QLatin1Char('|') + workTree;
    if (repoKey != m_pullBranchStateRepo) {
        m_pullBranchStateRepo = repoKey;
        m_pullBranchState.clear();
    }
    // number, base, head for every pull whose branch state the list can show.
    // Closed and merged pulls are left out for the same reason the conflict
    // probe skips them: their answer no longer changes what anyone can do, and
    // measuring them would put a `git rev-list` behind every archived row.
    QList<PullHeadProbe> heads;
    for (const PullRequest &pull : std::as_const(m_currentPulls)) {
        if (pull.status != QLatin1String("open") || pull.archivedOnCodeBranch ||
            pull.base.isEmpty() || pull.head.isEmpty() ||
            pull.head == pull.base)
            continue;
        heads.append({pull.number, pull.base, pull.head});
    }
    const quint64 gen = ++m_pullBranchStateGen;
    if (workTree.isEmpty() || heads.isEmpty()) {
        if (!m_pullBranchState.isEmpty()) {
            m_pullBranchState.clear();
            if (m_pullTable)
                refreshPullList();
        }
        return;
    }
    runOffThread<QHash<int, PullBranchState>>(
        [workTree, heads] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("pulls"),
                QStringLiteral("read pull branch state"),
                forkmesh::ActionTelemetry::Execution::Worker);
            QHash<int, PullBranchState> map;
            // One listing for the whole repository — served from the shared
            // memo — rather than a `git worktree list` per pull.
            QHash<QString, QString> worktrees;
            QByteArray listing;
            if (cachedWorktreeListPorcelain(workTree, &listing)) {
                QString path;
                for (const QString &raw : QString::fromUtf8(listing).split(
                         QLatin1Char('\n'))) {
                    const QString line = raw.trimmed();
                    if (line.isEmpty())
                        path.clear();
                    else if (line.startsWith(QLatin1String("worktree ")))
                        path = line.mid(9).trimmed();
                    else if (line.startsWith(QLatin1String("branch refs/heads/")) &&
                             !path.isEmpty())
                        worktrees.insert(line.mid(18).trimmed(), path);
                }
            }
            QHash<QString, int> dirtyByPath; // several pulls can share a checkout
            for (const PullHeadProbe &entry : heads) {
                const QString &base = entry.base;
                const QString &head = entry.head;
                PullBranchState state;
                QByteArray counts;
                // Deliberately still a git spawn. repoHasLocalBranch() answers
                // this from the ref files and would save ~84 spawns per ten
                // minutes, but it finishes this worker enough sooner that the
                // completion cascade re-enters refreshSourceControlOutgoing()
                // while the Source Control panel is still hidden, and that
                // early return clears m_scmOutgoingBlocking — leaving the Sync
                // Changes row hidden with its "N↑" text already painted. It
                // reproduced in 2 of 3 offscreen suite runs. Fix that ordering
                // bug in refreshSourceControlOutgoing() before speeding this up.
                if (runGitCapture(workTree,
                                  {QStringLiteral("rev-parse"),
                                   QStringLiteral("--verify"),
                                   QStringLiteral("--quiet"),
                                   QStringLiteral("refs/heads/%1").arg(head)},
                                  nullptr, nullptr) &&
                    runGitCapture(workTree,
                                  {QStringLiteral("rev-list"),
                                   QStringLiteral("--left-right"),
                                   QStringLiteral("--count"),
                                   base + QStringLiteral("...") + head},
                                  &counts, nullptr)) {
                    static const QRegularExpression separator(
                        QStringLiteral("\\s+"));
                    const QStringList parts =
                        QString::fromUtf8(counts).trimmed().split(separator);
                    if (parts.size() >= 2) {
                        state.behind = parts.at(0).toInt();
                        state.ahead = parts.at(1).toInt();
                        state.counted = true;
                    }
                }
                const QString path = worktrees.value(head);
                if (!path.isEmpty() && QDir(path).exists()) {
                    state.worktree = path;
                    const auto known = dirtyByPath.constFind(path);
                    if (known != dirtyByPath.constEnd()) {
                        state.dirty = *known;
                    } else {
                        QByteArray status;
                        if (runGitCapture(path,
                                          {QStringLiteral("status"),
                                           QStringLiteral("--porcelain")},
                                          &status, nullptr)) {
                            const QString lines =
                                QString::fromUtf8(status).trimmed();
                            state.dirty = lines.isEmpty()
                                              ? 0
                                              : lines.count(QLatin1Char('\n')) + 1;
                        }
                        dirtyByPath.insert(path, state.dirty);
                    }
                }
                if (state.counted || !state.worktree.isEmpty())
                    map.insert(entry.number, state);
            }
            return map;
        },
        [this, gen](QHash<int, PullBranchState> map) {
            if (gen != m_pullBranchStateGen)
                return; // a newer load superseded this pass
            if (map == m_pullBranchState)
                return;
            m_pullBranchState = std::move(map);
            if (m_pullTable)
                refreshPullList();
        });
}

void MainWindow::refreshPullList()
{
    if (!m_pullTable)
        return;
    const QString search = m_pullSearch ? m_pullSearch->text().trimmed() : QString();
    if (m_pullOpenTarget > 0 &&
        QDateTime::currentMSecsSinceEpoch() > m_pullOpenTargetDeadlineMs)
        m_pullOpenTarget = -1;
    bool revealTarget = false;
    for (const PullRequest &pull : std::as_const(m_currentPulls)) {
        if (m_pullOpenTarget <= 0 || pull.number != m_pullOpenTarget)
            continue;
        const QString status = pullDisplayStatus(pull);
        QPushButton *filter = status == QLatin1String("open")
                                  ? m_pullShowOpenButton
                              : status == QLatin1String("merged")
                                  ? m_pullShowMergedButton
                                  : m_pullShowClosedButton;
        if (filter && !filter->isChecked() &&
            !(m_pullShowAllButton && m_pullShowAllButton->isChecked())) {
            const QSignalBlocker block(filter);
            filter->setChecked(true);
        }
        revealTarget = true;
        break;
    }
    const int keep = revealTarget ? m_pullOpenTarget : m_currentPullNumber;
    const bool awaitingTarget = !revealTarget && m_pullOpenTarget > 0;
    if (revealTarget)
        m_pullOpenTarget = -1;
    const bool showAll = m_pullShowAllButton && m_pullShowAllButton->isChecked();
    const bool showClosed =
        m_pullShowClosedButton && m_pullShowClosedButton->isChecked();
    const bool showMerged =
        m_pullShowMergedButton && m_pullShowMergedButton->isChecked();
    if (m_pullShowAllButton) {
        const int count = qMax(m_pullRegisteredCount,
                               int(m_currentPulls.size()));
        setPullActionBadge(m_pullShowAllButton, count);
        m_pullShowAllButton->setToolTip(
            showAll
                ? QStringLiteral("Showing all %1 registered pull request%2, including "
                                 "closed and merged ones. Click to return to active "
                                 "work.")
                      .arg(count)
                      .arg(count == 1 ? QString() : QStringLiteral("s"))
                : QStringLiteral("Show all %1 registered pull request%2, including "
                                 "closed and merged ones, so they can be reviewed or "
                                 "deleted.")
                      .arg(count)
                      .arg(count == 1 ? QString() : QStringLiteral("s")));
    }
    int openCount = 0;
    int closedCount = 0;
    int mergedCount = 0;
    for (const PullRequest &pull : std::as_const(m_currentPulls)) {
        const QString status = pullDisplayStatus(pull);
        const bool open = status == QLatin1String("open");
        const bool merged = status == QLatin1String("merged");
        if (open)
            ++openCount;
        else if (merged)
            ++mergedCount;
        else
            ++closedCount;
    }
    if (m_pullRegisteredCount >= 0)
        closedCount = qMax(
            closedCount, m_pullRegisteredCount - openCount - mergedCount);
    if (m_pullShowOpenButton)
        setPullActionBadge(m_pullShowOpenButton, openCount);
    if (m_pullShowClosedButton)
        setPullActionBadge(m_pullShowClosedButton, closedCount);
    if (m_pullShowMergedButton)
        setPullActionBadge(m_pullShowMergedButton, mergedCount);
    // Rebuilding the table clears its selection. Without blocking that signal,
    // selecting `keep` below calls showPull() again even though the detail did
    // not change. A background load and its provenance callback can otherwise
    // rebuild the same large pull two or three times in quick succession.
    QSignalBlocker selectionBlock(m_pullTable);
    TableRepaintGuard repaintGuard(m_pullTable);
    m_pullTable->setSortingEnabled(false);
    QList<const PullRequest *> visiblePulls;
    visiblePulls.reserve(m_currentPulls.size());
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        const QString status = pullDisplayStatus(pr);
        const bool isOpen = status == QLatin1String("open");
        const bool isMerged = status == QLatin1String("merged");
        const bool isClosed = !isOpen && !isMerged;
        if (!showAll && ((showClosed && !isClosed) ||
                         (showMerged && !isMerged) ||
                         (!showClosed && !showMerged && !isOpen)))
            continue;
        if (!search.isEmpty()) {
            const QString hay = QStringLiteral("#%1 %2 %3 %4 %5")
                                    .arg(pr.number)
                                    .arg(pr.title, pr.base, pr.head, pr.authorName);
            if (!hay.contains(search, Qt::CaseInsensitive))
                continue;
        }
        visiblePulls.append(&pr);
    }
    m_pullTable->setRowCount(visiblePulls.size());
    QString repoOwner;
    QString repoName;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        repoOwner = m_repositories.at(m_repoDetailIndex).owner;
        repoName = m_repositories.at(m_repoDetailIndex).name;
    }
    for (int row = 0; row < visiblePulls.size(); ++row) {
        const PullRequest &pr = *visiblePulls.at(row);
        auto *num = new SortTableWidgetItem;
        num->setData(Qt::DisplayRole, QStringLiteral("#%1").arg(pr.number));
        num->setData(Qt::UserRole, pr.number);
        num->setData(kTableSortRole, pr.number);
        num->setData(kPullCreatedAtRole, pr.ts);
        qint64 updatedAt = pr.ts;
        for (const PullEvent &ev : pr.events)
            updatedAt = qMax(updatedAt, ev.ts);
        num->setData(kPullModifiedAtRole, updatedAt);
        num->setData(kPullFilesChangedRole, pr.filesChanged);
        num->setData(kPullViewedFilesRole,
                     loadDiffViewed(QStringLiteral("pull/") +
                                    QString::number(pr.number))
                         .size());
        num->setData(kPullAdditionsRole, pr.additions);
        num->setData(kPullDeletionsRole, pr.deletions);
        // Branch state, the way the agents list shows it. Only open pulls are
        // measured, so a merged or archived row carries the neutral defaults
        // and paints exactly as it did before.
        const PullBranchState branchState = m_pullBranchState.value(pr.number);
        num->setData(kPullBaseBranchRole, pr.base);
        num->setData(kPullConflictRole, m_pullConflictByNumber.value(pr.number));
        num->setData(kPullAheadRole, branchState.counted ? branchState.ahead : -1);
        num->setData(kPullBehindRole, branchState.counted ? branchState.behind : -1);
        num->setData(kPullWorktreeRole, branchState.worktree);
        num->setData(kPullDirtyRole, branchState.dirty);
        const QString displayStatus = pullDisplayStatus(pr);
        const bool checksRunning = pullHasRunningChecks(
            pr, m_actionRuns, repoOwner, repoName);
        // A spinner glyph gives an in-list progress signal without
        // displacing the PR state icon into another column. It changes only
        // while an executor is actively running, not while a run is queued or
        // waiting for approval.
        num->setIcon(themedOcticon(
            checksRunning ? QStringLiteral("sync")
                          : pullListStatusIcon(displayStatus),
            checksRunning ? QColor(QStringLiteral("#58a6ff"))
                          : pullListStatusColor(displayStatus),
            14));
        const QString createdAt =
            pr.ts > 0 ? QDateTime::fromMSecsSinceEpoch(pr.ts)
                           .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                      : QStringLiteral("unknown");
        const QString modifiedAt =
            updatedAt > 0 ? QDateTime::fromMSecsSinceEpoch(updatedAt)
                               .toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                          : QStringLiteral("unknown");
        num->setData(
            kPullTipPrefixRole,
            QStringLiteral("%1 pull request #%2%3\nCreated: %4 (%5)\nModified: %6 (%7)"
                           "\nBranch: %8 ← %9\n%10 file%11 changed\n+%12 −%13")
                .arg(pullListStatusLabel(displayStatus))
                .arg(pr.number)
                .arg(checksRunning ? QStringLiteral(" · checks running")
                                   : QString())
                .arg(createdAt, formatIssueRelativeTime(pr.ts))
                .arg(modifiedAt, formatIssueRelativeTime(updatedAt))
                .arg(pr.base, pr.head)
                .arg(pr.filesChanged)
                .arg(pr.filesChanged == 1 ? QString() : QStringLiteral("s"))
                .arg(formatCount(pr.additions), formatCount(pr.deletions)));
        num->setToolTip(num->data(kPullTipPrefixRole).toString() +
                        pullBranchStatusTip(num));
        m_pullTable->setItem(row, 0, num);
        auto *titleItem = new QTableWidgetItem(pr.title);
        const QString author =
            pr.authorName.trimmed().isEmpty()
                ? (pr.author.isEmpty() ? QString::fromUtf8("\xE2\x80\x94")
                                       : pr.author.left(8))
                : pr.authorName.trimmed();
        const QPixmap knownAvatar = m_avatars.value(pr.author);
        const QString avatarSeed = pr.author.isEmpty() ? author : pr.author;
        const QPixmap authorAvatar =
            knownAvatar.isNull()
                ? roundedAvatar(forkMeshAvatarPng(avatarSeed.toLower()), 18, 0.5)
                : roundedRectPixmap(knownAvatar, 18, 9);
        if (!authorAvatar.isNull())
            titleItem->setIcon(QIcon(authorAvatar));
        QString titleTip = QStringLiteral("Opened by %1").arg(author);
        if (pr.archivedOnCodeBranch) {
            titleTip += QStringLiteral(
                "\nArchived on the repository branch — use Restore to manage it");
        }
        if (!pr.author.isEmpty() && pr.author != author)
            titleTip += QStringLiteral("\n%1").arg(pr.author);
        const AgentSession *agent = agentSessionForPull(pr.number, pr.head);
        if (agent) {
            titleTip += QStringLiteral("\nAgent attached (%1)").arg(
                agent->prNumber == pr.number
                    ? QStringLiteral("this pull request")
                    : QStringLiteral("branch %1").arg(pr.head));
        } else if (const PullAgentProvenance prov =
                       m_pullProvenance.value(pr.number);
                   prov.isAgent) {
            // No local session (e.g. an agent pull from another node), but the
            // signed commit trailer still attributes authorship.
            titleTip += QStringLiteral("\nAgent-authored — %1")
                            .arg(agentProviderName(prov.tool));
        }
        titleItem->setToolTip(titleTip);
        m_pullTable->setItem(row, 1, titleItem);
    }
    m_pullTable->setSortingEnabled(true);
    m_pullListRenderedDigest = m_pullSnapshotDigest;
    setPullListLoading(m_pullBackgroundLoadInFlight);
    int selRow = -1;
    for (int r = 0; r < m_pullTable->rowCount(); ++r)
        if (m_pullTable->item(r, 0)->data(Qt::UserRole).toInt() == keep) {
            selRow = r;
            break;
        }
    if (selRow < 0 && !awaitingTarget && m_pullTable->rowCount() > 0)
        selRow = 0;
    int selectedNumber = -1;
    if (selRow >= 0) {
        m_pullTable->selectRow(selRow);
        if (QTableWidgetItem *item = m_pullTable->item(selRow, 0)) {
            selectedNumber = item->data(Qt::UserRole).toInt();
            if (revealTarget)
                m_pullTable->scrollToItem(item,
                                          QAbstractItemView::PositionAtTop);
        }
    }
    selectionBlock.unblock();
    if (selectedNumber >= 0) {
        if (selectedNumber != keep || m_pullDetailRenderedDigest.isEmpty() ||
            (revealTarget && selectedNumber != m_currentPullNumber))
            showPull(selectedNumber);
    } else {
        m_currentPullNumber = -1;
        showPull(-1);
    }
}

void MainWindow::showPull(int number)
{
    if (number != m_currentPullNumber) {
        m_pullInlineSelecting = false;
        m_pullInlinePath.clear();
        m_pullInlineSide.clear();
        m_pullInlineStartLine = 0;
        m_pullInlineEndLine = 0;
        if (m_pullInlineComposerFrame)
            m_pullInlineComposerFrame->hide();
        if (m_pullInlineCommentEdit)
            m_pullInlineCommentEdit->clear();
        if (m_pullDiff)
            m_pullDiff->setExtraSelections({});
        if (m_branchDiffView)
            m_branchDiffView->setExtraSelections({});
        m_pullInlineView = nullptr;
        m_pullInlineBranch.clear();
        m_pullOpenLineNotes.clear();
    }
    PullRequest foundPull;
    bool havePull = false;
    for (const PullRequest &pr : m_currentPulls)
        if (pr.number == number) {
            foundPull = pr;
            havePull = true;
        }
    const PullRequest *found = havePull ? &foundPull : nullptr;
    m_currentPullNumber = found ? number : -1;
    updatePullInlineReviewBatchBar();
    m_pullDetailRenderedDigest =
        found ? pullDetailDigest(*found) : QString();
    m_pullFiles->clear();
    m_pullFileDiffs.clear();
    togglePullDiffSearch(false); // opening a different PR clears any find-in-diff state
    m_pullFileAuthorship.clear();
    if (m_pullFileAuthorFilter)
        m_pullFileAuthorFilter->hide();

    if (!found) {
        m_pullTitle->setText("Select a pull request");
        m_pullTitle->setToolTip(QString());
        if (m_pullStatusBadge)
            m_pullStatusBadge->hide();
        m_pullMeta->clear();
        m_pullDiff->clear();
        m_pullDiffRenderKey.clear(); // widget no longer shows a rendered diff
        m_pullDiffSourceKey.clear();
        if (m_pullCommitsList)
            m_pullCommitsList->clear();
        renderPullThread(PullRequest());
        renderPullChecks(PullRequest());
        renderPullChecksSummary(PullRequest());
        updatePullSubTabCounts(PullRequest());
        if (m_pullBadgeWidget)
            m_pullBadgeWidget->clearPull();
        if (m_pullComposer)
            m_pullComposer->setEnabled(false);
        for (QPushButton *b : {m_pullCommentButton, m_pullApproveButton,
                               m_pullRequestChangesButton})
            if (b)
                b->setEnabled(false);
        updatePullActionState();
        updateQuickAddEnterTarget();
        return;
    }

    if (m_pullDetail)
        m_pullDetail->show();
    if (m_pullComposer) {
        m_pullComposer->setEnabled(!found->archivedOnCodeBranch);
        m_pullComposer->setMentionCandidates(mentionCandidateNames());
    }
    for (QPushButton *b : {m_pullCommentButton, m_pullApproveButton,
                           m_pullRequestChangesButton})
        if (b)
            b->setEnabled(!found->archivedOnCodeBranch);
    const QString pullTitle =
        QStringLiteral("#%1  %2").arg(found->number).arg(found->title);
    m_pullTitle->setText(pullTitle);
    m_pullTitle->setToolTip(pullTitle);
    const QString displayStatus = pullDisplayStatus(*found);
    if (m_pullStatusBadge) {
        QString foreground;
        QString background;
        QString border;
        if (displayStatus == QLatin1String("merged")) {
            foreground = Theme::nextOrHex(Theme::Role::AccentFg,
                                          QStringLiteral("#d2a8ff"));
            background = Theme::nextOrHex(Theme::Role::AccentSubtle,
                                          QStringLiteral("#362160"));
            border = Theme::nextOrHex(Theme::Role::Accent,
                                      QStringLiteral("#8957e5"));
        } else if (displayStatus == QLatin1String("closed")) {
            foreground = QStringLiteral("#ff7b72");
            background = QStringLiteral("#4a1f24");
            border = QStringLiteral("#da3633");
        } else {
            foreground = QStringLiteral("#56d364");
            background = QStringLiteral("#173b24");
            border = QStringLiteral("#238636");
        }
        m_pullStatusBadge->setText(
            pullListStatusLabel(displayStatus).toUpper());
        m_pullStatusBadge->setStyleSheet(Theme::remapSheet(
            QStringLiteral(
                "#pullDetailStatusBadge { color:%1; background:%2; border:1px "
                "solid %3; border-radius:10px; font-weight:700; padding:3px "
                "10px; }")
                .arg(foreground, background, border)));
        m_pullStatusBadge->show();
    }
    m_pullMeta->setText(
        QString::fromUtf8("<b>%1</b> \xE2\x86\x90 <b>%2</b> \xC2\xB7 %3 \xC2\xB7 %4 files "
                       "<span style='color:#3fb950'>+%5</span> "
                       "<span style='color:#f85149'>-%6</span> \xC2\xB7 by %7")
            .arg(branchLinkHtml(found->base), branchLinkHtml(found->head),
                 pullListStatusLabel(displayStatus), formatCount(found->filesChanged),
                 formatCount(found->additions), formatCount(found->deletions),
                 (found->authorName.isEmpty() ? found->author.left(10)
                                              : found->authorName)
                     .toHtmlEscaped()));
    if (found->archivedOnCodeBranch) {
        m_pullMeta->setText(
            m_pullMeta->text() +
            QString::fromUtf8(
                " \xC2\xB7 <span style='color:#d29922'>Archived record "
                "\xE2\x80\x94 restore it to manage this pull request</span>"));
    }
    const QString review = found->reviewSummary();
    if (review == QLatin1String("approved"))
        m_pullMeta->setText(m_pullMeta->text() +
                            QString::fromUtf8(" \xC2\xB7 <span style='color:#3fb950'>"
                                           "\xE2\x9C\x93 Approved</span>"));
    else if (review == QLatin1String("changes_requested"))
        m_pullMeta->setText(m_pullMeta->text() +
                            QString::fromUtf8(" \xC2\xB7 <span style='color:#f85149'>"
                                           "\xE2\x9A\xA0 Changes requested</span>"));
    // Owner-authored reward estimate. The payout queue and the public ledger
    // both read it, so the pull it belongs to should say what it is and where
    // it stands rather than leaving that only in a maintainer's comment.
    const QString rewardState = PullReward::statusText(*found);
    if (!rewardState.isEmpty()) {
        const QString status = found->rewardStatus.trimmed();
        const QString colour =
            status == QLatin1String("paid")        ? QStringLiteral("#3fb950")
            : status == QLatin1String("cancelled") ? QStringLiteral("#8b949e")
            : (status == QLatin1String("ready") ||
               status == QLatin1String("submitted"))
                ? QStringLiteral("#58a6ff")
                : QStringLiteral("#d29922");
        const QString amount =
            PullReward::amountText(found->rewardEstimateLamports);
        QString reward = amount.isEmpty() ? QStringLiteral("reward")
                                          : amount + QStringLiteral(" reward");
        const QString recipient = found->rewardRecipient.trimmed();
        if (!recipient.isEmpty() &&
            QString::compare(recipient, found->authorName.trimmed(),
                             Qt::CaseInsensitive) != 0)
            reward += QStringLiteral(" to ") + recipient.toHtmlEscaped();
        m_pullMeta->setText(
            m_pullMeta->text() +
            QString::fromUtf8(" \xC2\xB7 <span style='color:%1'>%2 \xC2\xB7 %3"
                              "</span>")
                .arg(colour, reward, rewardState.toHtmlEscaped()));
    }
    if (const AgentSession *agent =
            agentSessionForPull(found->number, found->head)) {
        const QString href = kAgentLinkScheme + QString::number(agent->id);
        const QString link =
            QStringLiteral("<a href=\"%1\" style=\"color:#58a6ff;"
                           "text-decoration:none\">agent</a>")
                .arg(href);
        m_pullMeta->setText(
            m_pullMeta->text() +
            QString::fromUtf8(" \xC2\xB7 %1 cost ~%2")
                .arg(link, agentCostText(agent->costUsd)));
    }

    const PullReviewSnapshot reviewSnapshot = buildPullReviewSnapshot(*found);
    QString currentFile;
    qsizetype currentFileStart = -1;
    const auto flush = [&](qsizetype end) {
        if (!currentFile.isEmpty() && currentFileStart >= 0) {
            m_pullFileDiffs.insert(
                currentFile,
                found->patch.mid(currentFileStart, end - currentFileStart));
        }
    };
    // Preserve slices of the original patch instead of split() allocating one
    // QString for every line and then join() copying every file again.  Large
    // generated pull requests made those two full-patch passes visible in the
    // watchdog samples.
    const QStringView patch(found->patch);
    qsizetype lineStart = 0;
    while (lineStart <= patch.size()) {
        const qsizetype newline = patch.indexOf(QLatin1Char('\n'), lineStart);
        const qsizetype lineEnd = newline < 0 ? patch.size() : newline;
        const QStringView line = patch.mid(lineStart, lineEnd - lineStart);
        if (line.startsWith(QLatin1String("diff --git "))) {
            flush(lineStart);
            const qsizetype bPath = line.indexOf(QLatin1String(" b/"));
            currentFile = bPath < 0 ? QString() : line.mid(bPath + 3).toString();
            currentFileStart = currentFile.isEmpty() ? -1 : lineStart;
        }
        if (newline < 0)
            break;
        lineStart = newline + 1;
    }
    flush(patch.size());

    // Per-file authorship from the signed commit trailers, so the file list can be
    // filtered by whether an agent touched each file.
    m_pullFileAuthorship = pullFileAuthorship(*found);

    for (auto it = m_pullFileDiffs.constBegin(); it != m_pullFileDiffs.constEnd(); ++it) {
        const QString name = it.key().section('/', -1);
        QString label = it.key();
        const auto summaryIt = reviewSnapshot.files.constFind(it.key());
        if (summaryIt != reviewSnapshot.files.constEnd() &&
            summaryIt->unresolvedThreads > 0) {
            label += QStringLiteral("  (%1 unresolved%2)")
                         .arg(summaryIt->unresolvedThreads)
                         .arg(summaryIt->outdatedThreads > 0
                                  ? QStringLiteral(", %1 outdated")
                                        .arg(summaryIt->outdatedThreads)
                                  : QString());
        }
        auto *item = new QListWidgetItem(iconForFile(name), label);
        item->setData(Qt::UserRole, it.key());
        item->setData(kPullFileAgentRole,
                      m_pullFileAuthorship.value(it.key(), false));
        if (summaryIt != reviewSnapshot.files.constEnd()) {
            item->setToolTip(QStringLiteral("%1 thread(s), %2 unresolved, %3 resolved, %4 suggestion(s), %5 outdated")
                                 .arg(summaryIt->totalThreads)
                                 .arg(summaryIt->unresolvedThreads)
                                 .arg(summaryIt->resolvedThreads)
                                 .arg(summaryIt->suggestions)
                                 .arg(summaryIt->outdatedThreads));
        }
        m_pullFiles->addItem(item);
    }
    m_pullFiles->sortItems();
    if (m_pullBadgeWidget) {
        QList<PullBadgeWidget::FileEntry> badgeFiles;
        badgeFiles.reserve(m_pullFileDiffs.size());
        for (auto it = m_pullFileDiffs.constBegin();
             it != m_pullFileDiffs.constEnd(); ++it) {
            PullBadgeWidget::FileEntry entry;
            entry.path = it.key();
            const QStringView fileDiff(it.value());
            qsizetype lineStart = 0;
            while (lineStart <= fileDiff.size()) {
                const qsizetype newline =
                    fileDiff.indexOf(QLatin1Char('\n'), lineStart);
                const qsizetype lineEnd =
                    newline < 0 ? fileDiff.size() : newline;
                const QStringView line =
                    fileDiff.mid(lineStart, lineEnd - lineStart);
                const bool fileHeader =
                    line.startsWith(QLatin1String("+++ ")) ||
                    line.startsWith(QLatin1String("--- "));
                if (!fileHeader) {
                    if (line.startsWith(QLatin1Char('+')))
                        ++entry.adds;
                    else if (line.startsWith(QLatin1Char('-')))
                        ++entry.dels;
                }
                if (newline < 0)
                    break;
                lineStart = newline + 1;
            }
            entry.icon = iconForFile(it.key().section('/', -1));
            badgeFiles << entry;
        }
        m_pullBadgeWidget->setPull(
            found->title, found->number,
            found->authorName.isEmpty() ? found->author.left(10)
                                        : found->authorName,
            found->additions, found->deletions, badgeFiles);
    }
    if (m_pullFileAuthorFilter) {
        bool anyAgent = false, anyHuman = false;
        for (auto it = m_pullFileAuthorship.constBegin();
             it != m_pullFileAuthorship.constEnd(); ++it)
            (it.value() ? anyAgent : anyHuman) = true;
        const QSignalBlocker block(m_pullFileAuthorFilter);
        m_pullFileAuthorFilter->setCurrentIndex(0);
        m_pullFileAuthorFilter->setVisible(anyAgent && anyHuman);
    }
    fitFileListToWidestEntry(m_pullFiles); // open wide enough for the longest path
    if (m_pullFiles->count() > 0) {
        renderPullDiff();
        m_pullSuppressFileScroll = true;
        m_pullFiles->setCurrentRow(0);
        m_pullSuppressFileScroll = false;
    } else {
        setDiffHtml(m_pullDiff, QStringLiteral("(no changes)"));
        m_pullDiffRenderKey.clear(); // widget no longer shows a rendered diff
        m_pullDiffSourceKey.clear();
        m_pullFileAnchors.clear();
        m_pullFileOrder.clear();
        m_pullStickyLabelHtml.clear();
        m_pullFileTops.clear();
        m_pullStickyFile.clear();
        if (m_pullStickyHeader)
            m_pullStickyHeader->hide();
        updatePullViewedLabel();
    }
    renderPullCommits(*found);
    renderPullThread(*found);
    renderPullChecks(*found);
    renderPullChecksSummary(*found);
    updatePullSubTabCounts(*found);
    updatePullActionState();
    updateQuickAddEnterTarget();
    scheduleNavRecord();
}

void MainWindow::applyPullFileAuthorFilter()
{
    if (!m_pullFiles || !m_pullFileAuthorFilter)
        return;
    const int mode = m_pullFileAuthorFilter->currentIndex();
    for (int row = 0; row < m_pullFiles->count(); ++row) {
        QListWidgetItem *item = m_pullFiles->item(row);
        const bool agent = item->data(kPullFileAgentRole).toBool();
        const bool show = mode == 0 || (mode == 1 && agent) || (mode == 2 && !agent);
        item->setHidden(!show);
    }
    if (QListWidgetItem *cur = m_pullFiles->currentItem();
        !cur || cur->isHidden()) {
        for (int row = 0; row < m_pullFiles->count(); ++row)
            if (!m_pullFiles->item(row)->isHidden()) {
                m_pullFiles->setCurrentRow(row);
                break;
            }
    }
}

void MainWindow::switchToPullTab(int pullNumber)
{
    if (m_repoDetailTabs && m_repoDetailTabs->button(4))
        m_repoDetailTabs->button(4)->setChecked(true);
    if (m_repoDetailStack)
        m_repoDetailStack->setCurrentIndex(4);
    m_currentPullNumber = pullNumber;
    reloadPulls();
    if (!m_pullTable)
        return;
    for (int row = 0; row < m_pullTable->rowCount(); ++row) {
        QTableWidgetItem *number = m_pullTable->item(row, 0);
        if (number && number->data(Qt::UserRole).toInt() == pullNumber) {
            m_pullTable->selectRow(row);
            showPull(pullNumber);
            return;
        }
    }
    showPull(pullNumber);
}

void MainWindow::openPullFromNotification(int pullNumber)
{
    if (pullNumber <= 0)
        return;
    m_pullOpenTarget = pullNumber;
    m_pullOpenTargetDeadlineMs = QDateTime::currentMSecsSinceEpoch() + 120000;
    switchToPullTab(pullNumber);
    if (m_pullOpenTarget != pullNumber)
        return;
    flashMessage(QStringLiteral("Fetching pull request #%1…").arg(pullNumber));
    refreshPullsFromMesh();
}

void MainWindow::openPullDiffInGitView(int pullNumber)
{
    ensureRepoDetailTabBuilt(4);
    m_currentPullNumber = pullNumber;
    reloadPulls();
    bool rowSelected = false;
    if (m_pullTable) {
        for (int row = 0; row < m_pullTable->rowCount(); ++row) {
            QTableWidgetItem *number = m_pullTable->item(row, 0);
            if (number && number->data(Qt::UserRole).toInt() == pullNumber) {
                m_pullTable->selectRow(row); // fires showPull(pullNumber)
                rowSelected = true;
                break;
            }
        }
    }
    if (!rowSelected)
        showPull(pullNumber);
    if (m_pullTabConversation)
        m_pullTabConversation->setChecked(true);
    if (m_pullSubStack)
        m_pullSubStack->setCurrentIndex(0);

    PullRequest pr;
    for (const PullRequest &p : std::as_const(m_currentPulls))
        if (p.number == pullNumber)
            pr = p;
    if (pr.number != pullNumber) {
        switchToPullTab(pullNumber);
        return;
    }

    m_branchDiffPullNumber = pullNumber;
    showOverviewCommits();
    const QString dir = repoGitDir();
    const QString reviewHead = resolvablePullHead(pr);
    const bool liveBranch = !reviewHead.isEmpty() && !dir.isEmpty() &&
                            localBranchExists(dir, reviewHead);
    const QString defaultBase = repoDefaultBranchFast();
    m_branchCompareBase =
        pr.base.trimmed().isEmpty() || pr.base == defaultBase ? QString()
                                                               : pr.base;
    const QString graphRef = liveBranch ? reviewHead : defaultBase;
    if (!graphRef.isEmpty() && m_repoBranch != graphRef)
        setRepoBranch(graphRef);
    setCommitWorkspacePage(kCommitWorkspaceRangePage);
    if (liveBranch) {
        showBranchDiff(reviewHead);
    } else {
        const bool leavingAnotherRange = enterBranchDiffRange(pr.head);
        m_branchDiffBranch = pr.head;
        m_branchDiffWorkDir.clear();
        // The pull renders on workers; until it lands, the pane must not keep
        // showing whichever branch or pull was up before it.
        if (leavingAnotherRange)
            beginBranchDiffTransition(pr.head);
        updateCommitsCompareIndicator(); // "<head> -> <base>" on the branch row
        updateBranchDetailActions(pr.head);
        m_branchDiffLastPatch = pr.patch.toUtf8();
        m_branchDiffLastEmpty = QStringLiteral("This pull request has no changes.");
        m_branchDiffLastValid = true;
        ++m_branchScopeDiffGen; // orphan any in-flight scope render
        beginBranchDiffLog(QStringLiteral("Showing pull #%1").arg(pullNumber));
        renderBranchDiffPatch(pr.patch, m_branchDiffLastEmpty,
                              QStringLiteral("pull/") + QString::number(pullNumber),
                              [this] { finishBranchDiffTransition(); });
    }
    if (m_branchDiffView)
        m_branchDiffView->setFocus();
    QTimer::singleShot(0, this, [this] {
        refreshCommitsUnlessCurrent();
    });
}

void MainWindow::registerDiffView(QTextEdit *view)
{
    if (!view || m_diffViews.contains(view))
        return;
    m_diffViews.append(view);
    view->setLineWrapMode(QTextEdit::WidgetWidth);
    view->setFrameShape(QFrame::NoFrame);
    view->document()->setDocumentMargin(0);
    view->viewport()->installEventFilter(this); // Ctrl+wheel, see eventFilter
    if (auto *browser = qobject_cast<QTextBrowser *>(view)) {
        browser->setOpenLinks(false);
        // Every diff surface can draw a markdown file's Preview pill, and none
        // of their own anchor handlers need to know about it.
        connect(browser, &QTextBrowser::anchorClicked, browser,
                [this, browser](const QUrl &url) {
                    if (url.scheme() == QLatin1String("mdpreview")) {
                        openDiffMarkdownPreview(browser, url);
                        return;
                    }
                    handleDiffFileActionLink(browser, url);
                });
    }
    addDiffStreamFinishedHook(view, [this, view] { onDiffStreamFinished(view); });
    connect(view, &QObject::destroyed, this, [this](QObject *o) {
        auto *dead = static_cast<QTextEdit *>(o);
        m_diffViews.removeAll(dead);
        m_diffRestoreScroll.remove(dead);
    });
}

void MainWindow::setDiffHtml(QTextEdit *view, const QString &html, bool endCap)
{
    if (!view)
        return;
    renderDiffStreamed(view, html, diffStyleSheet(m_diffFontPt), endCap);
}

void MainWindow::onDiffStreamFinished(QTextEdit *view)
{
    if (!view)
        return;
    const auto scroll = m_diffRestoreScroll.find(view);
    if (scroll != m_diffRestoreScroll.end()) {
        if (QScrollBar *vbar = view->verticalScrollBar())
            vbar->setValue(qMin(*scroll, vbar->maximum()));
        m_diffRestoreScroll.erase(scroll);
    }
    if (view == m_pullDiff) {
        m_pullFileTops.clear(); // file positions moved as the rest landed
        m_pullStickyFile.clear();
        if (m_pullDiffSearchBar && m_pullDiffSearchBar->isVisible())
            pullDiffSearchRecompute();
        updatePullDiffScrollState();
        paintPullDiffLineSelection();
        positionPullInlineComposer();
    } else if (view == m_scmDiff) {
        m_scmFileTops.clear(); // file positions moved as the rest landed
        int index = m_scmSectionKeys.indexOf(m_scmPendingScrollKey);
        if (index < 0 && !m_scmPendingScrollKey.isEmpty()) {
            const QString path = m_scmPendingScrollKey.mid(2);
            index = m_scmSectionPaths.indexOf(path);
        }
        if (index >= 0) {
            m_scmActiveSectionKey = m_scmSectionKeys.at(index);
            scrollDiffToAnchor(m_scmDiff, m_scmSectionAnchors.at(index));
        }
        if (index >= 0 && pinScmDiffSectionToTop(index)) {
            m_scmPendingScrollKey.clear();
            updateScmDiffActiveOutline();
        } else {
            // The loading placeholder also completes a tiny diff stream. Keep
            // the requested file queued until the background patch read has
            // supplied real sections; discard only a current completed patch
            // that genuinely does not contain it.
            if (m_scmPatchValid && !m_scmPatchReadInFlight)
                m_scmPendingScrollKey.clear();
            updateScmDiffScrollState();
        }
    } else if (view == m_branchDiffView) {
        m_branchFileTops.clear(); // file positions moved as the rest landed
        m_branchStickyFile.clear();
        rebuildBranchDiffSpans();
        if (m_branchDiffLogAwaitingLayout) {
            m_branchDiffLogAwaitingLayout = false;
            m_branchDiffLogTookMs = m_branchDiffLogClock.isValid()
                                        ? m_branchDiffLogClock.elapsed()
                                        : 0;
            appendBranchDiffLog(
                QStringLiteral("Whole diff laid out: %1 files, %2 since the load "
                               "began")
                    .arg(m_branchDiffFilePaths.size())
                    .arg(forkmesh::backgroundElapsedText(
                        m_branchDiffLogTookMs)),
                BranchDiffLogKind::Ok);
        }
        if (m_branchDiffSearchBar && m_branchDiffSearchBar->isVisible())
            branchDiffSearchRecompute();
        // Both overlays still carry the geometry of the document that was just
        // replaced. Painting resumes with this one, so leaving them until the
        // next scroll tick flashes the outline over whatever the new page put
        // where the old file used to be — which a paged diff does mid-scroll,
        // every time the loaded window moves.
        updateBranchDiffSticky();
        updateBranchDiffActiveOutline();
        if (m_pullInlineView == m_branchDiffView) {
            paintPullDiffLineSelection();
            positionPullInlineComposer();
        }
    }
}

void MainWindow::adjustDiffFont(int delta)
{
    const int next = qBound(8, m_diffFontPt + delta, 28);
    if (next == m_diffFontPt)
        return;
    m_diffFontPt = next;
    QSettings().setValue(kDiffFontPtSetting, m_diffFontPt);
    for (QTextEdit *view : m_diffViews) {
        if (!view || view->document()->isEmpty())
            continue;
        QScrollBar *vbar = view->verticalScrollBar();
        const int scroll = vbar ? vbar->value() : 0;
        if (scroll > 0)
            m_diffRestoreScroll.insert(view, scroll);
        if (!restyleDiffStreamed(view, diffStyleSheet(m_diffFontPt)))
            continue;
        if (vbar)
            vbar->setValue(qMin(scroll, vbar->maximum()));
    }
    m_pullDiffRenderKey.clear(); // the pull view's skip-relayout cache is now stale
    m_pullDiffSourceKey.clear(); // …and the skip-rebuild cache in front of it
    m_scmDiffRenderKey.clear();  // ditto for the working-tree changes diff
    m_scmDiffSourceKey.clear();
    if (m_pullDiffSearchBar && m_pullDiffSearchBar->isVisible())
        pullDiffSearchRecompute();
    if (m_branchDiffSearchBar && m_branchDiffSearchBar->isVisible())
        branchDiffSearchRecompute();
}

QHash<QString, QString> MainWindow::buildPullLineNotes(const PullRequest &pr)
{
    QHash<QString, QString> notes;
    const auto htmlBody = [](QString text) {
        text = text.toHtmlEscaped();
        text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        return text;
    };
    {
        const bool canApplyFixes = pr.status == QLatin1String("open") &&
                                   pullStoreForCurrentRepo().canWrite();
        const PullReviewSnapshot snapshot = buildPullReviewSnapshot(pr);
        for (const PullReviewThread &thread : snapshot.threads) {
            if (thread.path.isEmpty())
                continue;
            const bool fileLevel = thread.lineStart <= 0;
            const QString side =
                thread.side.isEmpty() ? QStringLiteral("new") : thread.side;
            // A thread is painted on the line its commented text sits on in
            // the diff being shown (which may have moved since it was
            // written); one whose text left the diff is pinned to the file
            // and says so, rather than landing on unrelated code.
            const QString key =
                fileLevel || thread.outdated()
                    ? fileLevelNoteKey(thread.path)
                    : pullLineNoteKey(thread.path, side, thread.currentLineStart);
            const QString writtenRange =
                thread.lineEnd > thread.lineStart
                    ? QStringLiteral("lines %1\xE2\x80\x93%2")
                          .arg(thread.lineStart)
                          .arg(thread.lineEnd)
                    : QStringLiteral("line %1").arg(thread.lineStart);
            QString stateBadges =
                QStringLiteral("<span class='threadstate %1'>%2</span>")
                    .arg(thread.resolved ? QStringLiteral("resolved")
                                         : QStringLiteral("unresolved"),
                         thread.resolved ? QStringLiteral("Resolved")
                                         : QStringLiteral("Unresolved"));
            QString placement;
            if (thread.outdated()) {
                stateBadges += QStringLiteral(
                    " <span class='threadstate outdated'>Outdated</span>");
                placement = QStringLiteral(
                    "Written on %1 %2 of %3; that code is no longer in this "
                    "diff, so the thread is shown here instead of on a line "
                    "it did not comment on.")
                                .arg(side.toHtmlEscaped(), writtenRange,
                                     thread.patchset.isEmpty()
                                         ? QStringLiteral("an earlier revision")
                                         : QStringLiteral("revision <code>%1</code>")
                                               .arg(thread.patchset.left(10)
                                                        .toHtmlEscaped()));
            } else if (thread.moved()) {
                stateBadges += QStringLiteral(
                    " <span class='threadstate moved'>Moved</span>");
                placement = QStringLiteral(
                    "Written on %1 %2 of revision <code>%3</code>; the same "
                    "code now sits here.")
                                .arg(side.toHtmlEscaped(), writtenRange,
                                     thread.patchset.left(10).toHtmlEscaped());
            }
            const int shownLine = thread.outdated() ? thread.lineStart
                                                    : thread.currentLineStart;
            QString note =
                fileLevel
                    ? QStringLiteral("<div class='reviewthread'><div class='threadhead'>"
                                     "<b>Review thread</b> on this file %1</div>")
                          .arg(stateBadges)
                    : QStringLiteral("<div class='reviewthread'><div class='threadhead'>"
                                     "<b>Review thread</b> on %1 line %2 %3</div>")
                          .arg(side.toHtmlEscaped())
                          .arg(shownLine)
                          .arg(stateBadges);
            if (!placement.isEmpty())
                note += QStringLiteral("<div class='threadplace'>%1</div>")
                            .arg(placement);
            for (const PullEvent &ev : thread.events) {
                const QString who =
                    ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
                const QString when = formatIssueRelativeTime(ev.ts);
                if (ev.type == QLatin1String("thread-state")) {
                    const QString action =
                        ev.state == QLatin1String("resolved")
                            ? QStringLiteral("resolved this thread")
                            : QStringLiteral("reopened this thread");
                    note += QStringLiteral(
                                "<div class='threadsystem'><b>%1</b> %2 %3</div>")
                                .arg(who.toHtmlEscaped(), action, when);
                    continue;
                }
                if (ev.type == QLatin1String("suggestion-state")) {
                    const QString action =
                        ev.state == QLatin1String("applied")
                            ? QStringLiteral("marked the suggestion applied")
                            : QStringLiteral("updated the suggestion");
                    note += QStringLiteral(
                                "<div class='threadsystem'><b>%1</b> %2 %3</div>")
                                .arg(who.toHtmlEscaped(), action, when);
                    if (!ev.body.isEmpty())
                        note += QStringLiteral("<div class='threadbody'>%1</div>")
                                    .arg(htmlBody(ev.body));
                    continue;
                }
                QString verb = QStringLiteral("commented");
                if (ev.type == QLatin1String("thread-comment"))
                    verb = QStringLiteral("started this thread");
                else if (ev.type == QLatin1String("thread-reply"))
                    verb = QStringLiteral("replied");
                note += QStringLiteral("<div class='threadevent'>"
                                       "<div class='notehdr'><b>%1</b> %2 %3</div>"
                                       "<div class='threadbody'>%4</div>")
                            .arg(who.toHtmlEscaped(), verb, when, htmlBody(ev.body));
                if (!ev.suggestionPatch.isEmpty()) {
                    note += QStringLiteral(
                                "<pre class='suggestion'>%1</pre>")
                                .arg(ev.suggestionPatch.toHtmlEscaped());
                }
                note += QStringLiteral("</div>");
            }
            if (!thread.id.isEmpty() && !thread.id.startsWith(QLatin1String("legacy:"))) {
                note += QStringLiteral("<div class='threadactions'>"
                                       "<a href='thread:reply:%1'>Reply</a>")
                            .arg(thread.id.toHtmlEscaped());
                if (thread.resolved) {
                    note += QStringLiteral(
                        " &nbsp; <a href='thread:unresolve:%1'>Reopen</a>")
                                .arg(thread.id.toHtmlEscaped());
                } else {
                    note += QStringLiteral(
                        " &nbsp; <a href='thread:resolve:%1'>Resolve</a>")
                                .arg(thread.id.toHtmlEscaped());
                }
                if (canApplyFixes && !thread.resolved && thread.hasSuggestion &&
                    thread.suggestionState != QLatin1String("applied"))
                    note += QStringLiteral(" &nbsp; <a href='thread:applyfix:%1'>"
                                           "Apply fix &amp; commit</a>")
                                .arg(thread.id.toHtmlEscaped());
                note += QStringLiteral("</div>");
            }
            note += QStringLiteral("</div>");
            notes[key] += note;
        }
    }

    QString repoKey;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        repoKey = (repo.owner + QLatin1Char('/') + repo.name).toLower();
    }
    for (const PullInlineDraft &draft : std::as_const(m_pullInlineDrafts)) {
        if (draft.repoKey != repoKey || draft.number != pr.number)
            continue;
        const int first = qMin(draft.lineStart, draft.lineEnd);
        const int last = qMax(draft.lineStart, draft.lineEnd);
        const QString range = first == last
                                  ? QString::number(first)
                                  : QStringLiteral("%1–%2").arg(first).arg(last);
        const QString key =
            pullLineNoteKey(draft.path, draft.side, draft.lineStart);
        notes[key] +=
            QStringLiteral("<div class='reviewthread'><div class='threadhead'>"
                           "<b>Draft comment</b> on %1 lines %2 "
                           "<span class='threadstate unresolved'>Pending</span>"
                           "</div><div class='threadbody'>%3</div></div>")
                .arg(draft.side.toHtmlEscaped(), range,
                     htmlBody(draft.body));
    }

    const bool composerHere =
        m_pullInlineBranch.isEmpty()
            ? pr.number == m_currentPullNumber
            : pr.number == 0 && pr.head == m_pullInlineBranch;
    if (!m_pullInlinePath.isEmpty() && m_pullInlineStartLine > 0 &&
        m_pullInlineEndLine > 0 && composerHere) {
        const int first = qMin(m_pullInlineStartLine, m_pullInlineEndLine);
        const int last = qMax(m_pullInlineStartLine, m_pullInlineEndLine);
        const QString range = first == last
                                  ? QString::number(first)
                                  : QStringLiteral("%1–%2").arg(first).arg(last);
        const QString key = pullLineNoteKey(m_pullInlinePath, m_pullInlineSide,
                                            last);
        notes[key] +=
            QStringLiteral("<a name='pull-inline-composer'>&#8203;</a>"
                           "<div class='inlinecomposerspace'>"
                           "Commenting on %1 lines %2<br><br><br><br><br><br>"
                           "</div>")
                .arg(m_pullInlineSide.toHtmlEscaped(), range);
    }

    for (auto it = notes.begin(); it != notes.end(); ++it) {
        // File-level (outdated) threads have no gutter marker to reopen them
        // from, so they never fold away.
        if (it.key().endsWith(QLatin1String("\x1f" "file:0")))
            continue;
        if (!m_pullOpenLineNotes.contains(it.key()) &&
            it.value().contains(QLatin1String("class='reviewthread'"))) {
            const int composer =
                it.value().indexOf(QLatin1String("<a name='pull-inline-composer'"));
            const QString threadHtml = composer < 0 ? it.value()
                                                    : it.value().left(composer);
            const QString trailing = composer < 0 ? QString()
                                                  : it.value().mid(composer);
            it.value() = QLatin1Char('\x1c') + threadHtml +
                         QLatin1Char('\x1d') + trailing;
        }
    }
    return notes;
}

// The signature of what the pull diff currently shows, minus the parts a Viewed
// tick cannot change — those are held from the last full render so folding one
// file does not have to rebuild the line notes or re-hash the patch.
QString MainWindow::pullDiffSourceKey(const QSet<QString> &viewed,
                                      const QSet<QString> &expanded) const
{
    QStringList viewedKeys(viewed.cbegin(), viewed.cend());
    viewedKeys.sort();
    QStringList expandedKeys(expanded.cbegin(), expanded.cend());
    expandedKeys.sort();
    return QString::number(m_currentPullNumber) + QLatin1Char('\x1f') +
           QString(diffSplitPref() ? QLatin1Char('s') : QLatin1Char('u')) +
           QLatin1Char('\x1f') + diffStyleSheet(m_diffFontPt) +
           QLatin1Char('\x1f') + viewedKeys.join(QLatin1Char('\x1e')) +
           QLatin1Char('\x1f') + expandedKeys.join(QLatin1Char('\x1e')) +
           QLatin1Char('\x1f') + m_pullDiffNotesKey + QLatin1Char('\x1f') +
           m_pullDiffPatchKey;
}

// Folds or reopens one file of the pull diff in place. The whole PR is one
// QTextDocument, so re-rendering it to tick one file re-lays out every other
// file and visibly redraws the pane. False when the swap is not available and
// the caller has to re-render after all.
bool MainWindow::swapPullDiffFileInPlace(const QString &path)
{
    if (!m_pullDiff || m_currentPullNumber < 0 || m_pullDiffSourceKey.isEmpty())
        return false;
    const QString anchor = m_pullFileAnchors.value(path);
    if (anchor.isEmpty())
        return false;
    const QString context =
        QStringLiteral("pull/") + QString::number(m_currentPullNumber);
    const QSet<QString> viewed = loadDiffViewed(context);
    const QSet<QString> expanded = diffExpandedViewed(context);
    const DiffFileBlockSwap swap = swapDiffFileViewedState(
        m_pullDiff, anchor, viewed.contains(path), expanded.contains(path));
    if (!swap.applied)
        return false;
    // Both keys still describe the document, so restate them rather than
    // dropping them: a stale key would send the next poll straight back into
    // the full re-render this swap exists to avoid.
    m_pullDiffSourceKey = pullDiffSourceKey(viewed, expanded);
    m_pullDiffRenderKey.replace(swap.oldHtml, swap.newHtml);
    m_pullFileTops.clear(); // measured off the layout the swap just changed
    m_pullStickyFile.clear();
    updatePullViewedLabel();
    if (m_pullDiffSearchBar && m_pullDiffSearchBar->isVisible())
        pullDiffSearchRecompute();
    QTimer::singleShot(0, this, &MainWindow::updatePullDiffScrollState);
    return true;
}

void MainWindow::updatePullViewedLabel()
{
    if (!m_pullViewedLabel)
        return;
    if (m_pullFileOrder.isEmpty() || m_currentPullNumber < 0) {
        m_pullViewedLabel->clear();
        m_pullViewedLabel->hide();
        return;
    }
    const QString context =
        QStringLiteral("pull/") + QString::number(m_currentPullNumber);
    const QSet<QString> viewed = loadDiffViewed(context);
    int seen = 0;
    for (const QString &path : std::as_const(m_pullFileOrder))
        if (viewed.contains(path))
            ++seen;
    m_pullViewedLabel->setText(diffViewedCountHtml(
        seen, m_pullFileOrder.size(), diffViewedMarksCurrent(context, m_pullFileOrder)));
    m_pullViewedLabel->show();
}

void MainWindow::renderPullDiff()
{
    if (!m_pullDiff)
        return;

    QHash<QString, QString> notes;
    const PullRequest *pr = nullptr;
    for (const PullRequest &p : m_currentPulls)
        if (p.number == m_currentPullNumber)
            pr = &p;
    if (pr)
        notes = buildPullLineNotes(*pr);

    QList<DiffFileEntry> files;
    const QString fullPatch = pr ? pr->patch : QString();
    const QString context =
        QStringLiteral("pull/") + QString::number(m_currentPullNumber);
    // A push to the branch re-cuts the patch: any file it changed loses its
    // Viewed mark, because that mark was made against the old diff.
    const QSet<QString> viewed = refreshDiffViewedForPatch(context, fullPatch);
    const QString styleSheet = diffStyleSheet(m_diffFontPt);

    QStringList noteKeys = notes.keys();
    noteKeys.sort();
    QString notesKey;
    for (const QString &k : std::as_const(noteKeys))
        notesKey += k + QLatin1Char('\x1e') + notes.value(k) + QLatin1Char('\x1d');
    // Held so an in-place Viewed swap can restate the key without rebuilding
    // the notes or re-hashing the patch (see swapPullDiffFileInPlace).
    m_pullDiffNotesKey = notesKey;
    m_pullDiffPatchKey = QString::number(fullPatch.size()) + QLatin1Char(':') +
                         QString::number(qHash(fullPatch));
    const QSet<QString> expanded = diffExpandedViewed(context);
    const QString sourceKey = pullDiffSourceKey(viewed, expanded);
    if (!m_pullDiffSourceKey.isEmpty() && sourceKey == m_pullDiffSourceKey &&
        !m_pullDiffRenderKey.isEmpty())
        return;
    m_pullDiffSourceKey = sourceKey;

    const QString html =
        renderDiffHtml(fullPatch, files, QString(), QString(), QString(),
                       QStringLiteral("*"), notes, viewed, expanded);

    m_pullFileAnchors.clear();
    m_pullFileOrder.clear();
    m_pullStickyLabelHtml.clear();
    m_pullFileTops.clear(); // positions change on re-render; force a recompute
    for (const DiffFileEntry &f : files) {
        m_pullFileAnchors.insert(f.path, f.anchor);
        m_pullFileOrder.append(f.path);
        m_pullStickyLabelHtml.insert(f.path,
                                     diffFileLabelHtml(f, viewed.contains(f.path)));
    }
    updatePullViewedLabel();

    const QString body =
        html.isEmpty() ? QStringLiteral("<p style='color:#8b949e'>(no changes)</p>") : html;

    const QString key = QString::number(m_currentPullNumber) +
                        QLatin1Char('\x1f') + styleSheet + QLatin1Char('\x1f') +
                        body;
    if (key == m_pullDiffRenderKey)
        return;
    m_pullDiffRenderKey = key;

    setDiffHtml(m_pullDiff, body);
    // What a later Viewed tick folds in place instead of re-rendering.
    noteDiffFileBlocks(m_pullDiff, html, files, viewed, expanded,
                       /*comments=*/true);
    if (m_pullDiffSearchBar && m_pullDiffSearchBar->isVisible())
        pullDiffSearchRecompute();
    m_pullStickyFile.clear();
    QTimer::singleShot(0, this, &MainWindow::updatePullDiffScrollState);
}

void MainWindow::scrollPullDiffToFile(const QString &filePath)
{
    if (!m_pullDiff)
        return;
    const QString anchor = m_pullFileAnchors.value(filePath);
    if (anchor.isEmpty())
        return;
    scrollDiffToAnchor(m_pullDiff, anchor);
}

void MainWindow::selectPullFileInList(const QString &filePath)
{
    if (!m_pullFiles)
        return;
    for (int row = 0; row < m_pullFiles->count(); ++row) {
        QListWidgetItem *item = m_pullFiles->item(row);
        if (item && item->data(Qt::UserRole).toString() == filePath) {
            if (m_pullFiles->currentItem() == item)
                return;
            m_pullSuppressFileScroll = true;
            m_pullFiles->setCurrentItem(item);
            m_pullFiles->scrollToItem(item);
            m_pullSuppressFileScroll = false;
            return;
        }
    }
}

// code-health: allow(deep-nested-loop) one pass over the diff document's fragments with a hash lookup per anchor name (usually one); the result is cached in m_pullFileTops until the diff re-renders
void MainWindow::computePullFileTops()
{
    m_pullFileTops = QList<int>(m_pullFileOrder.size(), -1);
    if (!m_pullDiff || m_pullFileOrder.isEmpty())
        return;
    QScrollBar *vbar = m_pullDiff->verticalScrollBar();
    const int viewTop = vbar ? vbar->value() : 0;
    QHash<QString, int> anchorIndex;
    for (int i = 0; i < m_pullFileOrder.size(); ++i) {
        const QString a = m_pullFileAnchors.value(m_pullFileOrder.at(i));
        if (!a.isEmpty())
            anchorIndex.insert(a, i);
    }
    QTextDocument *doc = m_pullDiff->document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment frag = it.fragment();
            if (!frag.isValid() || !frag.charFormat().isAnchor())
                continue;
            for (const QString &name : frag.charFormat().anchorNames()) {
                const auto ai = anchorIndex.constFind(name);
                if (ai == anchorIndex.constEnd())
                    continue;
                QTextCursor cur(doc);
                cur.setPosition(frag.position());
                // The anchor character sits inside the header cell, a cell
                // padding below the row's own top edge.
                const QRectF row = diffFileRowRect(m_pullDiff, frag.position());
                m_pullFileTops[ai.value()] =
                    row.isNull() ? m_pullDiff->cursorRect(cur).top() + viewTop
                                 : qRound(row.top());
            }
        }
    }
}

void MainWindow::layoutPullStickyHeader()
{
    if (!m_pullStickyHeader || !m_pullDiff)
        return;
    const int height = m_pullStickyHeader->sizeHint().height();
    // See layoutScmStickyHeader(): the offset is the scroll tick's, so a plain
    // relayout keeps whatever room the next file's header had already claimed.
    placeDiffOverlay(m_pullStickyHeader, m_pullDiff,
                     QRect(0, qBound(-height, m_pullStickyTopOffset, 0),
                           m_pullDiff->viewport()->width(), height));
}

void MainWindow::updatePullDiffScrollState()
{
    if (!m_pullDiff || !m_pullStickyHeader)
        return;
    if (m_currentPullNumber < 0 || m_pullFileOrder.isEmpty()) {
        m_pullStickyHeader->hide();
        return;
    }
    QScrollBar *vbar = m_pullDiff->verticalScrollBar();
    if (!vbar)
        return;
    if (vbar->value() <= vbar->minimum()) {
        m_pullStickyHeader->hide();
        return;
    }
    const int viewTop = vbar->value();
    const int viewBottom = viewTop + m_pullDiff->viewport()->height();
    QTextDocument *doc = m_pullDiff->document();
    const int docHeight = doc->documentLayout()->documentSize().height();

    if (m_pullFileTops.size() != m_pullFileOrder.size())
        computePullFileTops();
    const QList<int> &tops = m_pullFileTops;

    int idx = -1, fileTop = 0, fileBottom = 0;
    for (int i = 0; i < m_pullFileOrder.size(); ++i) {
        if (tops[i] < 0)
            continue;
        const int bottom =
            (i + 1 < tops.size() && tops[i + 1] >= 0) ? tops[i + 1] : docHeight;
        if (bottom > viewTop) {
            idx = i;
            fileTop = tops[i];
            fileBottom = bottom;
            break;
        }
    }
    if (idx < 0) {
        idx = m_pullFileOrder.indexOf(m_pullStickyFile);
        if (idx < 0)
            idx = 0;
        fileTop = 0;
        fileBottom = qMax(1, docHeight);
    }
    const QString path = m_pullFileOrder.at(idx);

    double progress = 1.0;
    if (fileBottom > fileTop)
        progress = double(viewBottom - fileTop) / double(fileBottom - fileTop);
    progress = qBound(0.0, progress, 1.0);

    const QSet<QString> viewed =
        loadDiffViewed(QStringLiteral("pull/") + QString::number(m_currentPullNumber));
    const bool isViewed = viewed.contains(path);
    if (path != m_pullStickyFile) {
        m_pullStickyFile = path;
        m_pullStickyPath->setText(m_pullStickyLabelHtml.contains(path)
                                      ? m_pullStickyLabelHtml.value(path)
                                      : diffStickyPathHtml(path));
        m_pullStickyPath->setToolTip(path);
        selectPullFileInList(path);
    }
    m_pullStickyControls->setText(diffRowControlsHtml(
        path, progress, isViewed, /*comments=*/true, QString(),
        diffExpandedViewed(QStringLiteral("pull/") +
                           QString::number(m_currentPullNumber))
            .contains(path)));

    const bool wasHidden = m_pullStickyHeader->isHidden();
    m_pullStickyTopOffset = diffStickyTopOffset(
        m_pullStickyHeader->sizeHint().height(), viewTop, fileBottom);
    layoutPullStickyHeader();
    if (wasHidden) {
        // Only when it comes back: raise() dirties the widget, and doing that
        // on every scroll tick is what makes the bar flicker.
        m_pullStickyHeader->show();
        m_pullStickyHeader->raise();
    }
}

void MainWindow::applyAutoMarkViewedOnScroll()
{
    if (!m_pullDiff || m_currentPullNumber < 0 || m_pullFileOrder.isEmpty())
        return;
    // A paged document exposes only one bounded slice to QTextDocument. Do not
    // infer that a multi-page file was reviewed merely because this slice's
    // scrollbar reached its end.
    if (!autoMarkViewedOnScrollPref() || diffIsPaginated(m_pullDiff))
        return;
    QScrollBar *vbar = m_pullDiff->verticalScrollBar();
    if (!vbar)
        return;

    const int viewTop = vbar->value();
    const int viewBottom = viewTop + m_pullDiff->viewport()->height();
    QTextDocument *doc = m_pullDiff->document();
    const int docHeight = doc->documentLayout()->documentSize().height();

    if (m_pullFileTops.size() != m_pullFileOrder.size())
        computePullFileTops(); // one document pass, shared with the sticky header
    const QList<int> &tops = m_pullFileTops;

    const QString context =
        QStringLiteral("pull/") + QString::number(m_currentPullNumber);
    const QSet<QString> viewed = loadDiffViewed(context);
    QString currentFile; // first file the reviewer hasn't fully scrolled through
    QStringList newlyViewed;
    for (int i = 0; i < m_pullFileOrder.size(); ++i) {
        if (tops[i] < 0)
            continue;
        const int bottom = (i + 1 < tops.size() && tops[i + 1] >= 0) ? tops[i + 1]
                                                                     : docHeight;
        if (bottom <= viewBottom) {
            if (!viewed.contains(m_pullFileOrder.at(i)))
                newlyViewed << m_pullFileOrder.at(i);
        } else if (currentFile.isEmpty()) {
            currentFile = m_pullFileOrder.at(i);
        }
    }
    if (newlyViewed.isEmpty())
        return;

    // Folding under the reviewer's own scroll is the worst place to re-render
    // the whole diff, so take the in-place route file by file and only fall
    // back once — for whichever file the document could not carry.
    bool swappedEvery = true;
    for (const QString &path : std::as_const(newlyViewed)) {
        setDiffViewed(context, path, true);
        swappedEvery = swapPullDiffFileInPlace(path) && swappedEvery;
    }
    if (!swappedEvery)
        renderPullDiff();
    // Either way the files that folded were above the viewport, so the rows the
    // reviewer is reading rose by their height: land on the first file they
    // have not finished instead of leaving them mid-scroll somewhere else.
    if (!currentFile.isEmpty())
        scrollPullDiffToFile(currentFile);
}

void MainWindow::pullSelectAdjacentChange(int delta)
{
    pullScrollToAdjacentHunk(delta);
}

bool MainWindow::pullScrollToAdjacentHunk(int delta)
{
    if (!m_pullDiff)
        return false;
    QScrollBar *vbar = m_pullDiff->verticalScrollBar();
    if (!vbar)
        return false;
    flushDiffStream(m_pullDiff);
    const int curTop = vbar->value();
    int target = delta > 0 ? std::numeric_limits<int>::max()
                           : std::numeric_limits<int>::min();
    QTextCursor cur(m_pullDiff->document());
    while (true) {
        cur = m_pullDiff->document()->find(QStringLiteral("@@ -"), cur);
        if (cur.isNull())
            break;
        QTextCursor lineCur(cur);
        lineCur.setPosition(cur.selectionStart());
        lineCur.movePosition(QTextCursor::StartOfLine);
        const int y = m_pullDiff->cursorRect(lineCur).top() + curTop;
        if (delta > 0) {
            if (y > curTop + 4)
                target = std::min(target, y);
        } else if (y < curTop - 4) {
            target = std::max(target, y);
        }
    }
    if (delta > 0 ? target == std::numeric_limits<int>::max()
                  : target == std::numeric_limits<int>::min()) {
        return showDiffPageContainingText(
            m_pullDiff, QStringLiteral("@@ -"), delta,
            /*wrap=*/false);
    }
    vbar->setValue(std::clamp(target - 4, vbar->minimum(), vbar->maximum()));
    return true;
}

void MainWindow::togglePullDiffSearch(bool show)
{
    if (!m_pullDiffSearchBar || !m_pullDiffSearchInput)
        return;
    m_pullDiffSearchBar->setVisible(show);
    if (show) {
        m_pullDiffSearchInput->setFocus();
        m_pullDiffSearchInput->selectAll();
    } else {
        m_pullDiffSearchInput->clear(); // triggers pullDiffSearchRecompute to clear highlights
        if (m_pullDiff)
            m_pullDiff->setFocus();
    }
}

void MainWindow::pullDiffSearchRecompute()
{
    if (!m_pullDiff)
        return;
    m_pullDiffSearchMatches.clear();
    m_pullDiffSearchIndex = -1;

    const QString term =
        m_pullDiffSearchInput ? m_pullDiffSearchInput->text() : QString();
    if (!term.isEmpty()) {
        flushDiffStream(m_pullDiff);
        if (diffStreamInProgress(m_pullDiff)) {
            applyDiffSearchHighlights(
                m_pullDiff, m_pullDiffSearchMatches, m_pullDiffSearchIndex,
                m_pullDiffSearchCount, false);
            return; // the finished hook retries against the complete page
        }
        QTextCursor cur = m_pullDiff->document()->find(term);
        while (!cur.isNull()) {
            m_pullDiffSearchMatches.append(cur);
            if (m_pullDiffSearchMatches.size() >= 5000)
                break; // safety cap on pathological match counts
            cur = m_pullDiff->document()->find(term, cur);
        }
        if (!m_pullDiffSearchMatches.isEmpty()) {
            const int arrival =
                m_pullDiff->property("forkmeshDiffSearchArrival").toInt();
            m_pullDiff->setProperty("forkmeshDiffSearchArrival", QVariant());
            m_pullDiffSearchIndex =
                arrival < 0 ? m_pullDiffSearchMatches.size() - 1 : 0;
        } else {
            m_pullDiff->setProperty("forkmeshDiffSearchArrival", 1);
            if (showDiffPageContainingText(m_pullDiff, term, 1,
                                           /*wrap=*/true)) {
                return;
            }
            m_pullDiff->setProperty("forkmeshDiffSearchArrival", QVariant());
        }
    }

    applyDiffSearchHighlights(m_pullDiff, m_pullDiffSearchMatches,
                                  m_pullDiffSearchIndex, m_pullDiffSearchCount,
                                  term.isEmpty());
    if (m_pullDiffSearchIndex >= 0)
        pullDiffSearchGoTo(0);
}

void MainWindow::pullDiffSearchGoTo(int delta)
{
    if (!m_pullDiff || m_pullDiffSearchMatches.isEmpty())
        return;
    QScrollBar *vbar = m_pullDiff->verticalScrollBar();
    if (!vbar)
        return;

    const int count = m_pullDiffSearchMatches.size();
    if (delta != 0 &&
        ((delta > 0 && m_pullDiffSearchIndex == count - 1) ||
         (delta < 0 && m_pullDiffSearchIndex == 0))) {
        const QString term =
            m_pullDiffSearchInput ? m_pullDiffSearchInput->text() : QString();
        if (!term.isEmpty()) {
            m_pullDiff->setProperty("forkmeshDiffSearchArrival", delta);
            if (showDiffPageContainingText(m_pullDiff, term, delta,
                                           /*wrap=*/true)) {
                return;
            }
            m_pullDiff->setProperty("forkmeshDiffSearchArrival", QVariant());
        }
    }
    m_pullDiffSearchIndex =
        ((m_pullDiffSearchIndex + delta) % count + count) % count;
    applyDiffSearchHighlights(m_pullDiff, m_pullDiffSearchMatches,
                                  m_pullDiffSearchIndex, m_pullDiffSearchCount,
                                  false);

    const QTextCursor &target = m_pullDiffSearchMatches.at(m_pullDiffSearchIndex);
    QTextCursor lineCur(target);
    lineCur.setPosition(target.selectionStart());
    const int y = m_pullDiff->cursorRect(lineCur).top() + vbar->value();
    const int centered = y - m_pullDiff->viewport()->height() / 3;
    vbar->setValue(std::clamp(centered, vbar->minimum(), vbar->maximum()));
}

bool MainWindow::handlePullDiffLinePointerEvent(QObject *obj, QEvent *event)
{
    QTextBrowser *view = nullptr;
    if (m_pullDiff && obj == m_pullDiff->viewport())
        view = m_pullDiff;
    else if (m_branchDiffView && obj == m_branchDiffView->viewport() &&
             (m_branchDiffPullNumber >= 0 || branchDiffTakesBranchComments()))
        view = m_branchDiffView;
    if (!view || !event)
        return false;
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove &&
        type != QEvent::MouseButtonRelease)
        return false;

    auto *mouse = static_cast<QMouseEvent *>(event);
    if (type == QEvent::MouseButtonPress) {
        if (mouse->button() != Qt::LeftButton)
            return false;
        QString path, side;
        int line = 0;
        if (!parsePullLineAnchor(
                view->anchorAt(mouse->position().toPoint()), &path,
                &side, &line))
            return false;
        m_pullInlineView = view;
        m_pullInlineSelecting = true;
        m_pullInlineBranch = view == m_branchDiffView && m_branchDiffPullNumber < 0
                                 ? m_branchDiffBranch
                                 : QString();
        m_pullInlinePath = path;
        m_pullInlineSide = side;
        m_pullInlineStartLine = line;
        m_pullInlineEndLine = line;
        if (m_pullInlineComposerFrame)
            m_pullInlineComposerFrame->hide();
        paintPullDiffLineSelection();
        return true;
    }

    if (!m_pullInlineSelecting)
        return false;
    if (type == QEvent::MouseMove) {
        if (!(mouse->buttons() & Qt::LeftButton))
            return true;
        const int y = mouse->position().toPoint().y();
        const int height = view->viewport()->height();
        if (QScrollBar *bar = view->verticalScrollBar()) {
            if (y < 0)
                bar->setValue(bar->value() + y);
            else if (y > height)
                bar->setValue(bar->value() + y - height);
        }
        QString path;
        int line = 0;
        if (pullLineAnchorAt(view, mouse->position().toPoint(),
                             m_pullInlineSide, &path, &line) &&
            path == m_pullInlinePath && line != m_pullInlineEndLine) {
            m_pullInlineEndLine = line;
            paintPullDiffLineSelection();
        }
        return true;
    }

    if (mouse->button() != Qt::LeftButton)
        return true;
    QString path;
    int line = 0;
    if (pullLineAnchorAt(view, mouse->position().toPoint(), m_pullInlineSide,
                         &path, &line) &&
        path == m_pullInlinePath)
        m_pullInlineEndLine = line;
    m_pullInlineSelecting = false;
    paintPullDiffLineSelection();
    openPullInlineComposer();
    return true;
}

void MainWindow::paintPullDiffLineSelection()
{
    QTextBrowser *view = m_pullInlineView ? m_pullInlineView : m_pullDiff;
    if (!view)
        return;
    if (m_pullInlinePath.isEmpty() || m_pullInlineStartLine <= 0 ||
        m_pullInlineEndLine <= 0) {
        view->setExtraSelections({});
        if (view == m_pullDiff && m_pullDiffSearchBar &&
            m_pullDiffSearchBar->isVisible())
            pullDiffSearchRecompute();
        else if (view == m_branchDiffView && m_branchDiffSearchBar &&
                 m_branchDiffSearchBar->isVisible())
            branchDiffSearchRecompute();
        return;
    }

    const int first = qMin(m_pullInlineStartLine, m_pullInlineEndLine);
    const int last = qMax(m_pullInlineStartLine, m_pullInlineEndLine);
    QList<QTextEdit::ExtraSelection> selections;
    QTextDocument *doc = view->document();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isAnchor())
                continue;
            QString path, side;
            int line = 0;
            if (!parsePullLineAnchor(fragment.charFormat().anchorHref(), &path,
                                     &side, &line) ||
                path != m_pullInlinePath || side != m_pullInlineSide ||
                line < first || line > last)
                continue;
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(doc);
            selection.cursor.setPosition(fragment.position());
            selection.cursor.setPosition(fragment.position() + fragment.length(),
                                         QTextCursor::KeepAnchor);
            QColor color = Theme::nextOr(Theme::Role::Accent,
                                         QColor(QStringLiteral("#1f6feb")));
            color.setAlpha(110);
            selection.format.setBackground(color);
            selections.append(selection);
        }
    }
    view->setExtraSelections(selections);
}

void MainWindow::rerenderPullInlineDiff()
{
    if (m_pullInlineView == m_branchDiffView && m_branchDiffView) {
        if (m_branchDiffLastValid) {
            beginBranchDiffLog(
                m_branchDiffPullNumber >= 0
                    ? QStringLiteral("Re-rendering pull #%1")
                          .arg(m_branchDiffPullNumber)
                    : QStringLiteral("Re-rendering %1 with its review comments")
                          .arg(m_branchDiffBranch));
            renderBranchDiffPatch(QString::fromUtf8(m_branchDiffLastPatch),
                                  m_branchDiffLastEmpty,
                                  m_branchDiffViewedContext);
        } else {
            renderBranchScopeDiff();
        }
        return;
    }
    renderPullDiff();
}

void MainWindow::openPullInlineComposer()
{
    const bool onBranch = !m_pullInlineBranch.isEmpty();
    if (!m_pullInlineComposerFrame || !m_pullInlineCommentEdit ||
        (!onBranch && m_currentPullNumber < 0) || m_pullInlinePath.isEmpty() ||
        m_pullInlineStartLine <= 0 || m_pullInlineEndLine <= 0)
        return;
    const int first = qMin(m_pullInlineStartLine, m_pullInlineEndLine);
    const int last = qMax(m_pullInlineStartLine, m_pullInlineEndLine);
    const QString range = first == last
                              ? QString::number(first)
                              : QStringLiteral("%1–%2").arg(first).arg(last);
    m_pullInlineComposerTitle->setText(
        QStringLiteral("Comment on %1 · %2 line%3 %4%5")
            .arg(m_pullInlinePath, m_pullInlineSide,
                 first == last ? QString() : QStringLiteral("s"), range,
                 onBranch ? QStringLiteral(" · visible to everyone viewing %1")
                                .arg(m_pullInlineBranch)
                          : QString()));
    m_pullInlineCommentEdit->setPlaceholderText(
        onBranch ? QStringLiteral("Leave feedback on this branch. Markdown is "
                                  "supported.")
                 : QStringLiteral("Leave feedback for the agent. Markdown is "
                                  "supported."));
    if (m_pullInlineAddButton)
        m_pullInlineAddButton->setVisible(!onBranch);
    if (m_pullInlineSubmitBatchButton)
        m_pullInlineSubmitBatchButton->setVisible(!onBranch);
    if (m_pullInlineNowButton)
        m_pullInlineNowButton->setText(onBranch ? QStringLiteral("Comment")
                                                : QStringLiteral("Comment now"));
    QTextBrowser *view = m_pullInlineView ? m_pullInlineView : m_pullDiff;
    if (!view)
        return;
    m_pullInlineView = view;
    if (m_pullInlineComposerFrame->parentWidget() != view->viewport())
        m_pullInlineComposerFrame->setParent(view->viewport());
    if (QScrollBar *bar = view->verticalScrollBar())
        m_diffRestoreScroll.insert(view, bar->value());
    m_pullDiffSourceKey.clear();
    m_pullDiffRenderKey.clear();
    rerenderPullInlineDiff();
    updatePullInlineReviewBatchBar();
    QTimer::singleShot(0, this, [this] {
        positionPullInlineComposer();
        if (m_pullInlineCommentEdit)
            m_pullInlineCommentEdit->setFocus();
    });
}

void MainWindow::positionPullInlineComposer()
{
    QTextBrowser *view = m_pullInlineView ? m_pullInlineView : m_pullDiff;
    if (!m_pullInlineComposerFrame || !view ||
        m_pullInlinePath.isEmpty() || m_pullInlineStartLine <= 0) {
        if (m_pullInlineComposerFrame)
            m_pullInlineComposerFrame->hide();
        return;
    }
    int position = -1;
    QTextDocument *doc = view->document();
    for (QTextBlock block = doc->begin(); block.isValid() && position < 0;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor() &&
                fragment.charFormat().anchorNames().contains(
                    QStringLiteral("pull-inline-composer"))) {
                position = fragment.position();
                break;
            }
        }
    }
    if (position < 0) {
        m_pullInlineComposerFrame->hide();
        return;
    }
    QTextCursor cursor(doc);
    cursor.setPosition(position);
    const QRect caret = view->cursorRect(cursor);
    const int x = qMin(136, qMax(74, view->viewport()->width() / 8));
    const int width = qMax(240, view->viewport()->width() - x - 14);
    const int y = caret.top() + 4;
    if (y + 12 < 0 || y >= view->viewport()->height()) {
        m_pullInlineComposerFrame->hide();
        return;
    }
    m_pullInlineComposerFrame->setGeometry(x, y, width,
                                           m_pullInlineComposerFrame->height());
    m_pullInlineComposerFrame->show();
    m_pullInlineComposerFrame->raise();
}

void MainWindow::cancelPullInlineComposer()
{
    const bool hadComposer = !m_pullInlinePath.isEmpty();
    m_pullInlineSelecting = false;
    m_pullInlineBranch.clear();
    m_pullInlinePath.clear();
    m_pullInlineSide.clear();
    m_pullInlineStartLine = 0;
    m_pullInlineEndLine = 0;
    if (m_pullInlineCommentEdit)
        m_pullInlineCommentEdit->clear();
    if (m_pullInlineComposerFrame)
        m_pullInlineComposerFrame->hide();
    paintPullDiffLineSelection();
    QTextBrowser *view = m_pullInlineView ? m_pullInlineView : m_pullDiff;
    if (hadComposer && view) {
        if (QScrollBar *bar = view->verticalScrollBar())
            m_diffRestoreScroll.insert(view, bar->value());
        m_pullDiffSourceKey.clear();
        m_pullDiffRenderKey.clear();
        rerenderPullInlineDiff();
    }
    updatePullInlineReviewBatchBar();
}

void MainWindow::queuePullInlineComment()
{
    if (!m_pullInlineBranch.isEmpty()) {
        submitPullInlineCommentNow();
        return;
    }
    const QString body = m_pullInlineCommentEdit
                             ? m_pullInlineCommentEdit->toPlainText().trimmed()
                             : QString();
    if (body.isEmpty() || m_currentPullNumber < 0 ||
        m_pullInlinePath.isEmpty()) {
        if (m_pullInlineCommentEdit)
            m_pullInlineCommentEdit->setFocus();
        return;
    }
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    PullInlineDraft draft;
    draft.repoKey =
        (repo.owner + QLatin1Char('/') + repo.name).toLower();
    draft.number = m_currentPullNumber;
    draft.path = m_pullInlinePath;
    draft.side = m_pullInlineSide;
    draft.lineStart = qMin(m_pullInlineStartLine, m_pullInlineEndLine);
    draft.lineEnd = qMax(m_pullInlineStartLine, m_pullInlineEndLine);
    draft.body = body;
    const PullAnchor anchor = currentPullAnchor(
        draft.number, draft.path, draft.side, draft.lineStart, draft.lineEnd);
    draft.patchset = anchor.patchset;
    draft.anchorLines = anchor.anchorLines;
    m_pullInlineDrafts.append(draft);
    m_pullOpenLineNotes.insert(
        pullLineNoteKey(draft.path, draft.side, draft.lineStart));
    cancelPullInlineComposer();
    updatePullInlineReviewBatchBar();
}

PullAnchor MainWindow::currentPullAnchor(int number, const QString &path,
                                         const QString &side, int lineStart,
                                         int lineEnd) const
{
    for (const PullRequest &pr : m_currentPulls)
        if (pr.number == number)
            return pullAnchorFor(pr, path, side, lineStart, lineEnd);
    return PullAnchor();
}

bool MainWindow::postPullInlineComment(int number, const QString &path,
                                       const QString &side, int lineStart,
                                       int lineEnd, const QString &body,
                                       const PullAnchor &anchor, QString *error)
{
    if (number < 0 || path.isEmpty() || lineStart <= 0 || body.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("The inline comment is incomplete.");
        return false;
    }
    PullStore store = pullStoreForCurrentRepo();
    if (store.canWrite())
        return store.addThreadComment(number, path, side, lineStart, lineEnd,
                                      body.trimmed(), QString(), anchor, error);
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        if (error)
            *error = QStringLiteral("No repository is selected.");
        return false;
    }
    PullEvent ev;
    ev.type = QStringLiteral("thread-comment");
    ev.path = path;
    ev.side = side;
    ev.lineStart = lineStart;
    ev.lineEnd = lineEnd;
    ev.body = body.trimmed();
    ev.patchset = anchor.patchset;
    ev.anchorLines = anchor.anchorLines;
    ev = store.makeSignedEvent(number, ev);
    submitPullEventToInbox(number, ev);
    return true;
}

void MainWindow::submitPullInlineCommentNow()
{
    const QString body = m_pullInlineCommentEdit
                             ? m_pullInlineCommentEdit->toPlainText().trimmed()
                             : QString();
    if (body.isEmpty() || m_pullInlinePath.isEmpty()) {
        if (m_pullInlineCommentEdit)
            m_pullInlineCommentEdit->setFocus();
        return;
    }
    if (!m_pullInlineBranch.isEmpty()) {
        QString error;
        if (!finishBranchInlineComment(&error))
            QMessageBox::warning(this, QStringLiteral("Comment"), error);
        return;
    }
    const int number = m_currentPullNumber;
    const int first = qMin(m_pullInlineStartLine, m_pullInlineEndLine);
    const int last = qMax(m_pullInlineStartLine, m_pullInlineEndLine);
    QString error;
    if (!postPullInlineComment(number, m_pullInlinePath, m_pullInlineSide,
                               first, last, body,
                               currentPullAnchor(number, m_pullInlinePath,
                                                 m_pullInlineSide, first, last),
                               &error)) {
        QMessageBox::warning(this, QStringLiteral("Comment"), error);
        return;
    }
    cancelPullInlineComposer();
    reloadPulls();
    showPull(number);
    if (m_pullInlineView == m_branchDiffView)
        rerenderPullInlineDiff();
}

bool MainWindow::branchDiffTakesBranchComments() const
{
    return m_branchDiffView && m_branchDiffPullNumber < 0 &&
           !m_branchDiffBranch.isEmpty() &&
           m_branchDiffBranch != branchCompareBase();
}

const QList<PullEvent> &MainWindow::branchReviewEvents(const QString &branch)
{
    const QString key = repoGitDir() + QLatin1Char('\n') + branch;
    if (key != m_branchReviewEventsKey) {
        m_branchReviewEventsKey = key;
        m_branchReviewEvents =
            pullStoreForCurrentRepo().readBranchReviewEvents(branch);
    }
    return m_branchReviewEvents;
}

PullRequest MainWindow::branchReviewPull(const QString &patch)
{
    PullRequest pr;
    pr.number = 0;
    pr.status = QStringLiteral("branch");
    pr.head = m_branchDiffBranch;
    pr.base = branchCompareBase();
    pr.patch = patch;
    pr.events = branchReviewEvents(m_branchDiffBranch);
    return pr;
}

void MainWindow::reloadBranchReview()
{
    m_branchReviewEventsKey.clear();
    if (!m_branchDiffView)
        return;
    if (QScrollBar *bar = m_branchDiffView->verticalScrollBar())
        m_diffRestoreScroll.insert(m_branchDiffView, bar->value());
    if (m_branchDiffLastValid)
        renderBranchDiffPatch(QString::fromUtf8(m_branchDiffLastPatch),
                              m_branchDiffLastEmpty, m_branchDiffViewedContext);
    else
        renderBranchScopeDiff();
}

bool MainWindow::postBranchInlineComment(QString *error)
{
    const QString branch = m_pullInlineBranch;
    const QString body = m_pullInlineCommentEdit
                             ? m_pullInlineCommentEdit->toPlainText().trimmed()
                             : QString();
    const int first = qMin(m_pullInlineStartLine, m_pullInlineEndLine);
    const int last = qMax(m_pullInlineStartLine, m_pullInlineEndLine);
    if (branch.isEmpty() || m_pullInlinePath.isEmpty() || first <= 0 ||
        body.isEmpty()) {
        if (error)
            *error = QStringLiteral("The inline comment is incomplete.");
        return false;
    }
    PullRequest pr = branchReviewPull(
        m_branchDiffLastValid && m_branchDiffBranch == branch
            ? QString::fromUtf8(m_branchDiffLastPatch)
            : QString());
    QByteArray head;
    const QString dir = m_branchDiffWorkDir.isEmpty() ? repoGitDir()
                                                       : m_branchDiffWorkDir;
    if (runGitCapture(dir,
                      {QStringLiteral("rev-parse"), QStringLiteral("--verify"),
                       QStringLiteral("-q"), branch + QStringLiteral("^{commit}")},
                      &head, nullptr))
        pr.patchHeadOid = QString::fromUtf8(head).trimmed();
    return pullStoreForCurrentRepo().addBranchThreadComment(
        branch, m_pullInlinePath, m_pullInlineSide, first, last, body,
        pullAnchorFor(pr, m_pullInlinePath, m_pullInlineSide, first, last),
        error);
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testBranchDiffDragComment(const QString &path, int fromLine,
                                              int toLine)
{
    if (!m_branchDiffView)
        return QString();
    QTextBrowser *view = m_branchDiffView;
    const auto pointFor = [view, &path](int line) {
        QTextDocument *doc = view->document();
        for (QTextBlock block = doc->begin(); block.isValid();
             block = block.next()) {
            for (auto it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                QString foundPath, foundSide;
                int foundLine = 0;
                if (!fragment.isValid() || !fragment.charFormat().isAnchor() ||
                    !parsePullLineAnchor(fragment.charFormat().anchorHref(),
                                         &foundPath, &foundSide, &foundLine) ||
                    foundPath != path || foundSide != QLatin1String("new") ||
                    foundLine != line)
                    continue;
                QTextCursor cursor(doc);
                cursor.setPosition(fragment.position());
                QRect rect = view->cursorRect(cursor);
                QScrollBar *bar = view->verticalScrollBar();
                if (bar && (rect.top() < 0 ||
                            rect.bottom() > view->viewport()->height())) {
                    bar->setValue(bar->value() + rect.top() - 40);
                    rect = view->cursorRect(cursor);
                }
                return QPoint(rect.left() + 3, rect.center().y());
            }
        }
        return QPoint(-1, -1);
    };
    const QPoint from = pointFor(fromLine);
    if (from.x() < 0)
        return QString();
    QWidget *viewport = view->viewport();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(from),
                      viewport->mapToGlobal(QPointF(from)), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &press);
    const QPoint to = pointFor(toLine);
    if (to.x() >= 0) {
        const QPoint across(to.x() + 200, to.y());
        QMouseEvent move(QEvent::MouseMove, QPointF(across),
                         viewport->mapToGlobal(QPointF(across)), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport, &move);
    }
    const QPoint end = to.x() >= 0 ? to : from;
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(end),
                        viewport->mapToGlobal(QPointF(end)), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport, &release);
    return m_pullInlineComposerTitle && !m_pullInlinePath.isEmpty()
               ? m_pullInlineComposerTitle->text()
               : QString();
}

bool MainWindow::testSubmitBranchInlineComment(const QString &body,
                                               QString *error)
{
    if (!m_pullInlineCommentEdit || m_pullInlineBranch.isEmpty())
        return false;
    m_pullInlineCommentEdit->setPlainText(body);
    return finishBranchInlineComment(error);
}

QList<PullEvent> MainWindow::testBranchReviewEvents(const QString &branch) const
{
    return pullStoreForCurrentRepo().readBranchReviewEvents(branch);
}
#endif

bool MainWindow::finishBranchInlineComment(QString *error)
{
    if (!postBranchInlineComment(error))
        return false;
    m_pullOpenLineNotes.insert(
        pullLineNoteKey(m_pullInlinePath, m_pullInlineSide,
                        qMin(m_pullInlineStartLine, m_pullInlineEndLine)));
    m_branchReviewEventsKey.clear();
    cancelPullInlineComposer();
    return true;
}

void MainWindow::onBranchReviewAnchorClicked(const QUrl &url)
{
    const QString branch = m_branchDiffBranch;
    if (branch.isEmpty())
        return;
    const QString href = QUrl::fromPercentEncoding(url.toEncoded());
    if (url.scheme() == QLatin1String("cmt") ||
        url.scheme() == QLatin1String("threadtoggle")) {
        const QUrlQuery query(url);
        const QString path = url.path();
        const QString side = query.queryItemValue(QStringLiteral("s"));
        const int line = query.queryItemValue(QStringLiteral("l")).toInt();
        if (path.isEmpty() || side.isEmpty() || line <= 0)
            return;
        if (url.scheme() == QLatin1String("threadtoggle")) {
            const QString key = pullLineNoteKey(path, side, line);
            if (m_pullOpenLineNotes.contains(key))
                m_pullOpenLineNotes.remove(key);
            else
                m_pullOpenLineNotes.insert(key);
            reloadBranchReview();
            return;
        }
        m_pullInlineView = m_branchDiffView;
        m_pullInlineBranch = branch;
        m_pullInlinePath = path;
        m_pullInlineSide = side;
        m_pullInlineStartLine = line;
        m_pullInlineEndLine = line;
        paintPullDiffLineSelection();
        openPullInlineComposer();
        return;
    }
    PullStore store = pullStoreForCurrentRepo();
    QString error;
    bool posted = false;
    if (href.startsWith(QLatin1String("thread:"))) {
        const QStringList parts = href.split(QLatin1Char(':'));
        if (parts.size() < 3)
            return;
        const QString action = parts.at(1);
        const QString threadId = parts.mid(2).join(QStringLiteral(":"));
        if (action == QLatin1String("reply")) {
            QString parentId;
            for (const PullEvent &ev : branchReviewEvents(branch))
                if (ev.threadId == threadId && !ev.id.isEmpty())
                    parentId = ev.id;
            bool ok = false;
            const QString body = QInputDialog::getMultiLineText(
                this, QStringLiteral("Reply to review thread"),
                QStringLiteral("Reply"), QString(), &ok);
            if (!ok || body.trimmed().isEmpty())
                return;
            posted = store.addBranchThreadReply(branch, threadId, parentId,
                                                body.trimmed(), &error);
        } else if (action == QLatin1String("resolve") ||
                   action == QLatin1String("unresolve")) {
            posted = store.setBranchThreadState(
                branch, threadId,
                action == QLatin1String("resolve") ? QStringLiteral("resolved")
                                                   : QStringLiteral("unresolved"),
                &error);
        } else {
            return;
        }
    } else if (url.scheme() == QLatin1String("filecomment")) {
        const QString path = url.path();
        bool ok = false;
        const QString body = QInputDialog::getMultiLineText(
            this, QStringLiteral("Comment on %1").arg(path),
            QStringLiteral("Comment"), QString(), &ok);
        if (path.isEmpty() || !ok || body.trimmed().isEmpty())
            return;
        posted = store.addBranchThreadComment(branch, path, QStringLiteral("new"),
                                              0, 0, body.trimmed(), PullAnchor(),
                                              &error);
    } else {
        return;
    }
    if (!posted) {
        QMessageBox::warning(this, QStringLiteral("Comment"), error);
        return;
    }
    reloadBranchReview();
}

void MainWindow::submitPullInlineReviewBatch()
{
    if (!m_pullInlineBranch.isEmpty()) {
        submitPullInlineCommentNow();
        return;
    }
    if (m_pullInlineCommentEdit && !m_pullInlinePath.isEmpty() &&
        !m_pullInlineCommentEdit->toPlainText().trimmed().isEmpty())
        queuePullInlineComment();
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        m_currentPullNumber < 0)
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString repoKey =
        (repo.owner + QLatin1Char('/') + repo.name).toLower();
    const int number = m_currentPullNumber;
    QList<PullInlineDraft> remaining;
    QStringList failures;
    for (const PullInlineDraft &draft : std::as_const(m_pullInlineDrafts)) {
        if (draft.repoKey != repoKey || draft.number != number) {
            remaining.append(draft);
            continue;
        }
        QString error;
        PullAnchor anchor;
        anchor.patchset = draft.patchset;
        anchor.anchorLines = draft.anchorLines;
        if (!postPullInlineComment(number, draft.path, draft.side,
                                   draft.lineStart, draft.lineEnd, draft.body,
                                   anchor, &error)) {
            remaining.append(draft);
            failures.append(error);
        }
    }
    m_pullInlineDrafts = remaining;
    updatePullInlineReviewBatchBar();
    reloadPulls();
    showPull(number);
    if (m_pullInlineView == m_branchDiffView)
        rerenderPullInlineDiff();
    if (!failures.isEmpty())
        QMessageBox::warning(this, QStringLiteral("Submit review"),
                             failures.join(QLatin1Char('\n')));
}

void MainWindow::discardPullInlineReviewBatch()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString repoKey =
        (repo.owner + QLatin1Char('/') + repo.name).toLower();
    QList<PullInlineDraft> kept;
    for (const PullInlineDraft &draft : std::as_const(m_pullInlineDrafts))
        if (draft.repoKey != repoKey || draft.number != m_currentPullNumber)
            kept.append(draft);
    m_pullInlineDrafts = kept;
    m_pullDiffSourceKey.clear();
    m_pullDiffRenderKey.clear();
    rerenderPullInlineDiff();
    updatePullInlineReviewBatchBar();
}

void MainWindow::updatePullInlineReviewBatchBar()
{
    if (!m_pullInlineBatchBar)
        return;
    QString repoKey;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        repoKey = (repo.owner + QLatin1Char('/') + repo.name).toLower();
    }
    int count = 0;
    for (const PullInlineDraft &draft : std::as_const(m_pullInlineDrafts))
        if (draft.repoKey == repoKey && draft.number == m_currentPullNumber)
            ++count;
    m_pullInlineBatchBar->setVisible(count > 0);
    if (m_pullInlineBatchLabel)
        m_pullInlineBatchLabel->setText(
            QStringLiteral("%1 draft comment%2 ready to submit")
                .arg(count)
                .arg(count == 1 ? QString() : QStringLiteral("s")));
    if (m_pullInlineSubmitBatchButton) {
        m_pullInlineSubmitBatchButton->setText(
            count > 0 ? QStringLiteral("Submit review (%1)").arg(count)
                      : QStringLiteral("Submit review"));
    }
}

void MainWindow::onPullDiffAnchorClicked(const QUrl &url)
{
    const QString href = QUrl::fromPercentEncoding(url.toEncoded());
    if ((url.scheme() == QLatin1String("cmt") ||
         url.scheme() == QLatin1String("threadtoggle")) &&
        sender() == m_pullDiff)
        m_pullInlineView = m_pullDiff;
    if (href.startsWith(QLatin1String("thread:"))) {
        const QStringList parts = href.split(QLatin1Char(':'));
        if (parts.size() < 3)
            return;
        const QString action = parts.at(1);
        const QString threadId = parts.mid(2).join(QStringLiteral(":"));
        if (action == QLatin1String("reply"))
            submitPullThreadReply(threadId);
        else if (action == QLatin1String("resolve"))
            setPullThreadState(threadId, QStringLiteral("resolved"));
        else if (action == QLatin1String("unresolve"))
            setPullThreadState(threadId, QStringLiteral("unresolved"));
        else if (action == QLatin1String("applyfix"))
            applyPullSuggestionFix(threadId);
        return;
    }
    if (url.scheme() == QLatin1String("threadtoggle")) {
        const QUrlQuery query(url);
        const QString path = url.path();
        const QString side = query.queryItemValue(QStringLiteral("s"));
        const int line = query.queryItemValue(QStringLiteral("l")).toInt();
        if (path.isEmpty() || side.isEmpty() || line <= 0)
            return;
        const QString key = pullLineNoteKey(path, side, line);
        if (m_pullOpenLineNotes.contains(key))
            m_pullOpenLineNotes.remove(key);
        else
            m_pullOpenLineNotes.insert(key);
        if (QScrollBar *bar = m_pullDiff->verticalScrollBar())
            m_diffRestoreScroll.insert(m_pullDiff, bar->value());
        m_pullDiffSourceKey.clear();
        m_pullDiffRenderKey.clear();
        renderPullDiff();
        return;
    }
    if (url.scheme() == QLatin1String("viewed") ||
        url.scheme() == QLatin1String("expand")) {
        const QString path = url.path();
        const QString context =
            QStringLiteral("pull/") + QString::number(m_currentPullNumber);
        if (!toggleDiffExpandedFromAnchor(url, context)) {
            const QSet<QString> cur = loadDiffViewed(context);
            setDiffViewed(context, path, !cur.contains(path));
        }
        if (swapPullDiffFileInPlace(path))
            return; // the rows above it never moved; leave the viewport alone
        renderPullDiff();
        scrollPullDiffToFile(path);
        return;
    }
    if (url.scheme() == QLatin1String("filecomment")) {
        if (m_currentPullNumber < 0)
            return;
        const QString path = url.path();
        bool ok = false;
        const QString body = QInputDialog::getMultiLineText(
            this, QStringLiteral("Comment on %1").arg(path),
            QStringLiteral("Comment"), QString(), &ok);
        if (!ok || body.trimmed().isEmpty())
            return;
        PullStore store = pullStoreForCurrentRepo();
        if (store.canWrite()) {
            QString error;
            if (!store.addLineComment(m_currentPullNumber, path,
                                      QStringLiteral("new"), 0, body.trimmed(),
                                      &error)) {
                QMessageBox::warning(this, "Comment", error);
                return;
            }
        } else {
            PullEvent ev;
            ev.type = QStringLiteral("line-comment");
            ev.path = path;
            ev.side = QStringLiteral("new");
            ev.line = 0;
            ev.body = body.trimmed();
            ev = store.makeSignedEvent(m_currentPullNumber, ev);
            submitPullEventToInbox(m_currentPullNumber, ev);
        }
        reloadPulls();
        showPull(m_currentPullNumber);
        return;
    }
    if (url.scheme() != QLatin1String("cmt"))
        return;
    const QString filePath = url.path();
    const QUrlQuery cmtQuery(url);
    const QString side = cmtQuery.queryItemValue(QStringLiteral("s"));
    const int line = cmtQuery.queryItemValue(QStringLiteral("l")).toInt();
    if (m_currentPullNumber < 0 || filePath.isEmpty() || line <= 0)
        return;

    m_pullInlinePath = filePath;
    m_pullInlineSide = side;
    m_pullInlineStartLine = line;
    m_pullInlineEndLine = line;
    paintPullDiffLineSelection();
    openPullInlineComposer();
}

// code-health: allow(deep-nested-loop) each level is a find-first that breaks on its match: the current pull, then the thread, then a backward scan for its last event id
void MainWindow::submitPullThreadReply(const QString &threadId)
{
    if (m_currentPullNumber < 0 || threadId.isEmpty())
        return;
    QString parentId;
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        if (pr.number != m_currentPullNumber)
            continue;
        const PullReviewSnapshot snapshot = buildPullReviewSnapshot(pr);
        for (const PullReviewThread &thread : snapshot.threads) {
            if (thread.id != threadId)
                continue;
            for (int i = thread.events.size() - 1; i >= 0; --i) {
                if (!thread.events.at(i).id.isEmpty()) {
                    parentId = thread.events.at(i).id;
                    break;
                }
            }
            break;
        }
        break;
    }

    bool ok = false;
    const QString body = QInputDialog::getMultiLineText(
        this, QStringLiteral("Reply to review thread"),
        QStringLiteral("Reply"), QString(), &ok);
    if (!ok || body.trimmed().isEmpty())
        return;

    PullStore store = pullStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        if (!store.addThreadReply(m_currentPullNumber, threadId, parentId,
                                  body.trimmed(), &error)) {
            QMessageBox::warning(this, "Reply", error);
            return;
        }
    } else {
        PullEvent ev;
        ev.type = QStringLiteral("thread-reply");
        ev.threadId = threadId;
        ev.parentId = parentId;
        ev.body = body.trimmed();
        ev = store.makeSignedEvent(m_currentPullNumber, ev);
        submitPullEventToInbox(m_currentPullNumber, ev);
    }
    reloadPulls();
    showPull(m_currentPullNumber);
}

void MainWindow::setPullThreadState(const QString &threadId, const QString &state)
{
    if (m_currentPullNumber < 0 || threadId.isEmpty() || state.isEmpty())
        return;
    PullStore store = pullStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        if (!store.setThreadState(m_currentPullNumber, threadId, state,
                                  QString(), &error)) {
            QMessageBox::warning(this, "Thread", error);
            return;
        }
    } else {
        PullEvent ev;
        ev.type = QStringLiteral("thread-state");
        ev.threadId = threadId;
        ev.state = state;
        ev = store.makeSignedEvent(m_currentPullNumber, ev);
        submitPullEventToInbox(m_currentPullNumber, ev);
    }
    reloadPulls();
    showPull(m_currentPullNumber);
}

void MainWindow::addConversationCard(QVBoxLayout *layout, const QString &author,
                                     const QString &headerHtml, const QString &body,
                                     const QString &accent, const QString &copyLink,
                                     const QString &authorId,
                                     const std::function<void()> &onDelete,
                                     const QList<QPair<QString, std::function<void()>>>
                                         &extraActions)
{
    if (!layout)
        return;
    const QString who = author.isEmpty() ? QStringLiteral("?") : author;
    auto *row = new QWidget;
    row->setObjectName("issueTimelineRow");
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(kPullThreadGutterPx);
    auto *avatar = new QLabel(who.left(2).toUpper());
    avatar->setObjectName("issueAvatar");
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(kPullThreadAvatarPx, kPullThreadAvatarPx);
    QPixmap authorAvatar;
    if (!authorId.isEmpty()) {
        const QPixmap cached = m_avatars.value(authorId);
        if (!cached.isNull())
            authorAvatar = roundedAvatar(cached, kPullThreadAvatarPx, 0.5);
        else if (authorId == m_profileIdentity.publicKey())
            authorAvatar =
                roundedAvatar(effectiveUserAvatar(), kPullThreadAvatarPx, 0.5);
    }
    if (authorAvatar.isNull()) {
        const QString seed = authorId.isEmpty() ? who.toLower() : authorId;
        authorAvatar =
            roundedAvatar(forkMeshAvatarPng(seed), kPullThreadAvatarPx, 0.5);
    }
    if (!authorAvatar.isNull()) {
        avatar->setText(QString());
        avatar->setPixmap(authorAvatar);
    }
    rowLayout->addWidget(avatar, 0, Qt::AlignTop);

    auto *card = new QWidget;
    card->setObjectName("issueTimelineCard");
    if (!accent.isEmpty() && Theme::edgeIndicators())
        card->setStyleSheet(Theme::remapSheet(QStringLiteral("#issueTimelineCard { border-left:3px solid %1; }")
                                .arg(accent)));
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    auto *headerBox = new QWidget(card);
    headerBox->setObjectName("issueTimelineHeader");
    auto *headerRow = new QHBoxLayout(headerBox);
    headerRow->setContentsMargins(16, 8, 10, 8);
    headerRow->setSpacing(8);
    auto *header = new QLabel(headerHtml);
    header->setTextFormat(Qt::RichText);
    headerRow->addWidget(header);
    headerRow->addStretch();
    if (!copyLink.isEmpty() || !body.trimmed().isEmpty() || bool(onDelete) ||
        !extraActions.isEmpty()) {
        auto *menu = new QMenu(card);
        for (const auto &extra : extraActions) {
            QAction *extraAction = menu->addAction(extra.first);
            const std::function<void()> run = extra.second;
            connect(extraAction, &QAction::triggered, this, [run]() { run(); });
        }
        if (!extraActions.isEmpty())
            menu->addSeparator();
        if (!copyLink.isEmpty()) {
            QAction *copyLinkAction = menu->addAction("Copy link");
            connect(copyLinkAction, &QAction::triggered, this, [this, copyLink]() {
                QApplication::clipboard()->setText(copyLink);
                flashMessage("Link copied.");
            });
        }
        if (!body.trimmed().isEmpty()) {
            QAction *copyMarkdownAction = menu->addAction("Copy Markdown");
            connect(copyMarkdownAction, &QAction::triggered, this, [this, body]() {
                QApplication::clipboard()->setText(body);
                flashMessage("Markdown copied.");
            });
        }
        if (onDelete) {
            menu->addSeparator();
            QAction *deleteAction = menu->addAction("Delete comment");
            connect(deleteAction, &QAction::triggered, this,
                    [onDelete]() { onDelete(); });
        }
        auto *actionsButton = new QToolButton(headerBox);
        actionsButton->setObjectName("issueActionButton");
        actionsButton->setText("...");
        actionsButton->setCursor(Qt::PointingHandCursor);
        actionsButton->setPopupMode(QToolButton::InstantPopup);
        actionsButton->setMenu(menu);
        headerRow->addWidget(actionsButton);
    }
    cardLayout->addWidget(headerBox);

    if (!body.trimmed().isEmpty()) {
        auto *bodyLabel = new QLabel;
        bodyLabel->setTextFormat(Qt::MarkdownText);
        bodyLabel->setText(autolinkReferencesWithStatus(body));
        bodyLabel->setWordWrap(true);
        bodyLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
        bodyLabel->setOpenExternalLinks(false);
        connect(bodyLabel, &QLabel::linkActivated, this,
                [this](const QString &href) {
                    if (href.startsWith(QLatin1String("applyfix:")))
                        applyPullSuggestionFix(href.mid(9));
                    else
                        openBodyReference(href);
                });
        bodyLabel->setContentsMargins(16, 12, 16, 14);
        cardLayout->addWidget(bodyLabel);
    }
    rowLayout->addWidget(card, 1);
    layout->insertWidget(layout->count() - 1, row);
}

void MainWindow::renderPullThread(const PullRequest &pr)
{
    if (!m_pullThreadLayout)
        return;
    while (QLayoutItem *item = m_pullThreadLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            // deleteLater() is not serviced while showPull() is inside one of
            // its nested Git event loops. Hide the retired row immediately so
            // it cannot remain painted beneath a newly inserted card during a
            // slow refresh.
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
    m_pullThreadLayout->addStretch();
    if (pr.number == 0) {
        if (m_pullLinksValue)
            m_pullLinksValue->hide();
        m_pullActivityExtraCards = 0;
        m_pullActivityCardsNumber = 0;
        m_pullAgentActivityDigest.clear();
        return;
    }

    if (pr.status == QLatin1String("open") && pr.ts > 0 &&
        QDateTime::currentMSecsSinceEpoch() - pr.ts < 24LL * 3600 * 1000) {
        PullCertificateData certificate;
        certificate.number = pr.number;
        certificate.title = pr.title;
        certificate.head = pr.head;
        certificate.base = pr.base;
        certificate.author =
            pr.authorName.isEmpty() ? pr.author.left(10) : pr.authorName;
        certificate.openedSecs = pr.ts / 1000;
        certificate.files = pr.filesChanged;
        certificate.additions = pr.additions;
        certificate.deletions = pr.deletions;
        const QPixmap cached = m_avatars.value(pr.author);
        if (!cached.isNull())
            certificate.avatar = roundedAvatar(cached, 56, 0.5);
        else if (!pr.author.isEmpty() && pr.author == m_profileIdentity.publicKey())
            certificate.avatar = roundedAvatar(effectiveUserAvatar(), 56, 0.5);
        if (certificate.avatar.isNull())
            certificate.avatar = roundedAvatar(
                forkMeshAvatarPng(pr.author.isEmpty() ? certificate.author.toLower()
                                                      : pr.author),
                56, 0.5);
        QList<qint64> opened;
        for (const PullRequest &pull : std::as_const(m_currentPulls))
            if (pull.ts > 0)
                opened << pull.ts / 1000;
        certificate.velocity =
            mergeVelocityForTimestamps(opened, QDateTime::currentSecsSinceEpoch());
        auto *label = new QLabel(pullCertificateHtml(certificate));
        label->setObjectName(QStringLiteral("pullCertificate"));
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        m_pullThreadLayout->insertWidget(0, label);
    }

    if (m_pullLinksValue) {
        const QList<int> issues = issuesLinkedFromPull(pr);
        if (issues.isEmpty()) {
            m_pullLinksValue->hide();
        } else {
            QStringList links;
            for (const int n : issues)
                links << QStringLiteral(
                             "<a href='issue:%1' style='color:#58a6ff;"
                             "text-decoration:none'>issue #%1</a>")
                             .arg(n);
            m_pullLinksValue->setText(
                QString::fromUtf8("<b>Linked issues</b> \xC2\xB7 %1")
                    .arg(links.join(QString::fromUtf8(" \xC2\xB7 "))));
            m_pullLinksValue->show();
        }
    }

    // The conversation is this pull's whole activity feed, not only its signed
    // review events: the commits behind it and any agent working its branch are
    // interleaved in time order, so a branch an agent is still pushing to reads
    // as one story. Cards are collected first and emitted after sorting, because
    // commits and agent transitions interleave with comments rather than
    // following them.
    struct ThreadEntry {
        qint64 ts = 0;
        int order = 0; // stable tie-break for entries sharing a timestamp
        std::function<void()> add;
    };
    QList<ThreadEntry> entries;
    int entryOrder = 0;
    const auto headerToken = [](const QString &text) {
        constexpr int limit = 24;
        const QString trimmed = text.trimmed();
        return (trimmed.size() <= limit ? trimmed
                                        : trimmed.left(limit - 1) + QChar(0x2026))
            .toHtmlEscaped();
    };
    int extraCards = 0;
    m_pullActivityExtraCards = 0;
    m_pullActivityCardsNumber = pr.number;

    const QString opener = pr.authorName.isEmpty() ? pr.author.left(10) : pr.authorName;
    QString linkOwner = QStringLiteral("repo");
    QString linkRepo = QStringLiteral("pull");
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        linkOwner = m_repositories.at(m_repoDetailIndex).owner;
        linkRepo = m_repositories.at(m_repoDetailIndex).name;
    }
    const QString pullLink =
        QStringLiteral("forkmesh://pull/%1/%2/%3").arg(linkOwner, linkRepo).arg(pr.number);
    entries.append(
        {pr.ts, entryOrder++, [this, opener, pr, pullLink] {
             addConversationCard(
                 m_pullThreadLayout, opener,
                 QStringLiteral("<b>%1</b> <span style='color:#8b949e'>opened this "
                                "pull request %2</span>")
                     .arg(opener.toHtmlEscaped(), formatIssueRelativeTime(pr.ts)),
                 pr.description, QString(), pullLink + QStringLiteral("#open"),
                 pr.author);
         }});

    if (m_pullActivityCommitsNumber == pr.number) {
        for (const PullActivityCommit &commit : std::as_const(m_pullActivityCommits)) {
            const QString who =
                commit.author.isEmpty() ? QStringLiteral("unknown") : commit.author;
            const QString shortSha = commit.sha.left(12);
            QString verb = QStringLiteral("committed");
            if (!shortSha.isEmpty())
                verb += QStringLiteral(" <code>%1</code>").arg(shortSha.toHtmlEscaped());
            if (!commit.agentTrailer.isEmpty())
                verb += QStringLiteral(" <span style='color:%2'>as %1</span>")
                            .arg(headerToken(commit.agentTrailer),
                                 Theme::nextOrHex(Theme::Role::FgSecondary,
                                                  QStringLiteral("#a371f7")));
            const QString when = commit.committedSecs > 0
                                     ? formatIssueRelativeTime(commit.committedSecs * 1000)
                                     : commit.when;
            entries.append(
                {commit.committedSecs * 1000, entryOrder++,
                 [this, who, verb, when, commit, pullLink] {
                     addConversationCard(
                         m_pullThreadLayout, who,
                         QStringLiteral("<b>%1</b> %2 <span style='color:#8b949e'>%3"
                                        "</span>")
                             .arg(who.toHtmlEscaped(), verb, when),
                         commit.subject, QStringLiteral("#58a6ff"),
                         pullLink + QStringLiteral("#commit-%1").arg(commit.sha),
                         QString());
                 }});
            ++extraCards;
        }
    }

    QSet<QString> applicableFixes;
    QHash<QString, QString> threadPlacements; // thread id -> badge text
    {
        const PullReviewSnapshot snapshot = buildPullReviewSnapshot(pr);
        const bool canFix = pr.status == QLatin1String("open") &&
                            pullStoreForCurrentRepo().canWrite();
        for (const PullReviewThread &thread : snapshot.threads) {
            if (canFix && !thread.resolved && thread.hasSuggestion &&
                thread.suggestionState != QLatin1String("applied") &&
                !thread.outdated())
                applicableFixes.insert(thread.id);
            const QString label = pullThreadPlacementLabel(thread);
            if (!label.isEmpty())
                threadPlacements.insert(thread.id, label);
        }
    }

    for (const PullEvent &ev : pr.events) {
        const QString who = ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
        const QString when = formatIssueRelativeTime(ev.ts);
        QString verb = QStringLiteral("commented");
        QString accent;
        QString body = ev.body;
        if (ev.type == QLatin1String("line-comment")) {
            verb = QStringLiteral("commented on <code>%1:%2</code>")
                       .arg(ev.path.toHtmlEscaped())
                       .arg(ev.line);
        } else if (ev.type == QLatin1String("thread-comment")) {
            verb = QStringLiteral("started a review thread on <code>%1:%2</code>")
                       .arg(ev.path.toHtmlEscaped())
                       .arg(ev.lineStart);
            const QString placement = threadPlacements.value(ev.threadId);
            if (!placement.isEmpty())
                verb += QStringLiteral(" <span style='color:#8b949e'>(%1)</span>")
                            .arg(placement.toHtmlEscaped());
            accent = ev.suggestionPatch.isEmpty() ? QStringLiteral("#d29922")
                                                  : QStringLiteral("#58a6ff");
            if (!ev.suggestionPatch.isEmpty()) {
                body += QStringLiteral("\n\n```diff\n%1\n```").arg(ev.suggestionPatch);
                if (applicableFixes.contains(ev.threadId))
                    body += QString::fromUtf8(
                                "\n\n[\xE2\x9A\xA1 Apply fix & commit](applyfix:%1)")
                                .arg(ev.threadId);
            }
        } else if (ev.type == QLatin1String("thread-reply")) {
            verb = QStringLiteral("replied in a review thread");
        } else if (ev.type == QLatin1String("thread-state")) {
            if (ev.state == QLatin1String("resolved")) {
                verb = QStringLiteral("<span style='color:#3fb950'>resolved a "
                                      "review thread</span>");
                accent = QStringLiteral("#3fb950");
            } else {
                verb = QStringLiteral("reopened a review thread");
                accent = QStringLiteral("#d29922");
            }
        } else if (ev.type == QLatin1String("suggestion-state")) {
            verb = ev.state == QLatin1String("applied")
                       ? QStringLiteral("<span style='color:#3fb950'>applied a "
                                        "suggested change</span>")
                       : QStringLiteral("updated a suggested change");
            accent = QStringLiteral("#58a6ff");
        } else if (ev.type == QLatin1String("review")) {
            if (ev.state == QLatin1String("approved")) {
                verb = QStringLiteral("<span style='color:#3fb950'>approved these "
                                      "changes</span>");
                accent = QStringLiteral("#3fb950");
            } else if (ev.state == QLatin1String("changes_requested")) {
                verb = QStringLiteral("<span style='color:#f85149'>requested "
                                      "changes</span>");
                accent = QStringLiteral("#f85149");
            } else {
                verb = QStringLiteral("reviewed");
            }
        }
        const QString anchor =
            pullLink + QStringLiteral("#%1")
                           .arg(ev.id.isEmpty() ? QString::number(ev.ts) : ev.id);
        entries.append(
            {ev.ts, entryOrder++,
             [this, who, verb, when, body, accent, anchor, author = ev.author] {
                 addConversationCard(
                     m_pullThreadLayout, who,
                     QStringLiteral("<b>%1</b> %2 <span style='color:#8b949e'>%3"
                                    "</span>")
                         .arg(who.toHtmlEscaped(), verb, when),
                     body, accent, anchor, author);
             }});
    }

    if (const AgentSession *linked = agentSessionForPull(pr.number, pr.head)) {
        const AgentSession agent = *linked;
        const QString provider = agentProviderName(agent.provider);
        const QString branch =
            agent.branchName.isEmpty() ? pr.head : agent.branchName;
        const QString on = branch.isEmpty()
                               ? QString()
                               : QStringLiteral(" on <code>%1</code>")
                                     .arg(headerToken(branch));
        const auto agentCard = [&](qint64 ts, const QString &verb,
                                   const QString &accent, QString body) {
            if (ts <= 0)
                return;
            ++extraCards;
            if (!branch.isEmpty())
                body = QStringLiteral("Branch: %1%2")
                           .arg(branch, body.isEmpty()
                                            ? QString()
                                            : QStringLiteral("\n\n") + body);
            entries.append(
                {ts, entryOrder++,
                 [this, provider, verb, accent, body, ts, pullLink, agent] {
                     addConversationCard(
                         m_pullThreadLayout, provider,
                         QStringLiteral("<b>%1</b> %2 <span style='color:#8b949e'>%3"
                                        "</span>")
                             .arg(provider.toHtmlEscaped(), verb,
                                  formatIssueRelativeTime(ts)),
                         body, accent,
                         pullLink + QStringLiteral("#agent-%1").arg(agent.id),
                         QString());
                 }});
        };
        const QString queuedTone = agentStatusColor(AgentStatus::Queued).name();
        agentCard(agent.createdAtMs,
                  QStringLiteral("was queued%1").arg(on), queuedTone, QString());
        agentCard(agent.startedAtMs,
                  QStringLiteral("started working%1").arg(on),
                  agentStatusColor(AgentStatus::Running).name(), QString());
        if (agent.finishedAtMs > 0) {
            QString verb = QStringLiteral("finished (%1)")
                               .arg(agentStatusText(agent.status).toLower());
            if (agent.status == AgentStatus::Success)
                verb = QStringLiteral("finished successfully");
            else if (agent.status == AgentStatus::Failed)
                verb = QStringLiteral("failed");
            else if (agent.status == AgentStatus::Stopped)
                verb = QStringLiteral("was stopped");
            agentCard(agent.finishedAtMs, verb,
                      agentStatusColor(agent.status).name(), agent.lastError);
        } else if (agentSessionActive(&agent) ||
                   agent.status == AgentStatus::Waiting) {
            agentCard(QDateTime::currentMSecsSinceEpoch(),
                      QStringLiteral("is <b>%1</b>%2")
                          .arg(agentStatusText(agent.status).toLower(), on),
                      agentStatusColor(agent.status).name(), agent.lastError);
        }
    }

    std::stable_sort(entries.begin(), entries.end(),
                     [](const ThreadEntry &a, const ThreadEntry &b) {
                         return a.ts != b.ts ? a.ts < b.ts : a.order < b.order;
                     });
    m_pullActivityExtraCards = extraCards;
    m_pullAgentActivityDigest = pullAgentActivityDigest(pr);
    for (const ThreadEntry &entry : std::as_const(entries))
        entry.add();
    // Private repositories have no ActivityPub actor, so there is no remote
    // thread to show and the lookup would only leak the repo name in a URL.
    if (m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size() && m_networkAccess &&
        !m_repositories.at(m_repoDetailIndex).isPrivate) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        auto *remoteThread =
            new FederatedThreadView(m_networkAccess, m_pullThreadContainer);
        const QUrl server(canonicalServerUrl(
            m_activeServer >= 0 && m_activeServer < m_servers.size()
                ? m_servers.at(m_activeServer).url
                : QString()));
        remoteThread->load(server, repo.owner, repo.name,
                           QStringLiteral("pull"), pr.number);
        m_pullThreadLayout->insertWidget(qMax(0, m_pullThreadLayout->count() - 1),
                                         new ThreadCardRow(remoteThread));
    }
}

QString MainWindow::pullAgentActivityDigest(const PullRequest &pr) const
{
    const AgentSession *agent = agentSessionForPull(pr.number, pr.head);
    if (!agent)
        return QString();
    return QStringList{QString::number(agent->id),
                       agent->status,
                       agent->branchName,
                       agent->lastError,
                       QString::number(agent->createdAtMs),
                       QString::number(agent->startedAtMs),
                       QString::number(agent->finishedAtMs)}
        .join(QLatin1Char('\x1f'));
}

void MainWindow::refreshPullAgentActivity()
{
    if (!m_pullDetail || !m_pullDetail->isVisible() || m_currentPullNumber <= 0)
        return;
    PullRequest current;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == m_currentPullNumber)
            current = pr;
    if (current.number <= 0)
        return;
    const QString digest = pullAgentActivityDigest(current);
    if (digest == m_pullAgentActivityDigest || m_pullActivityRefreshing)
        return;
    const int offset = m_pullThreadScroll && m_pullThreadScroll->verticalScrollBar()
                           ? m_pullThreadScroll->verticalScrollBar()->value()
                           : 0;
    const QScopedValueRollback<bool> busy(m_pullActivityRefreshing, true);
    renderPullCommits(current);
    renderPullThread(current);
    if (m_pullThreadScroll && m_pullThreadScroll->verticalScrollBar())
        m_pullThreadScroll->verticalScrollBar()->setValue(offset);
    setPullActionBadge(m_pullTabConversation,
                       m_pullConversationBaseCount + m_pullActivityExtraCards);
}

void MainWindow::renderPullCommits(PullRequest pr)
{
    if (!m_pullCommitsList)
        return;
    // `pr` is taken by value: the runGitCapture() below pumps the event loop
    // (under a GitKeepAlive scope), and that pump can re-enter reload paths that
    // reassign m_currentPulls. pr.base/pr.head/pr.commits are read *after* the
    // pump, so a reference into m_currentPulls would dangle and those reads would
    // be use-after-frees — the same crash class that took runIdsForPull by value
    // in. The copy stays valid across any nested reload.
    m_pullCommitsList->clear();
    m_pullActivityCommits.clear();
    m_pullActivityCommitsNumber = pr.number;
    const QString dir = repoGitDir();
    bool listed = false;
    QString repoOwner, repoName;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        repoOwner = m_repositories.at(m_repoDetailIndex).owner;
        repoName = m_repositories.at(m_repoDetailIndex).name;
    }
    const auto checkStatusFor = [&](const QString &sha) -> QString {
        if (sha.isEmpty() || repoOwner.isEmpty())
            return QString();
        const auto rank = [](const QString &s) {
            if (s == ActionStatus::Failed || s == ActionStatus::Rejected ||
                s == ActionStatus::Cancelled || s == ActionStatus::Skipped)
                return 4;
            if (s == ActionStatus::Running)
                return 3;
            if (s == ActionStatus::Queued || s == ActionStatus::AwaitingApproval)
                return 2;
            if (s == ActionStatus::Success)
                return 1;
            return 0;
        };
        QString best;
        int bestRank = 0;
        for (const ActionRun &run : std::as_const(m_actionRuns)) {
            if (run.owner != repoOwner || run.name != repoName || run.commit != sha)
                continue;
            if (const int r = rank(run.status); r > bestRank) {
                bestRank = r;
                best = run.status;
            }
        }
        return best;
    };
    const auto checkGlyph = [](const QString &status) -> QString {
        if (status == ActionStatus::Success)
            return QString::fromUtf8("\xE2\x9C\x93 "); // check mark
        if (status == ActionStatus::Failed || status == ActionStatus::Rejected ||
            status == ActionStatus::Cancelled || status == ActionStatus::Skipped)
            return QString::fromUtf8("\xE2\x9C\x97 "); // ballot X
        if (status == ActionStatus::Running)
            return QString::fromUtf8("\xE2\x97\x8F "); // filled circle
        if (status == ActionStatus::Queued || status == ActionStatus::AwaitingApproval)
            return QString::fromUtf8("\xE2\x97\x8B "); // hollow circle
        return QString();
    };
    const QString reviewHead = resolvablePullHead(pr);
    if (!dir.isEmpty() && !pr.base.isEmpty() && !reviewHead.isEmpty()) {
        PullRangeSnapshot range;
        if (pullRangeSnapshot(pr.base, reviewHead, &range) &&
            !range.detailedLog.trimmed().isEmpty()) {
            for (const QString &line : QString::fromUtf8(range.detailedLog)
                                           .split('\n', Qt::SkipEmptyParts)) {
                const QStringList f = line.split(QLatin1Char('\x1f'));
                if (f.size() < 6)
                    continue;
                const QString &sha = f.at(0);
                const QString rel = formatShortRelativeTime(f.at(5).toLongLong());
                const QString status = checkStatusFor(sha);
                QString text = checkGlyph(status);
                text += QString::fromUtf8("%1  %2 \xC2\xB7 %3 \xC2\xB7 %4")
                            .arg(f.at(1), f.at(2), f.at(3), f.at(4));
                if (!rel.isEmpty())
                    text += QString::fromUtf8(" \xC2\xB7 %1 ago").arg(rel);
                auto *item = new QListWidgetItem(text);
                item->setData(kCommitShaRole, sha);
                item->setData(kCommitMessageRole, f.at(2));
                QString tip = QString::fromUtf8("%1\n%2 committed %3")
                                  .arg(sha, f.at(3), f.at(4));
                if (!rel.isEmpty())
                    tip += QString::fromUtf8(" (%1 ago)").arg(rel);
                if (!status.isEmpty())
                    tip += QString::fromUtf8("\nChecks: %1").arg(actionStatusText(status));
                const QString agentTrailer = f.size() > 6 ? f.at(6).trimmed() : QString();
                if (!agentTrailer.isEmpty()) {
                    item->setIcon(themedOcticon(
                        "person",
                        Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")), 14));
                    tip += QString::fromUtf8("\nAgent-authored: %1").arg(agentTrailer);
                }
                item->setToolTip(tip);
                m_pullCommitsList->addItem(item);
                m_pullActivityCommits.append(
                    {sha, f.at(2), f.at(3), f.at(4), f.at(5).toLongLong(),
                     agentTrailer});
                listed = true;
            }
        }
    }
    // Cross-node fallback: the head ref isn't present on this node (the log above
    // found nothing), but the signed format-patch mbox carries every commit with
    // its original author/date/subject — parse those so attribution still shows.
    if (!listed && !pr.commits.isEmpty()) {
        static const QRegularExpression boundary(
            QStringLiteral("^From ([0-9a-f]{7,40}) "));
        static const QRegularExpression patchTag(
            QStringLiteral("^\\[PATCH[^\\]]*\\]\\s*"));
        QString author, subject, date, sha, agentTrailer;
        bool inHeaders = false;
        const auto flush = [&] {
            if (subject.isEmpty() && author.isEmpty())
                return;
            // The mbox Date: header is RFC 2822; reformat to a compact date+time
            // so the row carries it like the local-log path above,
            // falling back to the raw header if it doesn't parse.
            QString when = date;
            qint64 committedSecs = 0;
            const QDateTime dt = QDateTime::fromString(date, Qt::RFC2822Date);
            if (dt.isValid()) {
                when = dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
                committedSecs = dt.toSecsSinceEpoch();
            }
            const QString rel =
                committedSecs > 0 ? formatShortRelativeTime(committedSecs) : QString();
            const QString subj =
                subject.isEmpty() ? QStringLiteral("(no subject)") : subject;
            const QString auth = author.isEmpty() ? QStringLiteral("unknown") : author;
            const QString status = checkStatusFor(sha);
            QString text = checkGlyph(status);
            text += subj + QString::fromUtf8(" \xC2\xB7 ") + auth;
            if (!when.isEmpty())
                text += QString::fromUtf8(" \xC2\xB7 ") + when;
            if (!rel.isEmpty())
                text += QString::fromUtf8(" \xC2\xB7 %1 ago").arg(rel);
            auto *item = new QListWidgetItem(text);
            // The commit isn't on this node, so the row can't open it — but the
            // signed mbox still carries its SHA, so right-click can copy it.
            if (!sha.isEmpty())
                item->setData(kCommitCopyShaRole, sha);
            item->setData(kCommitMessageRole, subj);
            QString tip = sha.isEmpty() ? QString() : sha + QLatin1Char('\n');
            tip += QString::fromUtf8("%1 committed %2")
                       .arg(auth, when.isEmpty() ? date : when);
            if (!rel.isEmpty())
                tip += QString::fromUtf8(" (%1 ago)").arg(rel);
            if (!status.isEmpty())
                tip += QString::fromUtf8("\nChecks: %1").arg(actionStatusText(status));
            if (!agentTrailer.isEmpty()) {
                item->setIcon(themedOcticon(
                    "person",
                    Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")), 14));
                tip += QString::fromUtf8("\nAgent-authored: %1").arg(agentTrailer);
            }
            item->setToolTip(tip);
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
            m_pullCommitsList->addItem(item);
            m_pullActivityCommits.append(
                {sha, subj, auth, when, committedSecs, agentTrailer});
            listed = true;
            author.clear();
            subject.clear();
            date.clear();
            sha.clear();
            agentTrailer.clear();
        };
        for (const QString &line : pr.commits.split('\n')) {
            if (const QRegularExpressionMatch m = boundary.match(line);
                m.hasMatch()) {
                flush();
                sha = m.captured(1);
                inHeaders = true;
                continue;
            }
            if (!inHeaders) {
                if (line.startsWith(QLatin1String("ForkMesh-Agent:")))
                    agentTrailer = line.mid(15).trimmed();
                continue;
            }
            if (line.isEmpty()) { // blank line ends the header block
                inHeaders = false;
            } else if (line.startsWith(QLatin1String("From: "))) {
                author = line.mid(6).section(QLatin1String(" <"), 0, 0).trimmed();
            } else if (line.startsWith(QLatin1String("Date: "))) {
                date = line.mid(6).trimmed();
            } else if (line.startsWith(QLatin1String("Subject: "))) {
                subject = line.mid(9).trimmed();
                subject.remove(patchTag);
            }
        }
        flush();
    }
    if (!listed) {
        auto *item = new QListWidgetItem(
            QString::fromUtf8("%1 file(s) changed \xC2\xB7 +%2 -%3")
                .arg(formatCount(pr.filesChanged))
                .arg(formatCount(pr.additions))
                .arg(formatCount(pr.deletions)));
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        m_pullCommitsList->addItem(item);
    }
}

QString MainWindow::resolvablePullHead(PullRequest pr) const
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || pr.number <= 0)
        return QString();
    auto resolvesLocalBranch = [&](const QString &branch) {
        return repoHasLocalBranch(dir, branch);
    };
    QString head = pr.head.trimmed();
    const int separator = head.indexOf(QLatin1Char(':'));
    if (separator > 0)
        head = head.mid(separator + 1).trimmed();
    if (resolvesLocalBranch(head))
        return head;
    const QString canonical = QStringLiteral("pr/%1").arg(pr.number);
    return resolvesLocalBranch(canonical) ? canonical : QString();
}

QString MainWindow::localPullHeadForDeletion(PullRequest pr) const
{
    const QString base = pr.base.trimmed();
    const QString head = resolvablePullHead(std::move(pr));
    if (head.isEmpty() || head == base || head == currentRef())
        return QString();
    return head;
}

// The base..head range walks for a PR, reused while neither ref has moved.
// Both walks are `git log` over a branch range — the single most common blocking
// call in the stall log, because showPull() runs them afresh on every pull-list
// refresh (a push, an agent transcript event, a merge elsewhere) even when the
// branch tips are exactly where they were. Resolving the two refs first costs
// one short `rev-parse`, and those SHAs are an exact cache key: if neither moved
// the walk output is byte-identical by construction, so there is nothing to
// recompute. Returns false when either ref is missing from this node (a
// cross-node PR), which is the caller's cue to fall back to the signed mbox.
namespace {

// A local branch's tip straight from the ref files, for the common case where
// base and head are both branches of this checkout: two small reads instead of
// a git process the GUI thread then waits on (nine of those waits, one of them
// 153 s long, opened the 2026-09-12 freeze - stalls.log 0015). Anything git's
// own lookup could resolve differently - a SHA, a tag of the same name, a
// remote-tracking ref, a linked worktree whose refs live in the common dir -
// comes back empty and the caller runs `git rev-parse` exactly as before.
QString localBranchTipFromFiles(const QString &repoDir, const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('-')) ||
        name.contains(QLatin1String("..")) || name.contains(QLatin1Char('^')) ||
        name.contains(QLatin1Char('~')) || name.contains(QLatin1Char('@')) ||
        name.contains(QLatin1Char(':')) || name.startsWith(QLatin1String("refs/"))) {
        return QString();
    }
    const QString gitDir = resolveGitDirPath(repoDir);
    if (gitDir.isEmpty())
        return QString();
    if (!refTipFromFiles(gitDir, QStringLiteral("refs/tags/") + name).isEmpty())
        return QString(); // git would take the tag first; let it decide
    return refTipFromFiles(gitDir, QStringLiteral("refs/heads/") + name);
}

} // namespace

bool MainWindow::pullRangeSnapshot(const QString &base, const QString &head,
                                   PullRangeSnapshot *out) const
{
    const QString dir = repoGitDir();
    if (dir.isEmpty() || base.isEmpty() || head.isEmpty())
        return false;
    const QString baseSha = localBranchTipFromFiles(dir, base);
    const QString headSha = localBranchTipFromFiles(dir, head);
    const bool fromFiles = !baseSha.isEmpty() && !headSha.isEmpty();
    const QString key = dir + QLatin1Char('\x1f') + base + QLatin1Char('\x1f') + head;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kRecheckMs = 15000;
    MainWindow *self = const_cast<MainWindow *>(this);
    const auto cached = m_pullRangeCache.constFind(key);
    if (cached != m_pullRangeCache.constEnd() &&
        (fromFiles ? cached->baseSha == baseSha && cached->headSha == headSha
                   : true)) {
        if (!fromFiles && now - cached->readAtMs > kRecheckMs)
            self->loadPullRangeSnapshot(dir, base, head);
        if (out)
            *out = *cached;
        return true;
    }
    const auto missed = m_pullRangeMissAtMs.constFind(key);
    if (fromFiles || missed == m_pullRangeMissAtMs.constEnd() ||
        now - *missed > kRecheckMs)
        self->loadPullRangeSnapshot(dir, base, head);
    return false;
}

MainWindow::PullRangeSnapshot
MainWindow::readPullRangeSnapshot(const QString &dir, const QString &base,
                                  const QString &head)
{
    PullRangeSnapshot snapshot;
    QByteArray refsOut;
    if (!runGitCapture(dir,
                       {QStringLiteral("rev-parse"),
                        base + QStringLiteral("^{commit}"),
                        head + QStringLiteral("^{commit}")},
                       &refsOut, nullptr))
        return snapshot;
    const QStringList resolved =
        QString::fromUtf8(refsOut).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (resolved.size() < 2)
        return snapshot;
    const QString range = base + QStringLiteral("..") + head;
    runGitCapture(dir,
                  {"log", "--no-merges", "--date=format:%Y-%m-%d %H:%M",
                   "--pretty=%H\x1f%h\x1f%s\x1f%an\x1f%ad\x1f%ct\x1f"
                   "%(trailers:key=ForkMesh-Agent,valueonly,separator=%x2C)",
                   range},
                  &snapshot.detailedLog, nullptr);
    QByteArray shaOut;
    if (runGitCapture(dir, {"log", "--no-merges", "--pretty=%H", range}, &shaOut,
                      nullptr))
        snapshot.shas =
            QString::fromUtf8(shaOut).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    snapshot.baseSha = resolved.at(0).trimmed();
    snapshot.headSha = resolved.at(1).trimmed();
    snapshot.readAtMs = QDateTime::currentMSecsSinceEpoch();
    return snapshot;
}

void MainWindow::loadPullRangeSnapshot(const QString &dir, const QString &base,
                                       const QString &head)
{
    const QString key = dir + QLatin1Char('\x1f') + base + QLatin1Char('\x1f') + head;
    if (m_pullRangeLoading.contains(key))
        return;
    m_pullRangeLoading.insert(key);
    runOffThread<PullRangeSnapshot>(
        [dir, base, head] { return readPullRangeSnapshot(dir, base, head); },
        [this, key, dir](PullRangeSnapshot snapshot) {
            m_pullRangeLoading.remove(key);
            if (snapshot.baseSha.isEmpty() || snapshot.headSha.isEmpty()) {
                m_pullRangeMissAtMs.insert(key, QDateTime::currentMSecsSinceEpoch());
                return;
            }
            m_pullRangeMissAtMs.remove(key);
            const auto previous = m_pullRangeCache.constFind(key);
            const bool changed = previous == m_pullRangeCache.constEnd() ||
                                 previous->baseSha != snapshot.baseSha ||
                                 previous->headSha != snapshot.headSha;
            if (m_pullRangeCache.size() > 64)
                m_pullRangeCache.clear();
            m_pullRangeCache.insert(key, snapshot);
            if (!changed || dir != repoGitDir() || m_pullRangeRerenderQueued)
                return;
            m_pullRangeRerenderQueued = true;
            QTimer::singleShot(0, this, [this] { rerenderOpenPullRange(); });
        });
}

void MainWindow::rerenderOpenPullRange()
{
    m_pullRangeRerenderQueued = false;
    if (!m_pullDetail || !m_pullDetail->isVisible() || m_currentPullNumber <= 0)
        return;
    PullRequest current;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == m_currentPullNumber)
            current = pr;
    if (current.number <= 0)
        return;
    renderPullCommits(current);
    renderPullChecks(current);
    renderPullChecksSummary(current);
    updatePullSubTabCounts(current);
    if (m_pullActivityRefreshing)
        return;
    const QScopedValueRollback<bool> busy(m_pullActivityRefreshing, true);
    const int offset = m_pullThreadScroll && m_pullThreadScroll->verticalScrollBar()
                           ? m_pullThreadScroll->verticalScrollBar()->value()
                           : 0;
    renderPullThread(current);
    if (m_pullThreadScroll && m_pullThreadScroll->verticalScrollBar())
        m_pullThreadScroll->verticalScrollBar()->setValue(offset);
    setPullActionBadge(m_pullTabConversation,
                       m_pullConversationBaseCount + m_pullActivityExtraCards);
}

QStringList MainWindow::pullCommitShas(const PullRequest &pr) const
{
    const PullRequest snapshot = pr;
    PullRangeSnapshot range;
    const QString reviewHead = resolvablePullHead(snapshot);
    if (!pullRangeSnapshot(snapshot.base, reviewHead, &range))
        return {};
    return range.shas;
}

QList<int> MainWindow::runIdsForPull(PullRequest pr) const
{
    QList<int> ids;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return ids;
    // `pr` is taken by value and owner/name are copied up front: pullCommitShas()
    // and the rev-parse below both run synchronous git reads that pump the event
    // loop (under a GitKeepAlive scope), and that pump can re-enter
    // action/refresh paths which reassign m_repositories and m_currentPulls. A
    // reference into either would then dangle and the reads after the pump would
    // be use-after-frees (, — pr.head below crashed when a
    // caller's reference into m_currentPulls was freed by a nested reload).
    const QString repoOwner = m_repositories.at(m_repoDetailIndex).owner;
    const QString repoName = m_repositories.at(m_repoDetailIndex).name;
    QSet<QString> shas;
    PullRangeSnapshot range;
    const QString reviewHead = resolvablePullHead(pr);
    if (pullRangeSnapshot(pr.base, reviewHead, &range)) {
        shas = QSet<QString>(range.shas.cbegin(), range.shas.cend());
        if (!range.headSha.isEmpty())
            shas.insert(range.headSha);
    } else if (const QString dir = repoGitDir();
               !dir.isEmpty() && !reviewHead.isEmpty()) {
        if (const QString tip = localBranchTipFromFiles(dir, reviewHead);
            !tip.isEmpty())
            shas.insert(tip);
    }
    if (shas.isEmpty())
        return ids;
    for (const ActionRun &run : std::as_const(m_actionRuns)) {
        if (run.owner == repoOwner && run.name == repoName &&
            shas.contains(run.commit))
            ids.append(run.id);
    }
    return ids;
}

void MainWindow::renderPullChecks(PullRequest pr)
{
    if (!m_pullChecksTable)
        return;
    TableRepaintGuard repaintGuard(m_pullChecksTable);
    m_pullChecksTable->setRowCount(0);
    if (m_pullRunChecksButton)
        m_pullRunChecksButton->setEnabled(pr.number > 0 && pr.status == "open");
    if (m_pullApproveChecksButton)
        m_pullApproveChecksButton->hide();
    if (m_pullChecksNotice) {
        m_pullChecksNotice->hide();
        m_pullChecksNotice->setStyleSheet(QString());
    }
    if (pr.number <= 0) {
        if (m_pullChecksLog)
            m_pullChecksLog->clear();
        return;
    }
    int keepRunId = -1;
    if (const QModelIndexList sel = m_pullChecksTable->selectionModel()->selectedRows();
        !sel.isEmpty())
        if (QTableWidgetItem *it = m_pullChecksTable->item(sel.first().row(), 0))
            keepRunId = it->data(Qt::UserRole).toInt();
    const QList<int> ids = runIdsForPull(pr);
    for (const int id : ids) {
        const ActionRun *run = findRun(id);
        if (!run)
            continue;
        const int row = m_pullChecksTable->rowCount();
        m_pullChecksTable->insertRow(row);
        auto *status = new QTableWidgetItem(actionStatusText(run->status));
        status->setForeground(actionStatusColor(run->status));
        status->setData(Qt::UserRole, run->id);
        m_pullChecksTable->setItem(row, 0, status);
        m_pullChecksTable->setItem(
            row, 1, new QTableWidgetItem(run->workflowName.isEmpty()
                                             ? run->workflowPath
                                             : run->workflowName));
        m_pullChecksTable->setItem(row, 2, new QTableWidgetItem(run->commit.left(8)));
        const qint64 dur = run->finishedAtMs > run->startedAtMs && run->startedAtMs > 0
                               ? run->finishedAtMs - run->startedAtMs
                               : 0;
        m_pullChecksTable->setItem(
            row, 3,
            new QTableWidgetItem(dur > 0 ? formatDuration(dur)
                                         : QString::fromUtf8("\xE2\x80\x94")));
    }
    if (m_pullChecksTable->rowCount() > 0) {
        int keepRow = 0;
        if (keepRunId >= 0)
            for (int r = 0; r < m_pullChecksTable->rowCount(); ++r)
                if (m_pullChecksTable->item(r, 0)->data(Qt::UserRole).toInt() == keepRunId) {
                    keepRow = r;
                    break;
                }
        m_pullChecksTable->selectRow(keepRow);
    } else if (m_pullChecksLog)
        m_pullChecksLog->setPlainText(displaySafePlainLog(
            "No checks have run for this pull request yet. Checks queue "
            "themselves for every workflow that declares \"on: pull_request\"; "
            "use \"Run checks against this PR\" to queue this repository's "
            "workflows against it now."));

    // A first pull request from a new contributor waits here on purpose: its
    // run executes their commands on this node, so a maintainer releases it by
    // hand — for this pull request only.
    int held = 0;
    QString heldAuthor;
    bool firstTime = false;
    for (const int id : ids) {
        const ActionRun *run = findRun(id);
        if (!run || run->status != ActionStatus::AwaitingApproval)
            continue;
        ++held;
        if (run->firstTimePullAuthor) {
            firstTime = true;
            if (heldAuthor.isEmpty())
                heldAuthor = run->pullAuthor;
        }
    }
    if (held > 0 && m_pullApproveChecksButton && m_pullChecksNotice) {
        m_pullApproveChecksButton->show();
        m_pullApproveChecksButton->setText(
            held == 1 ? QStringLiteral("Approve and run")
                      : QStringLiteral("Approve and run %1").arg(held));
        if (heldAuthor.isEmpty())
            heldAuthor = pr.authorName.trimmed().isEmpty()
                             ? pr.author.left(10)
                             : pr.authorName.trimmed();
        m_pullChecksNotice->setText(
            firstTime
                ? QString::fromUtf8(
                      "%1 has no merged work in this repository yet, so every "
                      "pull request of theirs needs your approval before its "
                      "checks run.")
                      .arg(heldAuthor.toHtmlEscaped())
                : QString::fromUtf8(
                      "Waiting for your approval before running on this node."));
        m_pullChecksNotice->show();
    }
}

void MainWindow::renderPullChecksSummary(PullRequest pr)
{
    if (!m_pullChecksSummary)
        return;
    if (pr.number <= 0) {
        m_pullChecksSummary->hide();
        return;
    }
    // A skipped or cancelled run never reached a verdict, so it is neither a
    // pass nor a failure and must not be folded into either — the same
    // vocabulary the web pull view already counts by. Rolling them into the
    // failure count read as "3 failed" on a pull request nothing had rejected;
    // they get their own neutral counts, in the grey actionStatusColor() gives
    // them everywhere else. A rejected run is a real verdict and stays a
    // failure.
    int passed = 0, failed = 0, skipped = 0, cancelled = 0, running = 0, pending = 0;
    for (const int id : runIdsForPull(pr)) {
        const ActionRun *run = findRun(id);
        if (!run)
            continue;
        if (run->status == ActionStatus::Success)
            ++passed;
        else if (run->status == ActionStatus::Skipped)
            ++skipped;
        else if (run->status == ActionStatus::Cancelled)
            ++cancelled;
        else if (run->status == ActionStatus::Failed ||
                 run->status == ActionStatus::Rejected)
            ++failed;
        else if (run->status == ActionStatus::Running)
            ++running;
        else
            ++pending; // queued / awaiting approval
    }
    const int total =
        passed + failed + skipped + cancelled + running + pending;
    if (total == 0) {
        m_pullChecksSummary->hide();
        return;
    }
    QStringList parts;
    if (passed)
        parts << QString::fromUtf8("<span style='color:#3fb950'>\xE2\x9C\x93 %1 passed</span>")
                     .arg(passed);
    if (failed)
        parts << QString::fromUtf8("<span style='color:#f85149'>\xE2\x9C\x97 %1 failed</span>")
                     .arg(failed);
    if (skipped)
        parts << QString::fromUtf8( // circled slash
                     "<span style='color:#8b949e'>\xE2\x8A\x98 %1 skipped</span>")
                     .arg(skipped);
    if (cancelled)
        parts << QString::fromUtf8(
                     "<span style='color:#8b949e'>\xE2\x8A\x98 %1 cancelled</span>")
                     .arg(cancelled);
    if (running)
        parts << QString::fromUtf8("<span style='color:#58a6ff'>\xE2\x97\x8F %1 running</span>")
                     .arg(running);
    if (pending)
        parts << QStringLiteral("<span style='color:#8b949e'>%1 pending</span>").arg(pending);
    m_pullChecksSummary->setText(
        QString::fromUtf8("<b>Checks</b> \xC2\xB7 %1 \xC2\xB7 <a href='#checks' "
                       "style='color:#58a6ff;text-decoration:none'>details</a>")
            .arg(parts.join(QString::fromUtf8(" \xC2\xB7 "))));
    m_pullChecksSummary->show();
}

void MainWindow::showPullCheckLog(int runId)
{
    if (!m_pullChecksLog)
        return;
    const ActionRun *run = findRun(runId);
    if (!run) {
        m_pullChecksLog->clear();
        return;
    }
    m_pullChecksLog->setPlainText(displaySafePlainLog(
        m_actionStore ? m_actionStore->readLog(*run) : QString()));
    m_pullChecksLog->moveCursor(QTextCursor::End);
}

void MainWindow::runChecksForCurrentPull()
{
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    PullRequest pr;
    for (const PullRequest &p : std::as_const(m_currentPulls))
        if (p.number == m_currentPullNumber)
            pr = p;
    if (pr.number <= 0)
        return;
    const RepositoryRecord repo =
        writableRecordFor(m_repositories.at(m_repoDetailIndex));
    // Asking for the checks is not the same as vouching for the code: a first
    // pull request from a new contributor still lands awaiting approval, with
    // the workflow and repository state on screen before anything executes.
    // Same publication path as the automatic pass, so a pull request whose head
    // only exists in the working copy still reaches the runner's mirror — before
    // that it queued nothing at all and the tab stayed empty.
    resolvePullCheckHeads(repo, {pullCheckCandidate(pr, m_currentPulls, repo)},
                          WorkflowTrigger::PullRequestManual, /*force=*/true);
    setRepoDetailNotice(
        QString::fromUtf8("Queueing checks for pull request #%1\xE2\x80\xA6")
            .arg(pr.number),
        false);
    renderPullChecks(pr);
    renderPullChecksSummary(pr);
    updatePullSubTabCounts(pr);
}

// Release every held run for the open pull request. This is the same bound
// approval the Actions page grants — the workflow and the repository state it
// was recorded against — surfaced where a reviewer reads the pull request.
void MainWindow::approveChecksForCurrentPull()
{
    if (m_currentPullNumber < 0)
        return;
    PullRequest pr;
    for (const PullRequest &p : std::as_const(m_currentPulls))
        if (p.number == m_currentPullNumber)
            pr = p;
    if (pr.number <= 0)
        return;
    QStringList failures;
    int approved = 0;
    for (const int id : runIdsForPull(pr)) {
        const ActionRun *run = findRun(id);
        if (!run || run->status != ActionStatus::AwaitingApproval)
            continue;
        QString error;
        if (approveActionRun(id, &error))
            ++approved;
        else if (!error.isEmpty() && !failures.contains(error))
            failures.append(error);
    }
    if (approved > 0) {
        m_actionRuns = m_actionStore->loadAllRuns();
        refreshActionsTable();
        updateNotificationButton();
        processActionQueue();
    }
    refreshOpenPullChecks();
    if (approved > 0)
        showLocalNotice(m_pullChecksNotice,
                        QStringLiteral("Approved %1 check%2 for pull request #%3.")
                            .arg(approved)
                            .arg(approved == 1 ? QString() : QStringLiteral("s"))
                            .arg(pr.number));
    if (!failures.isEmpty())
        showLocalNotice(m_pullChecksNotice, failures.join(QLatin1Char(' ')), true);
}

void MainWindow::buildAndPreviewCurrentPull()
{
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const PullRequest *pr = nullptr;
    for (const PullRequest &p : std::as_const(m_currentPulls))
        if (p.number == m_currentPullNumber)
            pr = &p;
    if (!pr) {
        setRepoDetailNotice("This pull request has no head branch to build.", true);
        return;
    }
    // Copy what we need out of the PR now: runGitCapture below pumps the GUI
    // event loop, and a reloadPulls() serviced during the pump reassigns
    // m_currentPulls, dangling `pr` (git-pump UAF family).
    const PullRequest snapshot = *pr;
    const int number = snapshot.number;
    const QString head = resolvablePullHead(snapshot);
    if (head.isEmpty()) {
        setRepoDetailNotice(
            "Could not resolve this pull request's reviewed code branch.", true);
        return;
    }
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString gitDir = repoGitDir();
    if (gitDir.isEmpty()) {
        setRepoDetailNotice("No local copy of this repository to build from.", true);
        return;
    }
    if (m_pullPreviewResolving)
        return;
    m_pullPreviewResolving = true;
    runOffThread<QString>(
        [gitDir, head] {
            QByteArray tip;
            if (!runGitCapture(gitDir, {QStringLiteral("rev-parse"), head}, &tip,
                               nullptr))
                return QString();
            return QString::fromUtf8(tip).trimmed();
        },
        [this, repo, number, gitDir](QString commit) {
            m_pullPreviewResolving = false;
            if (m_currentPullNumber != number || repoGitDir() != gitDir)
                return;
            if (commit.isEmpty()) {
                setRepoDetailNotice(
                    "Could not resolve this pull request's head commit to build it.",
                    true);
                return;
            }
            previewPullAtCommit(repo, number, gitDir, commit);
        });
}

void MainWindow::previewPullAtCommit(const RepositoryRecord &repo, int number,
                                     const QString &gitDir, const QString &commit)
{
    QString slug = repo.owner + QLatin1Char('-') + repo.name;
    slug.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                 QStringLiteral("_"));
    const QString previewDir =
        QDir::tempPath() + QStringLiteral("/forkmesh-pr-preview/%1-pr%2")
                               .arg(slug).arg(number);
    const QString clientDir = previewDir + QStringLiteral("/desktop");
    const QString buildDir = clientDir + QStringLiteral("/build");
    const bool haveWorktree = QFileInfo::exists(previewDir + QStringLiteral("/.git"));

    if (m_pullPreviewDialog) {
        m_pullPreviewDialog->deleteLater();
        m_pullPreviewDialog = nullptr;
    }
    auto *dialog = new QDialog(this);
    m_pullPreviewDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QObject::destroyed, this, [this] { m_pullPreviewDialog = nullptr; });
    dialog->setWindowTitle(
        QStringLiteral("Build & preview pull request #%1").arg(number));
    dialog->resize(760, 480);
    auto *status = new QLabel(QString::fromUtf8("Preparing\xE2\x80\xA6"), dialog);
    status->setObjectName("statusLine");
    status->setWordWrap(true);
    auto *log = new QPlainTextEdit(dialog);
    log->setReadOnly(true);
    log->setObjectName("codeEditor");
    log->setLineWrapMode(QPlainTextEdit::NoWrap);
    applyLogFont(log);
    auto *closeBtn = new QPushButton(QStringLiteral("Close"), dialog);
    closeBtn->setObjectName("ghostButton");
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, dialog, &QDialog::close);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch();
    buttonRow->addWidget(closeBtn);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(status);
    layout->addWidget(log, 1);
    layout->addLayout(buttonRow);
    dialog->show();

    QPointer<QDialog> dlg(dialog);
    QPointer<QLabel> statusPtr(status);
    QPointer<QPlainTextEdit> logPtr(log);
    auto appendLog = [logPtr](const QString &text) {
        if (!logPtr)
            return;
        logPtr->moveCursor(QTextCursor::End);
        logPtr->insertPlainText(text);
        logPtr->moveCursor(QTextCursor::End);
    };

    auto launchPreview = [this, dlg, statusPtr, appendLog, buildDir, previewDir,
                          number] {
        const QString binary = builtExecutablePath(buildDir);
        if (!QFileInfo::exists(binary)) {
            if (statusPtr)
                statusPtr->setText(
                    QStringLiteral("Build finished but the binary was not found at %1.")
                        .arg(binary));
            return;
        }
        const QString sandbox = previewDir + QStringLiteral("/preview-home");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XDG_DATA_HOME"), sandbox + QStringLiteral("/data"));
        env.insert(QStringLiteral("XDG_CONFIG_HOME"),
                   sandbox + QStringLiteral("/config"));
        env.insert(QStringLiteral("XDG_CACHE_HOME"), sandbox + QStringLiteral("/cache"));
        env.insert(QStringLiteral("XDG_STATE_HOME"), sandbox + QStringLiteral("/state"));
        QProcess launcher;
        launcher.setProgram(binary);
        launcher.setProcessEnvironment(
            forkmesh::processreaper::releasedEnvironment(env));
        launcher.setWorkingDirectory(QFileInfo(binary).absolutePath());
        appendLog(QString::fromUtf8("\n\xE2\x86\x92 launching %1\n").arg(binary));
        if (launcher.startDetached()) {
            if (statusPtr)
                statusPtr->setText(
                    QStringLiteral("Launched the ForkMesh preview for pull request "
                                   "#%1.").arg(number));
            flashMessage(QStringLiteral("Launched preview of pull request #%1.")
                             .arg(number));
        } else if (statusPtr) {
            statusPtr->setText(QStringLiteral("Could not launch %1.").arg(binary));
        }
    };

    if (!haveWorktree) {
        forkmesh::safedelete::removeTree(previewDir, QFileInfo(previewDir).absolutePath());
        QDir().mkpath(QFileInfo(previewDir).absolutePath());
    }
    auto steps = std::make_shared<QList<PullPreviewStep>>(
        pullPreviewSteps(gitDir, previewDir, clientDir, buildDir, commit,
                         haveWorktree, ramCappedBuildJobs()));

    auto runNext = std::make_shared<std::function<void(int)>>();
    *runNext = [this, steps, runNext, dlg, statusPtr, appendLog,
                launchPreview](int index) {
        if (!dlg)
            return; // dialog closed — abandon the build
        if (index >= steps->size()) {
            launchPreview();
            return;
        }
        const PullPreviewStep st = steps->at(index);
        if (statusPtr)
            statusPtr->setText(st.status);
        appendLog(QStringLiteral("\n$ %1 %2\n  (in %3)\n")
                      .arg(st.program, st.args.join(QLatin1Char(' ')), st.dir));
        auto *proc = new QProcess(this);
        connect(dlg.data(), &QObject::destroyed, proc, [proc] {
            proc->disconnect();
            if (proc->state() != QProcess::NotRunning)
                proc->kill();
            proc->deleteLater();
        });
        proc->setWorkingDirectory(st.dir);
        proc->setProcessChannelMode(QProcess::MergedChannels);
        connect(proc, &QProcess::readyReadStandardOutput, this, [proc, appendLog] {
            appendLog(QString::fromUtf8(proc->readAllStandardOutput()));
        });
        connect(proc, &QProcess::finished, this,
                [proc, runNext, index, appendLog, statusPtr](
                    int code, QProcess::ExitStatus exitStatus) {
                    appendLog(QString::fromUtf8(proc->readAllStandardOutput()));
                    proc->deleteLater();
                    if (exitStatus != QProcess::NormalExit || code != 0) {
                        if (statusPtr)
                            statusPtr->setText(
                                QStringLiteral("Build failed (exit %1). See the log "
                                               "above.").arg(code));
                        return;
                    }
                    (*runNext)(index + 1);
                });
        connect(proc, &QProcess::errorOccurred, this,
                [proc, statusPtr](QProcess::ProcessError) {
                    if (statusPtr && proc->state() != QProcess::Running)
                        statusPtr->setText(
                            QString::fromUtf8("Could not run %1 \xE2\x80\x94 is it "
                                              "installed?").arg(proc->program()));
                });
        proc->start(st.program, st.args);
    };
    (*runNext)(0);
}

void MainWindow::updatePullSubTabCounts(PullRequest pr)
{
    const auto label = [](QPushButton *b, int n) { setPullActionBadge(b, n); };
    if (pr.number <= 0) {
        label(m_pullTabConversation, 0);
        label(m_pullTabCommits, 0);
        label(m_pullTabChecks, 0);
        label(m_pullTabFiles, 0);
        setPullChecksAttention(m_pullTabChecks, false);
        return;
    }
    const PullReviewSnapshot snapshot = buildPullReviewSnapshot(pr);
    m_pullConversationBaseCount = snapshot.topLevelItems + snapshot.totalThreads;
    label(m_pullTabConversation,
          m_pullConversationBaseCount +
              (m_pullActivityCardsNumber == pr.number ? m_pullActivityExtraCards : 0));
    label(m_pullTabCommits, pullCommitShas(pr).size());
    label(m_pullTabChecks, runIdsForPull(pr).size());
    label(m_pullTabFiles, pr.filesChanged);
    QString owner;
    QString name;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        owner = m_repositories.at(m_repoDetailIndex).owner;
        name = m_repositories.at(m_repoDetailIndex).name;
    }
    setPullChecksAttention(m_pullTabChecks,
                           pullHasRunningChecks(pr, m_actionRuns, owner, name));
}

void MainWindow::submitPullComment()
{
    if (m_currentPullNumber < 0 || !m_pullComposer)
        return;
    const QString body = m_pullComposer->markdown().trimmed();
    if (body.isEmpty()) {
        flashMessage(QStringLiteral("Write a comment first."));
        return;
    }
    PullStore store = pullStoreForCurrentRepo();
    PullEvent ev;
    ev.type = QStringLiteral("comment");
    ev.body = body;
    ev = store.makeSignedEvent(m_currentPullNumber, ev);
    QString error;
    if (store.canWrite()) {
        if (!store.addComment(m_currentPullNumber, body, &error)) {
            QMessageBox::warning(this, "Comment", error);
            return;
        }
    } else {
        submitPullEventToInbox(m_currentPullNumber, ev);
    }
    m_pullComposer->setMarkdown(QString());
    reloadPulls();
    showPull(m_currentPullNumber);
}

void MainWindow::sendPullRevisionToAgent()
{
    if (m_currentPullNumber < 0 || !m_pullAgentRevisionEdit || !m_agentStore)
        return;
    const QString feedback = m_pullAgentRevisionEdit->text().trimmed();
    if (feedback.isEmpty()) {
        flashMessage(QStringLiteral("Enter revision feedback first."));
        return;
    }

    QString head;
    for (const PullRequest &pr : m_currentPulls)
        if (pr.number == m_currentPullNumber) {
            head = pr.head;
            break;
        }
    const AgentSession *linked = agentSessionForPull(m_currentPullNumber, head);
    const int linkedId = linked ? linked->id : -1;
    if (linkedId < 0 || !findAgentSession(linkedId)) {
        flashMessage(QStringLiteral("No agent session found for this pull request."),
                     true);
        return;
    }

    PullStore store = pullStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        store.addComment(m_currentPullNumber, feedback, &error);
    }

    // Re-find the session only now: addComment pumps the GUI event loop while
    // waiting on git, and a reloadAgents() serviced during the pump rebuilds
    // m_agentSessions — a pointer taken before it would dangle (git-pump UAF
    // family).
    AgentSession *session = findAgentSession(linkedId);
    if (!session) {
        flashMessage(QStringLiteral("No agent session found for this pull request."),
                     true);
        return;
    }

    m_agentStore->appendLog(
        *session,
        QStringLiteral("\n==> Revision feedback from PR #%1:\n%2")
            .arg(m_currentPullNumber)
            .arg(feedback));
    session->status = AgentStatus::Queued;
    session->lastError.clear();
    session->finishedAtMs = 0;
    m_agentStore->saveSession(*session);

    const int sessionId = session->id;
    if (!m_agentQueue.contains(sessionId))
        m_agentQueue.append(sessionId);

    m_pullAgentRevisionEdit->clear();
    reloadAgents();
    reloadPulls();
    showPull(m_currentPullNumber);
    flashMessage(QStringLiteral("Revision sent to agent session #%1.").arg(sessionId));
    processAgentQueue();
}

void MainWindow::submitPullReview(const QString &state)
{
    if (m_currentPullNumber < 0 || !m_pullComposer)
        return;
    const QString body = m_pullComposer->markdown().trimmed();
    PullStore store = pullStoreForCurrentRepo();
    PullEvent ev;
    ev.type = QStringLiteral("review");
    ev.state = state;
    ev.body = body;
    ev = store.makeSignedEvent(m_currentPullNumber, ev);
    QString error;
    if (store.canWrite()) {
        if (!store.addReview(m_currentPullNumber, state, body, &error)) {
            QMessageBox::warning(this, "Review", error);
            return;
        }
    } else {
        submitPullEventToInbox(m_currentPullNumber, ev);
    }
    m_pullComposer->setMarkdown(QString());
    reloadPulls();
    showPull(m_currentPullNumber);
}

void MainWindow::updatePullActionState()
{
    const PullStore store = pullStoreForCurrentRepo();
    const bool writable = store.canWrite();
    const bool have = m_currentPullNumber >= 0;
    bool open = false;
    bool closed = false;
    bool merged = false;
    bool archived = false;
    QString head;
    QString base;
    QString patch;
    int independentApprovals = 0;
    int linkedIssues = 0;
    bool independentChangesRequested = false;
    bool independentReviewReady = false;
    QStringList changesRequestedBy;
    const PullRequest *selectedPull = nullptr;
    for (const PullRequest &pr : m_currentPulls) {
        if (pr.number == m_currentPullNumber) {
            selectedPull = &pr;
            open   = pr.status == "open";
            closed = pr.status == "closed";
            merged = pr.status == "merged";
            archived = pr.archivedOnCodeBranch;
            head   = pr.head;
            base   = pr.base;
            patch  = pr.patch;
            linkedIssues = issuesLinkedFromPull(pr).size();
            independentApprovals = pr.independentApprovalCount();
            changesRequestedBy = pr.independentChangesRequestedBy();
            independentChangesRequested = !changesRequestedBy.isEmpty();
            independentReviewReady = pr.independentReviewGateSatisfied();
        }
    }
    const bool manageable = writable && have && !archived;
    const bool authorRequest = !writable && currentPullAuthoredHere();
    const bool mergeable = manageable && open;
    const bool requirePeerApproval =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
            ? m_repositories.at(m_repoDetailIndex).requirePeerApproval
            : true;
    const bool repositoryOwner = currentAccountOwnsPullRepository();
    const bool reviewBlocks = PullMergePolicy::reviewGateBlocks(
        requirePeerApproval, independentReviewReady, repositoryOwner);
    const bool ownerReviewOverride = PullMergePolicy::ownerOverrideRequired(
        requirePeerApproval, independentReviewReady, repositoryOwner);
    bool behind = false;
    int behindCount = 0;
    bool behindPending = false;
    if (mergeable && selectedPull) {
        const QString cacheKey = pullBehindCacheKey(*selectedPull);
        const auto cached = m_pullBehindCache.constFind(cacheKey);
        if (cached != m_pullBehindCache.constEnd() && cached->valid) {
            behind = cached->behind;
            behindCount = cached->count;
        } else {
            behindPending = true;
            queuePullBehindCheck(m_currentPullNumber, cacheKey);
        }
    }
    bool mergeClean = true;
    bool conflictPending = false;
    QStringList conflictFiles;
    if (mergeable) {
        const QString fingerprint = pullPatchFingerprint(patch);
        const auto cached = m_pullConflictCache.constFind(m_currentPullNumber);
        if (cached != m_pullConflictCache.constEnd() &&
            cached->fingerprint == fingerprint) {
            mergeClean = !cached->conflict;
            conflictFiles = cached->conflictFiles;
        } else {
            conflictPending = true;
            queuePullConflictCheck(m_currentPullNumber, fingerprint);
        }
    }
    if (m_pullMergeStatus) {
        if (!mergeable) {
            m_pullMergeStatus->hide();
        } else if (conflictPending) {
            m_pullMergeStatus->setText(QString::fromUtf8(
                "<span style='color:#8b949e'>Checking for conflicts\xE2\x80\xA6"
                "</span>"));
            m_pullMergeStatus->show();
        } else if (reviewBlocks && independentChangesRequested) {
            m_pullMergeStatus->setText(QString::fromUtf8(
                "<span style='color:#f85149'>\xE2\x9A\xA0 Changes requested "
                "\xE2\x80\x94 a reviewer is blocking this merge. Resolve their "
                "review (approve, or clear the request) to merge.</span>"));
            m_pullMergeStatus->show();
        } else if (reviewBlocks) {
            m_pullMergeStatus->setText(QString::fromUtf8(
                "<span style='color:#d29922'>Peer approval required "
                "\xE2\x80\x94 at least one reviewer other than the pull-request "
                "author must approve before merge.</span>"));
            m_pullMergeStatus->show();
        } else if (mergeClean && ownerReviewOverride &&
                   independentChangesRequested) {
            m_pullMergeStatus->setText(QString::fromUtf8(
                "<span style='color:#d29922'>\xE2\x9A\xA0 Changes requested "
                "\xE2\x80\x94 peer review is recommended, but as the repository "
                "owner you can merge anyway. The waiver will be recorded in the "
                "pull-request ledger.</span>"));
            m_pullMergeStatus->show();
        } else if (mergeClean && ownerReviewOverride) {
            m_pullMergeStatus->setText(QString::fromUtf8(
                "<span style='color:#d29922'>Peer approval recommended "
                "\xE2\x80\x94 no independent reviewer has approved this pull "
                "request. As the repository owner you can merge without review; "
                "the waiver will be recorded in the pull-request ledger.</span>"));
            m_pullMergeStatus->show();
        } else if (mergeClean && !requirePeerApproval) {
            m_pullMergeStatus->setText(QString::fromUtf8(
                "<span style='color:#3fb950'>\xE2\x9C\x93 No conflicts \xE2\x80\x94 "
                "peer approval is optional for this repository; ready to "
                "merge.</span>"));
            m_pullMergeStatus->show();
        } else if (mergeClean) {
            m_pullMergeStatus->setText(QString::fromUtf8(
                "<span style='color:#3fb950'>\xE2\x9C\x93 No conflicts \xE2\x80\x94 "
                "%1 independent approval%2; ready to merge. More peer approvals "
                "strengthen the review signal.</span>")
                .arg(independentApprovals)
                .arg(independentApprovals == 1 ? QString() : QStringLiteral("s")));
            m_pullMergeStatus->show();
        } else {
            const QString detail =
                conflictFiles.isEmpty()
                    ? QStringLiteral("the patch does not apply to the current base")
                    : QStringLiteral("conflicts in %1 file(s): %2")
                          .arg(conflictFiles.size())
                          .arg(conflictFiles.join(QStringLiteral(", ")).toHtmlEscaped());
            m_pullMergeStatus->setText(
                QString::fromUtf8(
                    "<span style='color:#f85149'>\xE2\x9A\xA0 Cannot merge cleanly "
                    "\xE2\x80\x94 %1. Update the branch from its base, then retry."
                    "</span>")
                    .arg(detail));
            m_pullMergeStatus->show();
        }
    }
    if (m_pullNewButton)
        m_pullNewButton->setEnabled(m_repoDetailIndex >= 0);
    if (m_pullChooseDirButton)
        m_pullChooseDirButton->setEnabled(m_repoDetailIndex >= 0);
    if (m_pullImportButton)
        m_pullImportButton->setEnabled(writable);
    if (m_pullSyncButton)
        m_pullSyncButton->setEnabled(writable);
    refreshPendingInboxBadges();
    if (m_pullDeleteClosedAndMergedButton) {
        int cleanupCount = 0;
        for (const PullRequest &pr : m_currentPulls)
            if (!pr.archivedOnCodeBranch &&
                (pr.status == QLatin1String("closed") ||
                 pr.status == QLatin1String("merged")))
                ++cleanupCount;
        m_pullDeleteClosedAndMergedButton->setEnabled(
            writable && cleanupCount > 0 && !m_pullDeleteInProgress);
        setPullActionBadge(m_pullDeleteClosedAndMergedButton, cleanupCount);
    }
    const QString baseLabel =
        base.isEmpty() ? QStringLiteral("the base branch") : base;
    const QString updateTip =
        !writable
            ? QStringLiteral("Updating the branch needs a local working tree — "
                             "this repository is read-only on this node.")
            : behindPending
                  ? QStringLiteral("Checking whether this pull request branch "
                                   "is behind its base…")
            : behind
                  ? QStringLiteral("Merge %1 into this pull request's branch — "
                                   "it is %2 commit%3 behind")
                        .arg(baseLabel)
                        .arg(behindCount)
                        .arg(behindCount == 1 ? QString() : QStringLiteral("s"))
                  : QStringLiteral("This pull request's branch already has "
                                   "everything from %1")
                        .arg(baseLabel);
    if (m_pullUpdateButton) {
        m_pullUpdateButton->setVisible(writable && have && open &&
                                       (behind || behindPending));
        m_pullUpdateButton->setEnabled(writable && have && open && behind);
        m_pullUpdateButton->setToolTip(updateTip);
        setPullActionBadge(m_pullUpdateButton, behind ? behindCount : 0);
    }
    // The conversation tab keeps the button on screen for every open pull
    // request — disabled with a "why" tooltip when there is nothing to merge —
    // so updating the branch is discoverable next to "Merge pull request".
    if (m_pullConversationUpdateButton) {
        m_pullConversationUpdateButton->setVisible(have && open && !archived);
        m_pullConversationUpdateButton->setEnabled(writable && have && open &&
                                                   behind);
        m_pullConversationUpdateButton->setToolTip(updateTip);
    }
    if (m_pullMergeButton) {
        const bool canMerge = mergeable && mergeClean && !conflictPending &&
                              !reviewBlocks;
        m_pullMergeButton->setEnabled(canMerge);
        m_pullMergeButton->setToolTip(
            conflictPending
                ? QStringLiteral("Checking whether this pull request still applies "
                                 "cleanly…")
                : mergeable && reviewBlocks
                      ? QStringLiteral("At least one independent peer approval is "
                                       "required, with no unresolved request for "
                                       "changes.")
                      : mergeable && ownerReviewOverride
                            ? QStringLiteral(
                                  "Merge as repository owner without the required "
                                  "peer review; the waiver will be recorded")
                            : mergeable && !mergeClean
                                  ? QStringLiteral(
                                        "This pull request has conflicts — use "
                                        "\"Resolve conflicts\" to commit a fix to "
                                        "its branch, then merge.")
                                  : QStringLiteral(
                                        "Apply and merge this pull request"));
        if (m_pullConversationMergeButton) {
            m_pullConversationMergeButton->setVisible(have && open && !archived);
            m_pullConversationMergeButton->setEnabled(canMerge);
            m_pullConversationMergeButton->setText(
                ownerReviewOverride ? QStringLiteral("Merge without review")
                                    : QStringLiteral("Merge pull request"));
            m_pullConversationMergeButton->setToolTip(m_pullMergeButton->toolTip());
        }
    }
    if (m_pullQueueButton) {
        const bool queueEnabled = mergeQueueEnabledForOpenRepo();
        const bool offerQueue = queueEnabled && manageable && open;
        const MergeQueue queue = openRepoMergeQueue();
        const bool queued = have && queue.contains(m_currentPullNumber);
        m_pullQueueButton->setVisible(offerQueue);
        m_pullQueueButton->setEnabled(offerQueue);
        m_pullQueueButton->setText(queued ? QStringLiteral("Dequeue")
                                          : QStringLiteral("Queue"));
        m_pullQueueButton->setToolTip(
            queued ? QStringLiteral("Take this pull request back out of the merge "
                                    "queue")
                   : QStringLiteral("Add this pull request to the merge queue — it "
                                    "is updated from its base branch and merged in "
                                    "turn, after the ones already queued"));
        setPullActionBadge(m_pullQueueButton,
                           queued ? queue.indexOf(m_currentPullNumber) + 1 : 0);
    }
    if (m_pullMergeDeleteButton) {
        m_pullMergeDeleteButton->setEnabled(mergeable && mergeClean &&
                                            !conflictPending && !reviewBlocks &&
                                            !m_pullDeleteInProgress);
        m_pullMergeDeleteButton->setToolTip(
            ownerReviewOverride
                ? QStringLiteral(
                      "Merge as repository owner without the required peer "
                      "review, record the waiver, then permanently delete the "
                      "pull request and its head branch")
                : QStringLiteral(
                      "Merge this pull request, then permanently delete it and "
                      "its head branch"));
    }
    if (m_pullPreviewButton) {
        const bool buildable =
            m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size() &&
            !m_repositories.at(m_repoDetailIndex).localPath.isEmpty() &&
            QFileInfo::exists(m_repositories.at(m_repoDetailIndex).localPath +
                              QStringLiteral("/desktop/CMakeLists.txt"));
        const bool canPreview = have && !archived && !head.isEmpty() && buildable;
        m_pullPreviewButton->setVisible(canPreview);
        m_pullPreviewButton->setEnabled(canPreview);
    }
    const bool conflicted = mergeable && !mergeClean;
    updatePullConflictCard(selectedPull, conflicted, conflictFiles);
    updatePullMergeBlockers(have, open, writable, mergeable, conflictPending,
                            conflicted, reviewBlocks, independentChangesRequested,
                            changesRequestedBy, conflictFiles, base, behind,
                            behindCount);
    const bool aiFixBusy = m_aiFix && m_aiFix->number == m_currentPullNumber;
    const int conflictCount = conflicted ? conflictFiles.size() : 0;
    if (m_pullResolveButton) {
        m_pullResolveButton->setVisible(conflicted);
        m_pullResolveButton->setEnabled(conflicted && !aiFixBusy);
        setPullActionBadge(m_pullResolveButton, conflictCount);
    }
    if (m_pullFixButton) {
        m_pullFixButton->setVisible(conflicted);
        m_pullFixButton->setEnabled(conflicted && !aiFixBusy);
        setPullActionBadge(m_pullFixButton, conflictCount);
    }
    if (m_pullFixModelCombo) {
        m_pullFixModelCombo->setVisible(conflicted);
        m_pullFixModelCombo->setEnabled(conflicted && !aiFixBusy);
    }
    if (m_pullFixConflictsButton) {
        const AgentSession *agent =
            conflicted ? agentSessionForPull(m_currentPullNumber, head) : nullptr;
        const bool continuable =
            agent && !agent->branchName.isEmpty() && !isExternalSession(agent->id);
        const bool agentBusy =
            agent && (agent->status == AgentStatus::Running ||
                      agent->status == AgentStatus::Queued ||
                      runnerForSession(agent->id));
        m_pullFixConflictsButton->setVisible(conflicted && continuable);
        m_pullFixConflictsButton->setEnabled(conflicted && continuable &&
                                             !agentBusy && !aiFixBusy);
        setPullActionBadge(m_pullFixConflictsButton, conflictCount);
        m_pullFixConflictsButton->setToolTip(
            agentBusy
                ? QString::fromUtf8("The agent for this pull request is already "
                                 "running \xE2\x80\x94 watch it on the Agents tab")
                : QStringLiteral(
                      "Ask the %1 session that authored this branch to merge "
                      "`%2` in and resolve the conflicts itself")
                      .arg(agent ? agentProviderName(agent->provider)
                                 : QStringLiteral("agent"),
                           base.isEmpty() ? QStringLiteral("main") : base));
    }
    // AI review: "Review with AI" shows on any open PR with a diff —
    // it only reads the patch, so mirror nodes get it too (their findings travel
    // to the owner's inbox as signed events). "Fix all with AI" appears once
    // unresolved review threads exist; the agent commits to the PR's branch, so
    // it needs a working tree.
    if (m_pullReviewAiButton) {
        const bool reviewable = have && open && !archived &&
                                !patch.trimmed().isEmpty();
        m_pullReviewAiButton->setVisible(reviewable);
        m_pullReviewAiButton->setEnabled(reviewable && !m_aiReview);
    }
    if (m_pullFixAllAiButton) {
        int unresolved = 0;
        if (have && open)
            for (const PullRequest &pr : std::as_const(m_currentPulls))
                if (pr.number == m_currentPullNumber)
                    unresolved = buildPullReviewSnapshot(pr).unresolvedThreads;
        const bool fixable = manageable && open && unresolved > 0;
        m_pullFixAllAiButton->setVisible(fixable);
        m_pullFixAllAiButton->setEnabled(fixable && !m_aiFix && !m_aiReview);
        setPullActionBadge(m_pullFixAllAiButton, fixable ? unresolved : 0);
    }
    if (m_pullEditFileButton)
        m_pullEditFileButton->setEnabled(manageable && open && m_pullFiles &&
                                         m_pullFiles->currentItem());
    if (m_pullDeleteFileButton)
        m_pullDeleteFileButton->setEnabled(manageable && open && m_pullFiles &&
                                           m_pullFiles->currentItem());
    if (m_pullCloseButton)
        m_pullCloseButton->setEnabled((manageable || authorRequest) && open);
    if (m_pullConversationCloseButton) {
        m_pullConversationCloseButton->setVisible(have && open && !archived);
        m_pullConversationCloseButton->setEnabled((manageable || authorRequest) && open);
        m_pullConversationCloseButton->setToolTip(
            QStringLiteral("Close this pull request without merging it"));
    }
    if (m_pullReopenButton) {
        m_pullReopenButton->setVisible(manageable && (closed || merged));
        m_pullReopenButton->setEnabled(manageable && (closed || merged));
    }
    if (m_pullRestoreButton) {
        m_pullRestoreButton->setVisible(writable && have && archived);
        m_pullRestoreButton->setEnabled(writable && have && archived);
    }
    if (m_pullSendToSourceButton) {
        const bool offerSend = !writable && have && open && !archived;
        m_pullSendToSourceButton->setVisible(offerSend);
        m_pullSendToSourceButton->setEnabled(offerSend);
    }
    setPullActionBadge(m_pullLinkIssueButton, have ? linkedIssues : 0);
    if (m_pullLinkIssueButton)
        m_pullLinkIssueButton->setEnabled(have && !archived);
    if (m_pullDeleteButton)
        m_pullDeleteButton->setEnabled(manageable || authorRequest);
    if (m_pullConversationDeleteButton) {
        m_pullConversationDeleteButton->setVisible(have);
        m_pullConversationDeleteButton->setEnabled((manageable || authorRequest) &&
                                                   !m_pullDeleteInProgress);
        m_pullConversationDeleteButton->setToolTip(
            QStringLiteral("Permanently delete this pull request"));
    }
    if (m_pullDeleteBranchButton)
        m_pullDeleteBranchButton->setEnabled(manageable);
    if (m_pullAgentRevisionRow) {
        const bool hasAgent =
            have && !archived &&
            agentSessionForPull(m_currentPullNumber, head) != nullptr;
        m_pullAgentRevisionRow->setVisible(hasAgent);
        if (m_pullSendToAgentButton)
            m_pullSendToAgentButton->setEnabled(hasAgent && writable);
    }
}

// The conflict card under the pull request header: which files conflict and
// whether the author has already been asked to resolve them.
void MainWindow::updatePullConflictCard(const PullRequest *selectedPull, bool conflicted,
                                        const QStringList &conflictFiles)
{
    if (m_pullConflictCard && m_pullConflictDetails) {
        if (!conflicted) {
            m_pullConflictCard->hide();
        } else {
            QString body;
            if (conflictFiles.isEmpty()) {
                body = QStringLiteral(
                    "The patch no longer applies to the current base.");
            } else {
                QStringList items;
                for (const QString &f : std::as_const(conflictFiles))
                    items << QStringLiteral("<li><code>%1</code></li>")
                                 .arg(f.toHtmlEscaped());
                body = QStringLiteral(
                           "%1 conflicting file%2:<ul style='margin:6px 0 0 0;"
                           "-qt-list-indent:1'>%3</ul>")
                           .arg(conflictFiles.size())
                           .arg(conflictFiles.size() == 1 ? QString()
                                                          : QStringLiteral("s"),
                                items.join(QString()));
            }
            const bool noticeSent =
                selectedPull &&
                (pullHasConflictNotice(*selectedPull) ||
                 m_pullConflictNoticesPosted.contains(
                     pullConflictNoticeSessionKey(*selectedPull)));
            const bool ownPull =
                selectedPull && !selectedPull->author.isEmpty() &&
                selectedPull->author == m_profileIdentity.publicKey();
            if (noticeSent)
                body += QStringLiteral(
                    "<br><span style='color:#8b949e'>The author has been asked "
                    "to resolve these conflicts.</span>");
            m_pullConflictDetails->setText(
                QString::fromUtf8(
                    "<b style='color:#f85149'>\xE2\x9A\xA0 Merge conflicts</b>"
                    "<br>%1<br><a href='tab:files' "
                    "style='color:#58a6ff;text-decoration:none'>"
                    "View files changed \xE2\x86\x92</a>")
                    .arg(body));
            m_pullConflictMessageAuthorButton->setVisible(selectedPull &&
                                                          !noticeSent && !ownPull);
            m_pullConflictCard->show();
        }
    }
}

void MainWindow::updatePullMergeBlockers(bool have, bool open, bool writable,
                                         bool mergeable, bool conflictPending,
                                         bool conflicted, bool reviewBlocks,
                                         bool independentChangesRequested,
                                         const QStringList &changesRequestedBy,
                                         const QStringList &conflictFiles,
                                         const QString &base, bool behind,
                                         int behindCount)
{
    // Every reason the merge button is dead, listed above the merge button
    // itself. Open pull requests only: a closed or merged one hides the button
    // and has nothing to explain.
    if (m_pullMergeBlockers) {
        const QString baseName =
            base.isEmpty()
                ? QStringLiteral("the base branch")
                : QStringLiteral("<code>%1</code>").arg(base.toHtmlEscaped());
        QStringList reasons;
        bool pendingOnly = false;
        if (have && open && !writable) {
            reasons << QString::fromUtf8(
                "<b>This node cannot merge it.</b> The repository is read-only "
                "here \xE2\x80\x94 merging needs a local working tree, so merge "
                "from the node that holds the checkout.");
        }
        if (conflictPending) {
            pendingOnly = reasons.isEmpty();
            reasons << QStringLiteral(
                "<b>The conflict check is still running.</b> The button unlocks "
                "as soon as it finishes.");
        } else if (conflicted) {
            const QString detail =
                conflictFiles.isEmpty()
                    ? QStringLiteral("the patch no longer applies to it")
                    : QStringLiteral("%1 file%2 conflict: %3")
                          .arg(conflictFiles.size())
                          .arg(conflictFiles.size() == 1 ? QString()
                                                         : QStringLiteral("s"),
                               conflictFiles.join(QStringLiteral(", "))
                                   .toHtmlEscaped());
            reasons << QString::fromUtf8(
                           "<b>It conflicts with %1</b> \xE2\x80\x94 %2. Resolve "
                           "them on this branch, then merge. "
                           "<a href='tab:files' style='color:#58a6ff;"
                           "text-decoration:none'>View files changed "
                           "\xE2\x86\x92</a>")
                           .arg(baseName, detail);
        }
        if (mergeable && reviewBlocks && independentChangesRequested) {
            reasons << QStringLiteral(
                           "<b>%1 requested changes.</b> That review blocks the "
                           "merge until it is approved or withdrawn.")
                           .arg(changesRequestedBy.join(QStringLiteral(", "))
                                    .toHtmlEscaped());
        } else if (mergeable && reviewBlocks) {
            reasons << QStringLiteral(
                "<b>Nobody but the author has approved it.</b> This repository "
                "requires at least one approving review from someone other than "
                "the pull request's author.");
        }
        if (!have || !open || reasons.isEmpty()) {
            m_pullMergeBlockers->hide();
        } else {
            QStringList items;
            for (const QString &reason : std::as_const(reasons))
                items << QStringLiteral("<li>%1</li>").arg(reason);
            // Being behind never blocks a merge on its own, so it is a footnote
            // rather than a reason — but it is usually what caused the conflict
            // above, and "Update branch" is right there.
            const QString note =
                behind ? QStringLiteral(
                             "<br><span style='color:#8b949e'>This branch is also "
                             "%1 commit%2 behind %3. <a href='action:update' "
                             "style='color:#58a6ff;text-decoration:none'>Update "
                             "branch</a> merges it in.</span>")
                             .arg(behindCount)
                             .arg(behindCount == 1 ? QString()
                                                   : QStringLiteral("s"),
                                  baseName)
                       : QString();
            // Amber while the only thing missing is the conflict check: that
            // clears itself in a moment and is not a problem to act on.
            pendingOnly = pendingOnly && reasons.size() == 1;
            m_pullMergeBlockers->setText(
                QString::fromUtf8(
                    "<b style='color:%1'>\xE2\x9A\xA0 This pull request "
                    "cannot be merged yet \xE2\x80\x94 %2 reason%3</b>"
                    "<ul style='margin:6px 0 0 0;-qt-list-indent:1'>%4</ul>%5")
                    .arg(pendingOnly ? QStringLiteral("#d29922")
                                     : QStringLiteral("#f85149"))
                    .arg(reasons.size())
                    .arg(reasons.size() == 1 ? QString() : QStringLiteral("s"),
                         items.join(QString()), note));
            m_pullMergeBlockers->show();
        }
    }
}

void MainWindow::promptNewPull()
{
    promptNewPullFromSource(QString());
}

// code-health: allow(blocking-wait) the git apply --check wait runs inside the runOffThread worker; the import resumes in its apply callback
void MainWindow::importPatchAsPull()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const QString dir = repoGitDir();
    PullStore store = pullStoreForCurrentRepo();
    if (dir.isEmpty() || !store.canWrite()) {
        setRepoDetailNotice(
            "Importing a patch needs a writable local checkout of this repo.", true);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(
        this, "Import patch as pull request", QDir::homePath(),
        "Patch files (*.patch *.diff);;All files (*)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setRepoDetailNotice("Could not read the patch file.", true);
        return;
    }
    const QByteArray raw = file.readAll();
    file.close();
    const QString patch = QString::fromUtf8(raw);
    if (patch.trimmed().isEmpty()) {
        setRepoDetailNotice("That patch file is empty.", true);
        return;
    }

    QString title;
    for (const QString &line : patch.split(QLatin1Char('\n'))) {
        if (line.startsWith(QLatin1String("Subject:"))) {
            title = line.mid(8).trimmed();
            title.remove(QRegularExpression(QStringLiteral("^\\[PATCH[^\\]]*\\]\\s*")));
            break;
        }
        if (line.startsWith(QLatin1String("diff --git ")))
            break; // reached the diff with no Subject
    }
    if (title.isEmpty())
        title = QFileInfo(path).completeBaseName();

    const QStringList branches = repoBranches();
    const QString base = repoDefaultBranch(branches);

    // `git apply --check --3way` can take seconds on a large patch or tree
    // (up to its 8 s cap), so it runs on a worker and the import resumes here
    // with its answer.
    struct PatchCheck {
        bool clean = true;
        QString detail;
    };
    const int repoIndex = m_repoDetailIndex;
    const QString repoOwner = m_repositories.at(repoIndex).owner;
    const QString repoName = m_repositories.at(repoIndex).name;
    runOffThread<PatchCheck>(
        [dir, path] {
            PatchCheck result;
            QProcess check;
            check.setProgram("git");
            check.setArguments({"-C", dir, "apply", "--check", "--3way", path});
            check.start();
            check.waitForFinished(8000);
            result.clean = check.exitStatus() == QProcess::NormalExit &&
                           check.exitCode() == 0;
            if (!result.clean)
                result.detail =
                    QString::fromUtf8(check.readAllStandardError()).trimmed();
            return result;
        },
        [this, repoIndex, repoOwner, repoName, path, patch, title,
         base](PatchCheck checked) {
            if (repoIndex != m_repoDetailIndex ||
                repoIndex >= m_repositories.size() ||
                m_repositories.at(repoIndex).owner != repoOwner ||
                m_repositories.at(repoIndex).name != repoName) {
                setRepoDetailNotice(
                    "Patch import stopped: another repository was opened while "
                    "the patch was being checked.",
                    true);
                return;
            }
            PullStore store = pullStoreForCurrentRepo();
            if (!store.canWrite()) {
                setRepoDetailNotice(
                    "Importing a patch needs a writable local checkout of this repo.",
                    true);
                return;
            }
            if (!checked.clean) {
                if (QMessageBox::warning(
                        this, "Import patch",
                        QStringLiteral(
                            "This patch does not apply cleanly onto %1:\n\n%2\n\n"
                            "Open the pull request anyway?")
                            .arg(base, checked.detail.isEmpty() ? "(no details)"
                                                                : checked.detail),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
                    QMessageBox::Yes)
                    return;
            }

            QString head = QStringLiteral("imported/") +
                           title.toLower().replace(
                               QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                               QStringLiteral("-"));
            head = head.left(60);
            if (head.endsWith(QLatin1Char('-')))
                head.chop(1);

            QString error;
            const int number = store.createPull(
                title,
                QStringLiteral("Imported from patch file `%1`.")
                    .arg(QFileInfo(path).fileName()),
                base, head, patch, QString(), /*branchBacked=*/false, &error);
            if (number < 0) {
                setRepoDetailNotice(error.isEmpty()
                                        ? "Could not create the pull request."
                                        : error,
                                    true);
                return;
            }
            logSystem(QStringLiteral("Imported patch %1 as pull #%2.")
                          .arg(path)
                          .arg(number));
            setRepoDetailNotice(
                QStringLiteral("Imported patch as pull #%1.").arg(number));
            m_currentPullNumber = number;
            switchToPullTab(number);
            propagateRepoUpdate(m_repoDetailIndex);
            triggerTeammatesForEvent(
                QStringLiteral("new-pulls"),
                QStringLiteral("Pull request #%1 · %2").arg(number).arg(title));
        });
}

void MainWindow::promptNewPullFromDirectory()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    RepositoryRecord &currentRepo = m_repositories[m_repoDetailIndex];
    const QString startDir = currentRepo.localPath.isEmpty()
                                 ? QDir::homePath()
                                 : currentRepo.localPath;
    const QString chosen = QFileDialog::getExistingDirectory(
        this, "Choose a Git repository for this pull request", startDir);
    if (chosen.isEmpty())
        return;
    promptNewPullFromSource(chosen);
}

void MainWindow::promptNewPullFromSource(const QString &sourceDir,
                                         const QString &preferredBase,
                                         const QString &preferredHead)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString dir = sourceDir.trimmed().isEmpty() ? repoGitDir() : sourceDir.trimmed();
    if (dir.isEmpty()) {
        QMessageBox::warning(this, "New pull request",
                             "No local copy of this repository to diff.");
        return;
    }
    const bool fromSource = !sourceDir.trimmed().isEmpty();
    if (fromSource) {
        const bool looksLikeGit =
            QDir(dir).exists(".git") || QDir(dir).exists("HEAD");
        if (!looksLikeGit) {
            QMessageBox::warning(
                this, "New pull request",
                "That folder is not a Git repository. Choose a folder created by "
                "\"git init\" or \"git clone\".");
            return;
        }
    }
    if (m_newPullSourceReading)
        return;
    m_newPullSourceReading = true;
    runOffThread<NewPullSourceRead>(
        [dir, fromSource, preferredHead] {
            NewPullSourceRead read;
            QByteArray out;
            if (fromSource &&
                runGitCapture(dir, {"config", "--get", "remote.origin.url"}, &out,
                              nullptr))
                read.origin = QString::fromUtf8(out).trimmed();
            out.clear();
            if (runGitCapture(dir, {"branch", "--format=%(refname:short)"}, &out,
                              nullptr))
                for (const QString &b :
                     QString::fromUtf8(out).split('\n', Qt::SkipEmptyParts))
                    read.branches << b.trimmed();
            out.clear();
            read.haveCurrentBranch = runGitCapture(
                dir, {"rev-parse", "--abbrev-ref", "HEAD"}, &out, nullptr);
            read.currentBranch = QString::fromUtf8(out).trimmed();
            out.clear();
            if (!preferredHead.isEmpty() &&
                runGitCapture(dir, {"log", "-1", "--format=%s", preferredHead}, &out,
                              nullptr)) {
                read.haveSubject = true;
                read.subject = QString::fromUtf8(out).trimmed();
            }
            return read;
        },
        [this, sourceDir, preferredBase, preferredHead, dir,
         index = m_repoDetailIndex, owner = repo.owner,
         name = repo.name](NewPullSourceRead read) {
            m_newPullSourceReading = false;
            if (m_repoDetailIndex != index || index < 0 ||
                index >= m_repositories.size() ||
                m_repositories.at(index).owner != owner ||
                m_repositories.at(index).name != name)
            {
                flashMessage(QStringLiteral("The repository changed before the new "
                                            "pull request dialog could open; open "
                                            "it again from the repository."),
                             true);
                return;
            }
            showNewPullDialog(sourceDir, preferredBase, preferredHead, dir, read);
        });
}

void MainWindow::showNewPullDialog(const QString &sourceDir,
                                   const QString &preferredBase,
                                   const QString &preferredHead,
                                   const QString &dir,
                                   const NewPullSourceRead &read)
{
    RepositoryRecord currentRepo = m_repositories.at(m_repoDetailIndex);
    if (!sourceDir.trimmed().isEmpty()) {
        QString selectedName = repoNameFromUrl(dir);
        if (!read.origin.isEmpty())
            selectedName = repoNameFromUrl(read.origin);
        const QString currentName =
            repoSegment(currentRepo.name, QStringLiteral("repository"));
        if (selectedName != currentName) {
            QMessageBox::warning(
                this, "New pull request",
                QStringLiteral("That directory appears to be %1, but this page is for %2.")
                    .arg(selectedName, currentName));
            return;
        }
        if (currentRepo.localPath.isEmpty() || !QDir(currentRepo.localPath).exists()) {
            currentRepo.localPath = dir;
            if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size())
                m_repositories[m_repoDetailIndex].localPath = dir;
            saveRepositories();
            refreshRepositoryList();
        }
    }
    const QStringList branches = read.branches;
    if (branches.size() < 1) {
        QMessageBox::warning(this, "New pull request", "This repository has no branches.");
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle("New pull request");
    auto *targetCombo = new QComboBox(&dialog);
    targetCombo->setEditable(true);
    const QString upstreamProject = currentRepo.forkedFrom.trimmed();
    const QString upstreamOwner = upstreamProject.section(QLatin1Char('/'), 0, 0).trimmed();
    const QString upstreamName = upstreamProject.section(QLatin1Char('/'), 1, 1).trimmed();
    const bool hasUpstream = !upstreamOwner.isEmpty() && !upstreamName.isEmpty();
    if (hasUpstream)
        targetCombo->addItem(
            QStringLiteral("%1/%2 (upstream project)").arg(upstreamOwner, upstreamName),
            upstreamOwner);
    targetCombo->addItem(
        QStringLiteral("%1/%2 (your fork)").arg(currentRepo.owner, currentRepo.name),
        currentRepo.owner);
    QSet<QString> targetOwners{currentRepo.owner};
    if (hasUpstream)
        targetOwners.insert(upstreamOwner);
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.name == currentRepo.name && !targetOwners.contains(repo.owner)) {
            targetCombo->addItem(QStringLiteral("%1/%2").arg(repo.owner, repo.name),
                                 repo.owner);
            targetOwners.insert(repo.owner);
        }
    }
    targetCombo->setToolTip(
        "Repository that will receive this pull request. Choose the upstream project "
        "to contribute the change back to the original repository.");
    auto *baseCombo = new QComboBox(&dialog);
    auto *headCombo = new QComboBox(&dialog);
    baseCombo->addItems(branches);
    headCombo->addItems(branches);
    const QString defaultBase = preferredBase.isEmpty()
                                    ? QStringLiteral("main")
                                    : preferredBase;
    const int preferredBaseIndex = baseCombo->findText(defaultBase);
    if (preferredBaseIndex >= 0)
        baseCombo->setCurrentIndex(preferredBaseIndex);
    const int preferredHeadIndex = headCombo->findText(preferredHead);
    if (preferredHeadIndex >= 0) {
        headCombo->setCurrentIndex(preferredHeadIndex);
    } else {
        if (read.haveCurrentBranch) {
            const int currentIndex = headCombo->findText(read.currentBranch);
            if (currentIndex >= 0)
                headCombo->setCurrentIndex(currentIndex);
            else if (branches.size() > 1)
                headCombo->setCurrentIndex(1);
        } else if (branches.size() > 1) {
            headCombo->setCurrentIndex(1);
        }
    }
    auto *titleEdit = new QLineEdit(&dialog);
    titleEdit->setPlaceholderText("Title");
    if (!preferredHead.isEmpty() && read.haveSubject)
        titleEdit->setText(read.subject);
    auto *bodyEdit = new QPlainTextEdit(&dialog);
    bodyEdit->setPlaceholderText("Describe the change\xE2\x80\xA6");
    auto *directionHint = new QLabel(
        "Send changes from <b>Head</b> (your working branch) into <b>Base</b> "
        "(normally <code>main</code>) on the selected target repository.",
        &dialog);
    directionHint->setWordWrap(true);
    auto *form = new QFormLayout;
    form->addRow("Target repository", targetCombo);
    form->addRow("Base (destination)", baseCombo);
    form->addRow("Head (your changes)", headCombo);
    form->addRow("Title", titleEdit);
    form->addRow("Description", bodyEdit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                         &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto *dl = new QVBoxLayout(&dialog);
    dl->addWidget(directionHint);
    dl->addLayout(form);
    dl->addWidget(buttons);
    dialog.resize(520, 420);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString base = baseCombo->currentText();
    const QString head = headCombo->currentText();
    const QString targetText = targetCombo->currentText().trimmed();
    const int targetIndex = targetCombo->findText(targetText, Qt::MatchExactly);
    const QString targetOwner =
        targetIndex >= 0
            ? targetCombo->itemData(targetIndex).toString().trimmed()
            : repoSegment(targetText, QString());
    const QString title = titleEdit->text().trimmed();
    if (targetOwner.isEmpty()) {
        QMessageBox::warning(this, "New pull request", "A target node is required.");
        return;
    }
    if (title.isEmpty()) {
        QMessageBox::warning(this, "New pull request", "A title is required.");
        return;
    }
    if (base == head) {
        QMessageBox::warning(this, "New pull request", "Base and head must differ.");
        return;
    }
    const bool fromRange = sourceDir.trimmed().isEmpty();
    PullRequest pr;
    pr.title = title;
    pr.description = bodyEdit->toPlainText();
    pr.base = base;
    pr.head = head;
    if (m_newPullSourceReading)
        return;
    m_newPullSourceReading = true;
    runOffThread<NewPullDiffRead>(
        [dir, base, head, fromRange] {
            NewPullDiffRead read;
            read.ok = fromRange
                          ? runGitCapture(dir, {"diff", "--binary", base + ".." + head},
                                          &read.diff, &read.error)
                          : buildWorkingTreeDiff(dir, base, &read.diff, &read.error);
            if (read.ok && fromRange)
                runGitCapture(dir, {"format-patch", "--stdout", base + ".." + head},
                              &read.mbox, nullptr);
            return read;
        },
        [this, pr, fromRange, targetOwner, index = m_repoDetailIndex,
         owner = currentRepo.owner, name = currentRepo.name](NewPullDiffRead read) {
            m_newPullSourceReading = false;
            if (m_repoDetailIndex != index || index < 0 ||
                index >= m_repositories.size() ||
                m_repositories.at(index).owner != owner ||
                m_repositories.at(index).name != name)
            {
                flashMessage(QStringLiteral("The repository changed before the pull "
                                            "request could be created; nothing was "
                                            "created. Open the repository and try "
                                            "again."),
                             true);
                return;
            }
            finishNewPull(pr, fromRange, targetOwner, read);
        });
}

void MainWindow::finishNewPull(PullRequest pr, bool fromRange,
                               const QString &targetOwner,
                               const NewPullDiffRead &read)
{
    const RepositoryRecord currentRepo = m_repositories.at(m_repoDetailIndex);
    const QString base = pr.base;
    const QString head = pr.head;
    const QByteArray &diff = read.diff;
    const QString &diffError = read.error;
    if (!read.ok || diff.trimmed().isEmpty()) {
        QMessageBox::warning(this, "New pull request",
                             diffError.isEmpty()
                                 ? QStringLiteral("No differences between %1 and %2.")
                                       .arg(base, head)
                                 : QStringLiteral("Could not compare files: %1")
                                       .arg(diffError));
        return;
    }
    pr.patch = QString::fromUtf8(diff);
    if (fromRange && !read.mbox.isEmpty())
        pr.commits = QString::fromUtf8(read.mbox);

    PullStore store = pullStoreForCurrentRepo();
    if (store.canWrite() && targetOwner == currentRepo.owner) {
        QString error;
        const int number = store.createPull(pr.title, pr.description, pr.base, pr.head,
                                             pr.patch, pr.commits,
                                             /*branchBacked=*/fromRange, &error);
        if (number < 0) {
            QMessageBox::warning(this, "New pull request", error);
            return;
        }
        m_currentPullNumber = number;
        switchToPullTab(number);
        propagateRepoUpdate(m_repoDetailIndex);
        triggerTeammatesForEvent(
            QStringLiteral("new-pulls"),
            QStringLiteral("Pull request #%1 · %2")
                .arg(number)
                .arg(pr.title));
    } else {
        RepositoryRecord targetRepo = currentRepo;
        targetRepo.owner = targetOwner;
        // Cross-node submission: label the head with this node's name so the
        // owner can tell which mirror node the PR came from (two nodes may both
        // submit from "main"). The owner merges from the carried patch/commits,
        // never by resolving head, so "<node>:<branch>" is purely informational
        // on their side. Prefix before signing so the signature covers the label.
        const QString nodeName = accountNameFromInput(m_userName, QString());
        if (!nodeName.isEmpty() && !pr.head.contains(QLatin1Char(':')))
            pr.head = nodeName + QLatin1Char(':') + pr.head;
        store.setSigningRepository(targetRepo.owner, targetRepo.name);
        submitPullToInbox(store.makeSignedPull(pr), targetRepo);
    }
}

void MainWindow::updateCurrentPullBranch()
{
    if (m_currentPullNumber < 0)
        return;
    // Copied out of m_currentPulls before the confirmation prompt: the modal
    // pumps the event loop, and a reload behind it can rewrite the list.
    const int number = m_currentPullNumber;
    QString base;
    QString head;
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        if (pr.number == number) {
            base = pr.base;
            head = pr.head;
            break;
        }
    }
    const QString baseLabel =
        base.isEmpty() ? QStringLiteral("the base branch") : base;
    const QString headLabel =
        head.isEmpty() ? QStringLiteral("its branch") : head;
    if (QMessageBox::question(this, "Update branch",
                              QStringLiteral("Merge %1 into %2 (pull request #%3)?")
                                  .arg(baseLabel, headLabel)
                                  .arg(number)) != QMessageBox::Yes)
        return;
    PullStore store = pullStoreForCurrentRepo();
    QString error;
    if (!store.updateBranchFromBase(number, &error)) {
        QMessageBox warning(QMessageBox::Warning,
                            QStringLiteral("Update branch"), error,
                            QMessageBox::Ok, this);
        // A dirty checkout is the typed update gate doing its job, not a
        // release failure. Classify this exact PullStore refusal at its caller;
        // checkout/merge failures from the same operation remain reportable.
        if (error == QStringLiteral(
                         "Commit or stash local changes before updating the "
                         "branch.")) {
            warning.setProperty(
                forkmesh::kLocalOperationDialogProperty, true);
        }
        warning.exec();
        return;
    }
    logSystem(QStringLiteral("Merged %1 into %2 for pull request #%3.")
                  .arg(baseLabel, headLabel)
                  .arg(number));
    reloadPulls();
}

void MainWindow::mergeCurrentPull()
{
    if (m_currentPullNumber < 0)
        return;
    PullRequest current;
    bool found = false;
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        if (pr.number == m_currentPullNumber) {
            current = pr;
            found = true;
            break;
        }
    }
    if (!found)
        return;
    const bool requirePeerApproval =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
            ? m_repositories.at(m_repoDetailIndex).requirePeerApproval
            : true;
    const bool reviewReady = current.independentReviewGateSatisfied();
    const bool repositoryOwner = currentAccountOwnsPullRepository();
    const bool reviewBlocks = PullMergePolicy::reviewGateBlocks(
        requirePeerApproval, reviewReady, repositoryOwner);
    const bool ownerReviewOverride = PullMergePolicy::ownerOverrideRequired(
        requirePeerApproval, reviewReady, repositoryOwner);
    if (reviewBlocks) {
        QMessageBox::warning(
            this, "Merge pull request",
            current.hasIndependentChangesRequested()
                ? QStringLiteral("Pull request #%1 has an unresolved peer "
                                 "\"request changes\" review. Resolve it before "
                                 "merging.")
                      .arg(m_currentPullNumber)
                : QStringLiteral("Pull request #%1 needs at least one approval "
                                 "from a peer other than its author before "
                                 "merging.")
                .arg(m_currentPullNumber));
        return;
    }
    if (ownerReviewOverride) {
        const QString reviewWarning =
            current.hasIndependentChangesRequested()
                ? QStringLiteral(
                      "A peer has requested changes on pull request #%1. As the "
                      "repository owner, you can merge it anyway, but doing so "
                      "waives the required peer-review gate. The waiver will be "
                      "recorded in the pull-request ledger.\n\nMerge without "
                      "review?")
                      .arg(m_currentPullNumber)
                : QStringLiteral(
                      "No independent peer has approved pull request #%1. As "
                      "the repository owner, you can merge it anyway, but doing "
                      "so waives the required peer-review gate. The waiver will "
                      "be recorded in the pull-request ledger.\n\nMerge without "
                      "review?")
                      .arg(m_currentPullNumber);
        if (QMessageBox::warning(
                this, QStringLiteral("Merge without peer review"), reviewWarning,
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel) != QMessageBox::Yes) {
            return;
        }
    } else if (QMessageBox::question(
                   this, "Merge pull request",
                   QStringLiteral("Apply and merge pull request #%1?")
                       .arg(m_currentPullNumber)) != QMessageBox::Yes) {
        return;
    }
    PullStore store = pullStoreForCurrentRepo();
    QString error;
    if (!store.mergePull(m_currentPullNumber, &error, requirePeerApproval,
                         ownerReviewOverride)) {
        reloadPullsInBackground();
        QMessageBox warning(QMessageBox::Warning,
                            QStringLiteral("Merge pull request"), error,
                            QMessageBox::Ok, this);
        if (forkmesh::pulldecision::decidedPullError(error)
            || forkmesh::pulldecision::localChangesBlockError(error))
            warning.setProperty(forkmesh::kLocalOperationDialogProperty, true);
        warning.exec();
        return;
    }
    afterPullMerged(current, ownerReviewOverride);
}

void MainWindow::afterPullMerged(PullRequest pr, bool ownerReviewOverride)
{
    logSystem(ownerReviewOverride
                  ? QStringLiteral(
                        "Merged pull request #%1 (peer-review gate waived by "
                        "repository owner).")
                        .arg(pr.number)
                  : QStringLiteral("Merged pull request #%1.").arg(pr.number));
    closeIssuesLinkedFromPull(pr);
    fundBountiesForMergedPull(pr);
    autoBountyForMergedPull(pr);
    refreshSourceControl(true);
    reloadPulls();
    markAgentSessionsMerged(pr.number, pr.head, /*mergeVerified=*/true);
    danceGenie();
    if (QSettings().value(kAutoSyncOnMergeSetting, false).toBool()) {
        propagateRepoUpdate(m_repoDetailIndex);
    } else {
        logSystem(QStringLiteral("Merge landed locally — click \"Sync\" to publish "
                                 "it to main (auto-sync-on-merge is off)."));
    }
}

namespace {
class MergeEditorDialog : public QDialog
{
public:
    using QDialog::QDialog;
    bool committing = false;
    void reject() override
    {
        if (!committing)
            QDialog::reject();
    }
};
}

bool MainWindow::runMergeConflictEditor(
    const QString &title, const QString &introHtml, const QString &workTree,
    const QStringList &conflictedFiles, const QString &commitButtonText,
    const std::function<bool(QString *)> &commitFn, bool commitOffThread)
{
#ifdef FORKMESH_WINDOW_TESTS
    if (m_testMergeConflictEditorHook)
        return m_testMergeConflictEditorHook(workTree, conflictedFiles,
                                             commitFn);
#endif
    MergeEditorDialog dlg(this);
    dlg.setWindowTitle(title);
    dlg.resize(960, 640);

    auto *intro = new QLabel(introHtml);
    intro->setObjectName("statusLine");
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);

    auto *fileList = new QListWidget;
    fileList->setObjectName("overviewList");
    enableHoverRowHighlight(fileList);
    fileList->setMinimumWidth(220);

    auto *editor = new QPlainTextEdit;
    editor->setObjectName("codeEditor");
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    applyLogFont(editor);
    new ConflictHighlighter(editor->document());

    auto *oursBtn = new QPushButton(QStringLiteral("Accept ours"));
    auto *theirsBtn = new QPushButton(QStringLiteral("Accept theirs"));
    auto *bothBtn = new QPushButton(QStringLiteral("Accept both"));
    auto *allOursBtn = new QPushButton(QStringLiteral("Accept all ours"));
    auto *allTheirsBtn = new QPushButton(QStringLiteral("Accept all theirs"));
    auto *prevBtn = new QPushButton(QString::fromUtf8("\xE2\x86\x91 Prev"));
    auto *nextBtn = new QPushButton(QString::fromUtf8("\xE2\x86\x93 Next"));
    for (QPushButton *b :
         {oursBtn, theirsBtn, bothBtn, allOursBtn, allTheirsBtn, prevBtn, nextBtn}) {
        b->setObjectName("ghostButton");
        b->setProperty("buttonSize", "sm");
        b->setCursor(Qt::PointingHandCursor);
    }
    oursBtn->setToolTip("Keep our version of this conflict");
    theirsBtn->setToolTip("Take their version of this conflict");
    bothBtn->setToolTip("Keep both sides (ours first, then theirs)");
    allOursBtn->setToolTip("Keep our version of every conflict in every file");
    allTheirsBtn->setToolTip("Take their version of every conflict in every file");
    oursBtn->setObjectName("conflictOursBtn");
    theirsBtn->setObjectName("conflictTheirsBtn");
    allOursBtn->setObjectName("conflictAllOursBtn");
    allTheirsBtn->setObjectName("conflictAllTheirsBtn");
    const bool darkConflict = currentThemeIsDark();
    const auto tintConflictBtn = [](QPushButton *b, const QString &bg,
                                    const QString &border, const QString &fg,
                                    const QString &hoverBg) {
        b->setStyleSheet(Theme::remapSheet(QStringLiteral(
                             "QPushButton#%1 { background:%2; border:1px solid %3;"
                             " color:%4; font-weight:600; padding:3px 10px;"
                             " border-radius:6px; }"
                             "QPushButton#%1:hover { background:%5; color:%4; }")
                             .arg(b->objectName(), bg, border, fg, hoverBg)));
    };
    if (darkConflict) {
        tintConflictBtn(oursBtn, "#0b2a4a", "#1f6feb", "#cae3ff", "#10395f");
        tintConflictBtn(theirsBtn, "#0b3a1e", "#238636", "#aff5b8", "#114a26");
        tintConflictBtn(allOursBtn, "#0b2a4a", "#1f6feb", "#cae3ff", "#10395f");
        tintConflictBtn(allTheirsBtn, "#0b3a1e", "#238636", "#aff5b8", "#114a26");
    } else {
        tintConflictBtn(oursBtn, "#ddf4ff", "#54aeff", "#0969da", "#cae8ff");
        tintConflictBtn(theirsBtn, "#e6ffec", "#4ac26b", "#1a7f37", "#d2f8d9");
        tintConflictBtn(allOursBtn, "#ddf4ff", "#54aeff", "#0969da", "#cae8ff");
        tintConflictBtn(allTheirsBtn, "#e6ffec", "#4ac26b", "#1a7f37", "#d2f8d9");
    }
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->addWidget(oursBtn);
    toolbar->addWidget(theirsBtn);
    toolbar->addWidget(bothBtn);
    toolbar->addWidget(allOursBtn);
    toolbar->addWidget(allTheirsBtn);
    toolbar->addStretch();
    toolbar->addWidget(prevBtn);
    toolbar->addWidget(nextBtn);

    auto *commitBtn = new QPushButton(commitButtonText);
    commitBtn->setObjectName("primaryButton");
    commitBtn->setCursor(Qt::PointingHandCursor);
    auto *cancelBtn = new QPushButton(QStringLiteral("Cancel"));
    cancelBtn->setObjectName("ghostButton");
    cancelBtn->setCursor(Qt::PointingHandCursor);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch();
    buttonRow->addWidget(cancelBtn);
    buttonRow->addWidget(commitBtn);

    auto *editorCol = new QVBoxLayout;
    editorCol->setContentsMargins(0, 0, 0, 0);
    editorCol->addLayout(toolbar);
    editorCol->addWidget(editor, 1);
    auto *editorPane = new QWidget;
    editorPane->setLayout(editorCol);
    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(fileList);
    split->addWidget(editorPane);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({240, 680});

    auto *outer = new QVBoxLayout(&dlg);
    outer->addWidget(intro);
    outer->addWidget(split, 1);
    outer->addLayout(buttonRow);

    auto currentPath = std::make_shared<QString>();
    const auto hasMarkers = [](const QString &text) {
        return text.startsWith(QLatin1String("<<<<<<< ")) ||
               text.contains(QLatin1String("\n<<<<<<< ")) ||
               text.contains(QLatin1String("\n>>>>>>> "));
    };
    const auto readFile = [workTree](const QString &rel) {
        QFile f(workTree + "/" + rel);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    };
    const auto saveCurrent = [=] {
        if (currentPath->isEmpty())
            return;
        QFile f(workTree + "/" + *currentPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(editor->toPlainText().toUtf8());
    };
    const auto refreshStatus = [=] {
        const QString currentText = editor->toPlainText();
        bool allClean = true;
        for (int i = 0; i < fileList->count(); ++i) {
            QListWidgetItem *it = fileList->item(i);
            const QString rel = it->data(Qt::UserRole).toString();
            const bool markers =
                rel == *currentPath ? hasMarkers(currentText) : hasMarkers(readFile(rel));
            it->setText((markers ? QString::fromUtf8("\xE2\x9A\xA0 ")
                                 : QString::fromUtf8("\xE2\x9C\x93 ")) +
                        rel);
            if (markers)
                allClean = false;
        }
        commitBtn->setEnabled(allClean);
        commitBtn->setToolTip(allClean
                                  ? QStringLiteral("Commit the resolved merge")
                                  : QStringLiteral("Resolve every conflict first"));
    };

    // Put a line in view and park the cursor on it. The dialog isn't laid out
    // yet on the first selection, so centring is repeated once the event loop
    // has given the editor its real viewport height.
    const auto focusLine = [=](int line) {
        QTextCursor c = editor->textCursor();
        c.movePosition(QTextCursor::Start);
        if (line > 0)
            c.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor, line);
        editor->setTextCursor(c);
        editor->centerCursor();
        QTimer::singleShot(0, editor, [editor] { editor->centerCursor(); });
    };
    // Conflicts of any file in the list: the visible one comes from the editor
    // (it may hold unsaved edits), the rest from disk.
    const auto conflictsInRow = [=](int row) {
        QList<ConflictRegion> regions;
        if (row < 0 || row >= fileList->count())
            return regions;
        const QString rel = fileList->item(row)->data(Qt::UserRole).toString();
        return findConflicts(
            (rel == *currentPath ? editor->toPlainText() : readFile(rel)).split('\n'));
    };

    for (const QString &rel : conflictedFiles) {
        auto *it = new QListWidgetItem(rel);
        it->setData(Qt::UserRole, rel);
        fileList->addItem(it);
    }

    connect(fileList, &QListWidget::currentItemChanged, &dlg,
            [=](QListWidgetItem *item, QListWidgetItem *) {
                saveCurrent();
                if (!item) {
                    currentPath->clear();
                    editor->clear();
                    return;
                }
                *currentPath = item->data(Qt::UserRole).toString();
                editor->setPlainText(readFile(*currentPath));
                refreshStatus();
                // Open on the first conflict rather than the top of the file.
                const QList<ConflictRegion> regions =
                    findConflicts(editor->toPlainText().split('\n'));
                focusLine(regions.isEmpty() ? 0 : regions.first().startLine);
            });
    connect(editor, &QPlainTextEdit::textChanged, &dlg, [=] { refreshStatus(); });

    const auto applyResolution = [=](int which) {
        QStringList lines = editor->toPlainText().split('\n');
        const QList<ConflictRegion> regions = findConflicts(lines);
        if (regions.isEmpty())
            return;
        const int cursorLine = editor->textCursor().blockNumber();
        int idx = -1;
        for (int i = 0; i < regions.size(); ++i)
            if (cursorLine >= regions.at(i).startLine &&
                cursorLine <= regions.at(i).endLine) {
                idx = i;
                break;
            }
        if (idx < 0)
            for (int i = 0; i < regions.size(); ++i)
                if (regions.at(i).startLine >= cursorLine) {
                    idx = i;
                    break;
                }
        if (idx < 0)
            idx = 0;
        const ConflictRegion r = regions.at(idx);
        const QStringList ours = lines.mid(r.startLine + 1, r.sepLine - r.startLine - 1);
        const QStringList theirs = lines.mid(r.sepLine + 1, r.endLine - r.sepLine - 1);
        QStringList repl = which == 0 ? ours : which == 1 ? theirs : (ours + theirs);
        const QStringList out =
            lines.mid(0, r.startLine) + repl + lines.mid(r.endLine + 1);
        editor->setPlainText(out.join('\n'));
        QTextCursor c = editor->textCursor();
        c.movePosition(QTextCursor::Start);
        c.movePosition(QTextCursor::Down, QTextCursor::MoveAnchor,
                       qMin(r.startLine, qMax(0, out.size() - 1)));
        editor->setTextCursor(c);
    };
    connect(oursBtn, &QPushButton::clicked, &dlg, [=] { applyResolution(0); });
    connect(theirsBtn, &QPushButton::clicked, &dlg, [=] { applyResolution(1); });
    connect(bothBtn, &QPushButton::clicked, &dlg, [=] { applyResolution(2); });

    // ours == 0 takes the pre-separator side of every region, ours == 1 the
    // post-separator one. Regions are rewritten back-to-front so the earlier
    // ones keep the line numbers findConflicts() reported.
    const auto takeAllOneSide = [](const QString &text, int which) {
        QStringList lines = text.split('\n');
        const QList<ConflictRegion> regions = findConflicts(lines);
        for (int i = regions.size() - 1; i >= 0; --i) {
            const ConflictRegion r = regions.at(i);
            const QStringList side =
                which == 0 ? lines.mid(r.startLine + 1, r.sepLine - r.startLine - 1)
                           : lines.mid(r.sepLine + 1, r.endLine - r.sepLine - 1);
            lines = lines.mid(0, r.startLine) + side + lines.mid(r.endLine + 1);
        }
        return lines.join('\n');
    };
    const auto applyToEveryFile = [=](int which) {
        saveCurrent(); // flush the visible editor to disk before re-reading
        for (int i = 0; i < fileList->count(); ++i) {
            const QString rel = fileList->item(i)->data(Qt::UserRole).toString();
            const QString resolved = takeAllOneSide(readFile(rel), which);
            QFile f(workTree + "/" + rel);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(resolved.toUtf8());
        }
        if (!currentPath->isEmpty())
            editor->setPlainText(readFile(*currentPath));
        refreshStatus();
    };
    connect(allOursBtn, &QPushButton::clicked, &dlg, [=] { applyToEveryFile(0); });
    connect(allTheirsBtn, &QPushButton::clicked, &dlg, [=] { applyToEveryFile(1); });

    // Prev/Next walk every remaining conflict in the merge, not just this
    // file's: once the current file is exhausted the jump continues into the
    // next file that still has markers (wrapping around the whole list).
    const auto jump = [=](int dir) {
        const QList<ConflictRegion> regions =
            findConflicts(editor->toPlainText().split('\n'));
        const int cursorLine = editor->textCursor().blockNumber();
        int target = -1;
        if (dir > 0) {
            for (const ConflictRegion &r : regions)
                if (r.startLine > cursorLine) {
                    target = r.startLine;
                    break;
                }
        } else {
            for (int i = regions.size() - 1; i >= 0; --i)
                if (regions.at(i).startLine < cursorLine) {
                    target = regions.at(i).startLine;
                    break;
                }
        }
        if (target >= 0) {
            focusLine(target);
            return;
        }
        // Nothing left this way in this file — look through the others.
        const int count = fileList->count();
        const int row = fileList->currentRow();
        for (int step = 1; step <= count; ++step) {
            const int next = ((row + dir * step) % count + count) % count;
            const QList<ConflictRegion> other = conflictsInRow(next);
            if (other.isEmpty())
                continue;
            const int line =
                dir > 0 ? other.first().startLine : other.last().startLine;
            if (next == row) {
                focusLine(line); // only this file has conflicts: wrap inside it
                return;
            }
            fileList->setCurrentRow(next); // selection loads and focuses the file
            focusLine(line);
            return;
        }
    };
    connect(nextBtn, &QPushButton::clicked, &dlg, [=] { jump(1); });
    connect(prevBtn, &QPushButton::clicked, &dlg, [=] { jump(-1); });

    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    bool committed = false;
    connect(commitBtn, &QPushButton::clicked, &dlg, [&] {
        saveCurrent();
        if (commitOffThread) {
            if (dlg.committing)
                return;
            dlg.committing = true;
            commitBtn->setEnabled(false);
            cancelBtn->setEnabled(false);
            QPointer<QDialog> guard(&dlg);
            const std::function<bool(QString *)> work = commitFn;
            runOffThread<QPair<bool, QString>>(
                [work] {
                    QString err;
                    const bool ok = work(&err);
                    return qMakePair(ok, err);
                },
                [&, guard](QPair<bool, QString> result) {
                    if (!guard)
                        return;
                    dlg.committing = false;
                    cancelBtn->setEnabled(true);
                    if (!result.first) {
                        QMessageBox::warning(
                            &dlg, title,
                            result.second.isEmpty()
                                ? QStringLiteral("Could not commit the merge.")
                                : result.second);
                        refreshStatus();
                        return;
                    }
                    committed = true;
                    dlg.accept();
                });
            return;
        }
        QString err;
        if (!commitFn(&err)) {
            QMessageBox::warning(&dlg, title,
                                 err.isEmpty()
                                     ? QStringLiteral("Could not commit the merge.")
                                     : err);
            refreshStatus();
            return;
        }
        committed = true;
        dlg.accept();
    });

    if (fileList->count() > 0)
        fileList->setCurrentRow(0);
    refreshStatus();
    dlg.exec();
    return committed;
}


void MainWindow::aiFixLog(const QString &text)
{
    if (!m_aiFix || !m_agentStore)
        return;
    if (AgentSession *s = findAgentSession(m_aiFix->sessionId))
        m_agentStore->appendLog(*s, text);
    onAgentLog(m_aiFix->sessionId, text); // live-append if this session is shown
}

void MainWindow::aiFixSetSessionStatus(const QString &status, const QString &error)
{
    if (!m_aiFix || !m_agentStore)
        return;
    AgentSession *s = findAgentSession(m_aiFix->sessionId);
    if (!s)
        return;
    s->status = status;
    s->costUsd = m_aiFix->costUsd;
    s->promptTokens = int(m_aiFix->inTokens);
    s->completionTokens = int(m_aiFix->outTokens);
    s->totalTokens = int(m_aiFix->inTokens + m_aiFix->outTokens);
    if (!error.isEmpty())
        s->lastError = error;
    if (status == AgentStatus::Success || status == AgentStatus::Failed ||
        status == AgentStatus::Stopped)
        s->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_agentStore->saveSession(*s);
    reloadAgents();
    if (m_aiFix->sessionId == m_selectedAgentSessionId)
        showAgentSession(m_aiFix->sessionId);
}

// ---- AI code review ---------------------------------------------
// "Review with AI" sends the PR's diff to a model in one shot. Each finding the
// model reports lands as a signed review thread anchored to the file+line it
// concerns; findings where the model supplied both the original lines and a
// replacement carry a suggestion patch the reviewer applies and commits in one
// click. A summary review event records the run's outcome in the conversation.

void MainWindow::reviewCurrentPullWithAi()
{
    if (m_aiReview) {
        flashMessage("An AI review is already running; wait for it to finish.",
                     true);
        return;
    }
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const int number = m_currentPullNumber;
    PullRequest current;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == number)
            current = pr;
    if (current.number == 0 || current.patch.trimmed().isEmpty()) {
        flashMessage("This pull request has no diff to review.", true);
        return;
    }

    // Provider: a configured API key wins (one request over the diff suffices
    // and the reply is easiest to keep to strict JSON); without one, fall back
    // to the Claude Code CLI, which authenticates through its local login.
    const QString claudeKey =
        QSettings().value(kClaudeApiKeySetting).toString().trimmed();
    const QString openAiKey =
        QSettings().value(kCodexApiKeySetting).toString().trimmed();
    QString provider = QStringLiteral("claude-code");
    QString model;
    if (!claudeKey.isEmpty()) {
        provider = QStringLiteral("claude");
        model = QStringLiteral("claude-opus-5");
    } else if (!openAiKey.isEmpty()) {
        provider = QStringLiteral("openai");
        model = QStringLiteral("gpt-4.1-mini");
    }

    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    AgentSession session;
    session.owner = repo.owner;
    session.name = repo.name;
    session.issueNumber = 0;
    session.issueTitle = QStringLiteral("AI review of PR #%1").arg(number);
    session.provider = provider;
    session.model = model;
    session.prNumber = number;
    session.status = AgentStatus::Running;
    session = m_agentStore->createSession(session);
    session.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    session.beginRun(session.startedAtMs);
    danceGenie();
    m_agentStore->saveSession(session);
    reloadAgents();

    m_aiReview = new AiPullReview;
    m_aiReview->number = number;
    m_aiReview->repoIndex = m_repoDetailIndex;
    m_aiReview->sessionId = session.id;
    m_aiReview->provider = provider;
    m_aiReview->model = model;

    aiReviewLog(QStringLiteral(
                    "==> %1 reviewing pull request #%2 (%3 file(s), +%4 -%5).\n")
                    .arg(agentProviderName(provider))
                    .arg(number)
                    .arg(current.filesChanged)
                    .arg(current.additions)
                    .arg(current.deletions));
    if (m_pullMergeStatus) {
        m_pullMergeStatus->setText(QString::fromUtf8(
            "<span style='color:#58a6ff'>\xF0\x9F\xA4\x96 %1 is reviewing this "
            "pull request\xE2\x80\xA6 findings will be attached to the lines "
            "they concern.</span>").arg(agentProviderName(provider)));
        m_pullMergeStatus->show();
    }
    updatePullActionState();

    const QString prompt =
        buildAiReviewPrompt(current.title, current.description, current.patch);

    if (provider == QLatin1String("claude-code")) {
        aiReviewRunClaudeCode(prompt);
        return;
    }

    const bool claude = provider == QLatin1String("claude");
    constexpr int kReviewOutTokens = 8000;
    QNetworkReply *reply = nullptr;
    if (claude) {
        QJsonObject payload;
        payload.insert("model", m_aiReview->model);
        payload.insert("max_tokens", kReviewOutTokens);
        QJsonArray messages;
        QJsonObject um;
        um.insert("role", "user");
        um.insert("content", prompt);
        messages.append(um);
        payload.insert("messages", messages);
        QNetworkRequest req(
            QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
        req.setRawHeader("x-api-key", claudeKey.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    } else {
        QJsonObject payload;
        payload.insert("model", m_aiReview->model);
        payload.insert("input", prompt);
        payload.insert("max_output_tokens", kReviewOutTokens);
        QNetworkRequest req = openAiRequest(
            QUrl(QStringLiteral("https://api.openai.com/v1/responses")),
            openAiKey);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, claude] {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (!m_aiReview) // torn down (e.g. app closing) — nothing to do
            return;
        if (reply->error() != QNetworkReply::NoError) {
            aiReviewFail(
                QStringLiteral("API error: %1").arg(apiErrorSummary(reply, body)));
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        QString text;
        if (claude) {
            for (const QJsonValue &v : obj.value("content").toArray()) {
                const QJsonObject o = v.toObject();
                if (o.value("type").toString() == QLatin1String("text"))
                    text += o.value("text").toString();
            }
            const QJsonObject usage = obj.value("usage").toObject();
            const qint64 in = usage.value("input_tokens").toInt();
            const qint64 out = usage.value("output_tokens").toInt();
            m_aiReview->inTokens += in;
            m_aiReview->outTokens += out;
            m_aiReview->costUsd += in / 1e6 * 5.0 + out / 1e6 * 25.0; // Opus 4.8
        } else {
            text = openAiResponseText(obj);
            qint64 in = 0, out = 0;
            m_aiReview->costUsd += openAiAskCostUsd(obj, &in, &out);
            m_aiReview->inTokens += in;
            m_aiReview->outTokens += out;
        }
        aiReviewHandleReply(text);
    });
}

void MainWindow::aiReviewRunClaudeCode(const QString &prompt)
{
    if (!m_aiReview)
        return;
    const QString workTree =
        (m_aiReview->repoIndex >= 0 && m_aiReview->repoIndex < m_repositories.size())
            ? writableRecordFor(m_repositories.at(m_aiReview->repoIndex)).localPath
            : QString();
    const QString promptPath = QDir::temp().filePath(
        QStringLiteral("forkmesh-review-%1.md").arg(m_aiReview->number));
    m_aiReview->promptFile = promptPath;
    QFile pf(promptPath);
    if (!pf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        aiReviewFail(QStringLiteral("Could not write the review prompt file."));
        return;
    }
    pf.write(prompt.toUtf8());
    pf.write(QByteArray("\n\nDo NOT modify any file and do NOT run any git "
                        "command - this is a read-only review. Print ONLY the "
                        "JSON array as your final output.\n"));
    pf.close();

    QString promptQuoted = promptPath;
    promptQuoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    promptQuoted = QLatin1Char('\'') + promptQuoted + QLatin1Char('\'');
    QString command = claudeCodeCommandSetting();
    if (command.contains(QStringLiteral("{promptFile}")))
        command.replace(QStringLiteral("{promptFile}"), promptQuoted);
    else
        command += QStringLiteral(" < ") + promptQuoted;

    auto *process = new QProcess(this);
    m_aiReview->process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(workTree.isEmpty() ? QDir::tempPath() : workTree);
    process->setStandardInputFile(QProcess::nullDevice());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    forkmesh::agentcli::widenPath(env);
    process->setProcessEnvironment(env);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (!m_aiReview || m_aiReview->process != process)
            return;
        const QString chunk = QString::fromUtf8(process->readAllStandardOutput());
        m_aiReview->output += chunk;
        aiReviewLog(chunk);
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError err) {
                if (!m_aiReview || m_aiReview->process != process)
                    return;
                if (err == QProcess::FailedToStart) {
                    m_aiReview->process = nullptr;
                    process->deleteLater();
                    QFile::remove(m_aiReview->promptFile);
                    aiReviewFail(QString::fromUtf8(
                        "Could not start the `claude` CLI \xE2\x80\x94 install "
                        "Claude Code or set its command in Settings."));
                }
            });
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus) {
                if (!m_aiReview || m_aiReview->process != process)
                    return;
                const QByteArray tail = process->readAllStandardOutput();
                if (!tail.isEmpty()) {
                    m_aiReview->output += QString::fromUtf8(tail);
                    aiReviewLog(QString::fromUtf8(tail));
                }
                m_aiReview->process = nullptr;
                process->deleteLater();
                QFile::remove(m_aiReview->promptFile);
                if (exitCode != 0) {
                    aiReviewFail(QStringLiteral(
                                     "Claude Code exited with code %1 before "
                                     "finishing the review.")
                                     .arg(exitCode));
                    return;
                }
                aiReviewHandleReply(m_aiReview->output);
            });

    aiReviewLog(QString::fromUtf8(
        "==> Running Claude Code over the pull request diff\xE2\x80\xA6\n"));
    trackProcessActivity(process, QStringLiteral("review"),
                         QStringLiteral("AI review of the pull request diff"));
#ifdef Q_OS_WIN
    process->start(QStringLiteral("cmd"), {QStringLiteral("/c"), command});
#else
    const QString shell = QFile::exists(QStringLiteral("/bin/bash"))
                              ? QStringLiteral("/bin/bash")
                              : QStringLiteral("/bin/sh");
    process->start(shell, {QStringLiteral("-lc"), command});
#endif
}

// Turn the model's reply into review threads on the PR. Owner nodes commit the
// signed events straight into.forkmesh/pulls/<N>/; mirror nodes route them through the
// relay inbox like any hand-written review comment.
void MainWindow::aiReviewHandleReply(const QString &text)
{
    if (!m_aiReview)
        return;
    bool parsed = false;
    QList<AiReviewFinding> findings = parseAiReviewFindings(text, &parsed);
    if (!parsed) {
        aiReviewFail(QStringLiteral(
                         "The model's reply carried no findings JSON:\n%1")
                         .arg(text.left(400)));
        return;
    }
    constexpr int kMaxFindings = 25;
    if (findings.size() > kMaxFindings)
        findings = findings.mid(0, kMaxFindings);

    const int number = m_aiReview->number;
    if (m_aiReview->repoIndex < 0 ||
        m_aiReview->repoIndex >= m_repositories.size()) {
        aiReviewFail(QStringLiteral("The repository is no longer open."));
        return;
    }
    const RepositoryRecord &target = m_repositories.at(m_aiReview->repoIndex);
    const RepositoryRecord &repo = writableRecordFor(target);
    PullStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity,
                    m_userName);
    store.setSigningRepository(target.owner, target.name);
    const int aiReviewRepoIndex = m_aiReview->repoIndex;

    // Findings anchor to the diff the model was shown, like a human
    // reviewer's comments.
    PullRequest reviewedPull;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == number)
            reviewedPull = pr;

    int quickFixes = 0;
    for (const AiReviewFinding &f : std::as_const(findings)) {
        const QString body = QString::fromUtf8("**\xF0\x9F\xA4\x96 AI review "
                                               "\xC2\xB7 %1:** %2")
                                 .arg(f.severity, f.comment);
        const PullAnchor anchor =
            reviewedPull.number == number
                ? pullAnchorFor(reviewedPull, f.path, QStringLiteral("new"),
                                f.lineStart, f.lineEnd)
                : PullAnchor();
        if (!f.suggestionPatch.isEmpty())
            ++quickFixes;
        if (store.canWrite()) {
            QString postError;
            store.addThreadComment(number, f.path, QStringLiteral("new"),
                                   f.lineStart, f.lineEnd, body,
                                   f.suggestionPatch, anchor, &postError);
        } else {
            PullEvent ev;
            ev.type = QStringLiteral("thread-comment");
            ev.path = f.path;
            ev.side = QStringLiteral("new");
            ev.lineStart = f.lineStart;
            ev.lineEnd = f.lineEnd;
            ev.body = body;
            ev.suggestionPatch = f.suggestionPatch;
            ev.patchset = anchor.patchset;
            ev.anchorLines = anchor.anchorLines;
            ev = store.makeSignedEvent(number, ev);
            submitPullEventToInbox(number, ev, aiReviewRepoIndex);
        }
        aiReviewLog(QStringLiteral("==> %1:%2 [%3] %4%5\n")
                        .arg(f.path)
                        .arg(f.lineStart)
                        .arg(f.severity, f.comment.left(120),
                             f.suggestionPatch.isEmpty()
                                 ? QString()
                                 : QStringLiteral(" (quick fix)")));
    }

    // A summary review event so the conversation records the outcome. Posted as
    // "commented" — an AI approving/blocking under the node's own signature
    // would distort the human review summary.
    QString summary;
    if (findings.isEmpty())
        summary = QString::fromUtf8(
            "\xF0\x9F\xA4\x96 AI review found no issues in this diff.");
    else
        summary =
            QString::fromUtf8(
                "\xF0\x9F\xA4\x96 AI review found %1 issue(s); %2 carry a "
                "one-click \"Apply fix & commit\" suggestion. Use \"Fix all "
                "with AI\" to hand the open findings to an agent.")
                .arg(findings.size())
                .arg(quickFixes);
    if (store.canWrite()) {
        QString postError;
        store.addReview(number, QStringLiteral("commented"), summary, &postError);
    } else {
        PullEvent ev;
        ev.type = QStringLiteral("review");
        ev.state = QStringLiteral("commented");
        ev.body = summary;
        ev = store.makeSignedEvent(number, ev);
        submitPullEventToInbox(number, ev, aiReviewRepoIndex);
    }

    aiReviewLog(QStringLiteral("==> Review finished: %1 finding(s), %2 with a "
                               "quick fix (cost ~$%3).\n")
                    .arg(findings.size())
                    .arg(quickFixes)
                    .arg(QString::number(m_aiReview->costUsd, 'f', 4)));
    aiReviewSetSessionStatus(AgentStatus::Success);
    const int repoIndex = m_aiReview->repoIndex;
    delete m_aiReview;
    m_aiReview = nullptr;

    logSystem(QStringLiteral("AI review of pull request #%1 finished.").arg(number));
    if (repoIndex == m_repoDetailIndex) {
        reloadPulls();
        showPull(number);
    }
    flashMessage(findings.isEmpty()
                     ? QStringLiteral("AI review: no issues found on PR #%1.")
                           .arg(number)
                     : QStringLiteral("AI review: %1 finding(s) attached to "
                                      "PR #%2's code.")
                           .arg(findings.size())
                           .arg(number));
}

void MainWindow::aiReviewFail(const QString &message)
{
    if (!m_aiReview)
        return;
    const int number = m_aiReview->number;
    const int repoIndex = m_aiReview->repoIndex;
    aiReviewLog(QStringLiteral("!! %1\n").arg(message));
    aiReviewSetSessionStatus(AgentStatus::Failed, message);
    delete m_aiReview;
    m_aiReview = nullptr;
    flashMessage(QStringLiteral("AI review failed: %1").arg(message), true);
    if (repoIndex == m_repoDetailIndex) {
        reloadPulls();
        showPull(number);
    }
}

void MainWindow::aiReviewLog(const QString &text)
{
    if (!m_aiReview || !m_agentStore)
        return;
    if (AgentSession *s = findAgentSession(m_aiReview->sessionId))
        m_agentStore->appendLog(*s, text);
    onAgentLog(m_aiReview->sessionId, text); // live-append if shown
}

void MainWindow::aiReviewSetSessionStatus(const QString &status,
                                          const QString &error)
{
    if (!m_aiReview || !m_agentStore)
        return;
    AgentSession *s = findAgentSession(m_aiReview->sessionId);
    if (!s)
        return;
    s->status = status;
    s->costUsd = m_aiReview->costUsd;
    s->promptTokens = int(m_aiReview->inTokens);
    s->completionTokens = int(m_aiReview->outTokens);
    s->totalTokens = int(m_aiReview->inTokens + m_aiReview->outTokens);
    if (!error.isEmpty())
        s->lastError = error;
    if (status == AgentStatus::Success || status == AgentStatus::Failed ||
        status == AgentStatus::Stopped)
        s->finishedAtMs = QDateTime::currentMSecsSinceEpoch();
    m_agentStore->saveSession(*s);
    reloadAgents();
    if (m_aiReview->sessionId == m_selectedAgentSessionId)
        showAgentSession(m_aiReview->sessionId);
}

void MainWindow::applyPullSuggestionFix(const QString &threadId)
{
    if (m_currentPullNumber < 0 || threadId.isEmpty())
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    if (m_aiFix) {
        flashMessage("An AI fix is already running; wait for it to finish.",
                     true);
        return;
    }
    PullRequest current;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == m_currentPullNumber)
            current = pr;
    if (current.number == 0)
        return;
    PullReviewThread target;
    const PullReviewSnapshot snapshot = buildPullReviewSnapshot(current);
    for (const PullReviewThread &thread : snapshot.threads)
        if (thread.id == threadId)
            target = thread;
    QString suggestion;
    for (const PullEvent &ev : std::as_const(target.events))
        if (ev.type == QLatin1String("thread-comment") &&
            !ev.suggestionPatch.isEmpty())
            suggestion = ev.suggestionPatch;
    if (target.path.isEmpty() || suggestion.isEmpty()) {
        flashMessage("This thread has no applicable suggestion.", true);
        return;
    }
    if (target.suggestionState == QLatin1String("applied")) {
        flashMessage("This suggestion is already applied.");
        return;
    }
    if (target.outdated()) {
        flashMessage("This suggestion is outdated: the lines it changes are "
                     "no longer in the diff.", true);
        return;
    }
    // The suggestion replaces the lines the thread sits on now, which may
    // have moved since it was written.
    const int applyAtLine =
        target.currentLineStart > 0 ? target.currentLineStart : target.lineStart;

    PullStore store = pullStoreForCurrentRepo();
    if (!store.canWrite()) {
        flashMessage("This repository is read-only on this node.", true);
        return;
    }
    if (m_pullSuggestionApplying) {
        flashMessage("A suggested fix is already being applied.");
        return;
    }
    m_pullSuggestionApplying = true;
    const int number = m_currentPullNumber;
    const int repoIndex = m_repoDetailIndex;
    const QString workTree =
        writableRecordFor(m_repositories.at(m_repoDetailIndex)).localPath;
    const QString path = target.path;
    runOffThread<QString>(
        [store, number, path, applyAtLine, suggestion, threadId,
         workTree]() mutable {
            QString error;
            QString content;
            if (!store.startPullFileEdit(number, path, &content, &error))
                return error.isEmpty() ? QStringLiteral("Could not apply the fix.")
                                       : error;
            if (!applySuggestionToContent(&content, applyAtLine, suggestion,
                                          &error) ||
                !store.finishPullFileEdit(number, path, content, &error)) {
                store.abortConflictMerge();
                return error.isEmpty() ? QStringLiteral("Could not apply the fix.")
                                       : error;
            }
            QString appliedSha;
            for (const PullRequest &p : store.loadAll())
                if (p.number == number && !p.head.isEmpty()) {
                    QByteArray out;
                    if (runGitCapture(workTree, {"rev-parse", p.head}, &out, nullptr))
                        appliedSha = QString::fromUtf8(out).trimmed();
                }
            store.setSuggestionState(number, threadId, QStringLiteral("applied"),
                                     appliedSha,
                                     QStringLiteral("Applied the suggested fix."),
                                     &error);
            store.setThreadState(number, threadId, QStringLiteral("resolved"),
                                 QString(), &error);
            return QString();
        },
        [this, number, repoIndex, workTree](QString failure) {
            m_pullSuggestionApplying = false;
            if (!failure.isEmpty()) {
                QMessageBox::warning(this, "Apply fix", failure);
                return;
            }
            if (repoIndex >= 0 && repoIndex < m_repositories.size() &&
                writableRecordFor(m_repositories.at(repoIndex)).localPath == workTree) {
                if (m_repoDetailIndex == repoIndex) {
                    reloadPulls();
                    if (m_currentPullNumber == number)
                        showPull(number);
                }
                propagateRepoUpdate(repoIndex);
            }
            flashMessage(QStringLiteral("Fix applied and committed to PR #%1's branch.")
                             .arg(number));
        });
}

void MainWindow::fixCurrentPullFindingsWithAgent()
{
    if (m_aiFix) {
        flashMessage("An AI fix is already running; wait for it to finish.",
                     true);
        return;
    }
    if (m_aiReview) {
        flashMessage("Wait for the AI review to finish first.", true);
        return;
    }
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const int number = m_currentPullNumber;
    PullRequest current;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == number)
            current = pr;
    if (current.number == 0)
        return;

    const PullReviewSnapshot snapshot = buildPullReviewSnapshot(current);
    QStringList findingBlocks;
    QStringList paths;
    for (const PullReviewThread &thread : snapshot.threads) {
        if (thread.resolved ||
            thread.suggestionState == QLatin1String("applied"))
            continue;
        QStringList lines;
        lines << QStringLiteral("- %1:%2%3")
                     .arg(thread.path)
                     .arg(thread.lineStart)
                     .arg(thread.lineEnd > thread.lineStart
                              ? QStringLiteral("-%1").arg(thread.lineEnd)
                              : QString());
        for (const PullEvent &ev : thread.events) {
            if (ev.type != QLatin1String("thread-comment") &&
                ev.type != QLatin1String("thread-reply") &&
                ev.type != QLatin1String("line-comment"))
                continue;
            if (!ev.body.trimmed().isEmpty())
                lines << QStringLiteral("  %1").arg(
                    ev.body.trimmed().left(600).replace(
                        QLatin1Char('\n'), QStringLiteral("\n  ")));
            if (!ev.suggestionPatch.isEmpty())
                lines << QStringLiteral("  Suggested fix:\n  %1").arg(
                    QString(ev.suggestionPatch)
                        .replace(QLatin1Char('\n'), QStringLiteral("\n  ")));
        }
        findingBlocks << lines.join(QLatin1Char('\n'));
        if (!thread.path.isEmpty() && !paths.contains(thread.path))
            paths << thread.path;
    }
    if (findingBlocks.isEmpty()) {
        flashMessage("No unresolved review findings to fix.");
        return;
    }

    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString workTree = writableRecordFor(repo).localPath;
    if (workTree.isEmpty()) {
        QMessageBox::warning(this, "Fix all with AI",
                             "This repository is read-only on this node.");
        return;
    }

    auto *store = new PullStore(pullStoreForCurrentRepo());
    QString error;
    if (!store->startPullAgentEdit(number, &error)) {
        delete store;
        QMessageBox::warning(this, "Fix all with AI", error);
        return;
    }
    const QString editTree = store->agentEditWorkTree();

    AgentSession session;
    session.owner = repo.owner;
    session.name = repo.name;
    session.issueNumber = 0;
    session.issueTitle =
        QStringLiteral("Fix review findings on PR #%1").arg(number);
    session.provider = QStringLiteral("claude-code");
    session.prNumber = number;
    session.branchName = current.head.isEmpty()
                             ? QStringLiteral("pull/%1").arg(number)
                             : current.head;
    session.status = AgentStatus::Running;
    session = m_agentStore->createSession(session);
    session.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    session.beginRun(session.startedAtMs);
    danceGenie();
    m_agentStore->saveSession(session);
    m_agentStore->appendLog(
        session,
        QStringLiteral("==> Claude Code fixing %1 review finding(s) on pull "
                       "request #%2.\n")
            .arg(findingBlocks.size())
            .arg(number));

    m_aiFix = new AiConflictFix;
    m_aiFix->store = store;
    m_aiFix->number = number;
    m_aiFix->repoIndex = m_repoDetailIndex;
    m_aiFix->sessionId = session.id;
    m_aiFix->provider = QStringLiteral("claude-code");
    m_aiFix->workTree = editTree.isEmpty() ? workTree : editTree;
    m_aiFix->files = paths;
    m_aiFix->claudeCode = true;
    m_aiFix->agentEdit = true;
    m_aiFix->agentEditFindings = findingBlocks.size();

    QStringList prompt;
    prompt << QStringLiteral(
        "You are addressing code-review findings on a pull request. Its branch "
        "is checked out in this repository with the pull request applied.");
    prompt << QString();
    prompt << QStringLiteral(
        "The findings, each anchored to file:line(s) of the current checkout:");
    prompt << findingBlocks.join(QStringLiteral("\n\n"));
    prompt << QString();
    prompt << QStringLiteral(
        "Edit the files to properly fix every finding. Keep the changes "
        "minimal and in the spirit of the pull request.");
    prompt << QString::fromUtf8(
        "Do NOT run any git command, do NOT commit, and do NOT touch unrelated "
        "code \xE2\x80\x94 ForkMesh commits the result for you once you are "
        "done.");
    m_aiFix->agentEditPrompt = prompt.join(QLatin1Char('\n'));

    if (m_pullMergeStatus) {
        m_pullMergeStatus->setText(QString::fromUtf8(
            "<span style='color:#58a6ff'>\xF0\x9F\xA4\x96 Claude Code is fixing "
            "the review findings\xE2\x80\xA6 watch it on the Agents tab."
            "</span>"));
        m_pullMergeStatus->show();
    }
    updatePullActionState();
    switchToAgentsTab(session.id);
    aiFixRunClaudeCode();
}

// The head branch as this node knows it: a pull request from a fork records
// "owner:branch", and only the branch part names a ref here.
static QString pullHeadBranch(const PullRequest &pull)
{
    return pull.head.section(QLatin1Char(':'), -1, -1).trimmed();
}

// The local branch that actually carries a pull request's code in `gitDir`, or
// empty when this node holds none.
//
// A pull request records the head its author signed, and that name need not
// exist here: a contribution that arrives through the relay is a signed patch,
// not a branch, and PullStore replays it onto the base under the durable
// numbered name `pr/<n>`. Asking git for the signed name alone is what made
// "Fix" refuse cross-node pull requests with "branch is not on this node" while
// their code was sitting right here under the other name.
//
// Which name to trust first differs by kind, exactly as the checks runner
// decides it: a branch-backed pull *is* its branch, while a stored-patch pull
// may keep a stale head name that no longer describes the patch, so there the
// numbered branch is authoritative and the signed name only a last resort.
QString forkmesh::ui::localPullAgentBranch(const QString &gitDir,
                                          const PullRequest &pull)
{
    if (gitDir.trimmed().isEmpty() || pull.number <= 0)
        return QString();
    const auto resolves = [&gitDir](const QString &branch) {
        return repoHasLocalBranch(gitDir, branch);
    };
    const QString head = pullHeadBranch(pull);
    const QString canonical = QStringLiteral("pr/%1").arg(pull.number);
    QStringList candidates;
    if (pull.branchBacked)
        candidates << head << canonical;
    else
        candidates << canonical << head;
    for (const QString &candidate : std::as_const(candidates))
        if (resolves(candidate))
            return candidate;
    return QString();
}

QString MainWindow::pullAgentBranch(const PullRequest &pull) const
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return QString();
    // An agent needs a checkout of its own, which only a working copy can give
    // it, so the branch is looked for where the run will happen rather than in
    // the bare mirror repoGitDir() would fall back to.
    return localPullAgentBranch(
        writableRecordFor(m_repositories.at(m_repoDetailIndex)).localPath, pull);
}

// The task an agent gets for a conflicted pull request. It says which pull
// request, which branches, and which files clash — plus the thing the prompt
// box could never say, that the agent is already standing in a checkout of the
// pull request's own branch, so the fix belongs on that branch and not on a
// fresh one cut from the base. `checkoutBranch` is the branch the run will
// actually hold, which for a pull request that arrived through the relay is the
// numbered `pr/<n>` branch rather than the head its author signed; empty means
// the signed name.
QString MainWindow::pullFixAgentPrompt(const PullRequest &pull,
                                       const QString &checkoutBranch) const
{
    const QString head = pullHeadBranch(pull);
    const QString branch =
        checkoutBranch.trimmed().isEmpty() ? head : checkoutBranch.trimmed();
    const QString base =
        pull.base.trimmed().isEmpty() ? QStringLiteral("main") : pull.base.trimmed();
    QStringList lines;
    lines << QStringLiteral("Resolve the merge conflicts on pull request #%1%2.")
                 .arg(pull.number)
                 .arg(pull.title.isEmpty()
                          ? QString()
                          : QStringLiteral(" (\"%1\")").arg(pull.title));
    if (!branch.isEmpty()) {
        // Naming the signed head when the checkout is the numbered branch would
        // send the agent looking for a branch that is not there, so the two are
        // spelled out whenever they differ.
        lines << (branch == head || head.isEmpty()
                      ? QString::fromUtf8(
                            "You are working in a checkout of that pull "
                            "request's own branch `%1` \xE2\x80\x94 its code is "
                            "already here. Do not create a new branch or "
                            "worktree for this work.")
                            .arg(branch)
                      : QString::fromUtf8(
                            "You are working in a checkout of `%1`, the branch "
                            "this node holds that pull request's code on (it "
                            "was proposed as `%2`) \xE2\x80\x94 its code is "
                            "already here. Do not create a new branch or "
                            "worktree for this work.")
                            .arg(branch, head));
        lines << QStringLiteral("`%1` no longer applies cleanly to `%2`.")
                     .arg(branch, base);
    }
    const auto cached = m_pullConflictCache.constFind(pull.number);
    if (cached != m_pullConflictCache.constEnd() && !cached->conflictFiles.isEmpty())
        lines << QStringLiteral("Conflicting files: %1.")
                     .arg(cached->conflictFiles.join(QStringLiteral(", ")));
    lines << QStringLiteral(
                 "Merge `%1` in, resolve every conflict keeping both sides' "
                 "intent, check that the build and tests still pass, then commit "
                 "the fix to this branch so the pull request merges cleanly. "
                 "Leave the pull request open.")
                 .arg(base);
    return lines.join(QLatin1Char(' '));
}

// The ten agent+model pairs this node runs most, so the picker beside "Fix"
// offers the ones in use here. Each row carries the provider and model the
// launch needs, and the pair is remembered as one settings key.
void MainWindow::refreshPullFixModelCombo()
{
    if (!m_pullFixModelCombo)
        return;
    const QSignalBlocker block(m_pullFixModelCombo);
    const QString wanted =
        m_pullFixModelCombo->count() > 0
            ? m_pullFixModelCombo->currentData().toString()
            : QSettings().value(kPullFixAgentModelSetting).toString();
    m_pullFixModelCombo->clear();
    for (const ComposerModelChoice &choice :
         mostUsedAgentModels(kPullFixModelChoices)) {
        m_pullFixModelCombo->addItem(
            choice.icon, choice.label,
            composerModelKey(choice.provider, choice.model));
        const int row = m_pullFixModelCombo->count() - 1;
        m_pullFixModelCombo->setItemData(row, choice.provider, Qt::UserRole + 1);
        m_pullFixModelCombo->setItemData(row, choice.model, Qt::UserRole + 2);
        m_pullFixModelCombo->setItemData(
            row,
            QString::fromUtf8("%1 \xC2\xB7 %2")
                .arg(choice.label, choice.agentName),
            Qt::ToolTipRole);
    }
    const int idx = m_pullFixModelCombo->findData(wanted);
    m_pullFixModelCombo->setCurrentIndex(
        idx >= 0 ? idx : (m_pullFixModelCombo->count() > 0 ? 0 : -1));
}

QString MainWindow::pullFixProvider() const
{
    if (!m_pullFixModelCombo || m_pullFixModelCombo->currentIndex() < 0)
        return defaultAgentProvider();
    const QString provider =
        m_pullFixModelCombo->currentData(Qt::UserRole + 1).toString();
    return provider.isEmpty() ? defaultAgentProvider() : provider;
}

QString MainWindow::pullFixModel() const
{
    if (!m_pullFixModelCombo || m_pullFixModelCombo->currentIndex() < 0)
        return QString();
    return m_pullFixModelCombo->currentData(Qt::UserRole + 2).toString();
}

// "Fix" on a conflicted pull request. This used to fill the prompt box, and a
// prompt sent from there starts an agent the ordinary way: a new branch cut
// from the base in a new worktree — so the fix landed somewhere other than the
// pull request it was written for, and the conflict stayed (adhoc #2091). The
// button now starts the agent itself, on the pull request's own branch. A
// session already sitting on that branch is the one asked, because it is
// holding that checkout and git cannot hand the branch to a second one.
void MainWindow::fixCurrentPullWithAgent()
{
    if (m_currentPullNumber <= 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    PullRequest current;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == m_currentPullNumber)
            current = pr;
    if (current.number == 0)
        return;
    // Held by name, not by index: the git below services UI events, and a
    // repository list rebuilt under it can renumber or drop rows.
    const QString repoOwner = m_repositories.at(m_repoDetailIndex).owner;
    const QString repoName = m_repositories.at(m_repoDetailIndex).name;
    const QString head = pullHeadBranch(current);
    if (head.isEmpty()) {
        flashMessage(
            QStringLiteral("Pull request #%1 records no head branch, so there is "
                           "no branch for an agent to work on.")
                .arg(current.number),
            true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    // The branch to work on is the one this node actually holds the code on.
    QString branch = pullAgentBranch(current);
    if (branch.isEmpty()) {
        // Nothing resolved: a pull request that arrived as a signed patch has
        // no branch at all until its commits are replayed onto the base. Doing
        // that here is what the checks runner does for the same reason, and it
        // beats telling the operator to fetch a branch no node ever pushed.
        QString materializeError;
        const PullStore store = pullStoreForCurrentRepo();
        if (store.canWrite())
            store.ensurePullHeadRef(current, &materializeError);
        else
            materializeError = QStringLiteral("this repository has no working "
                                              "tree here");
        branch = pullAgentBranch(current);
        if (branch.isEmpty()) {
            // A signed patch is not a branch anybody can fetch, so the
            // launcher's "fetch it first" would send the operator after
            // something no node ever pushed. Say what actually failed.
            flashMessage(
                QStringLiteral(
                    "Pull request #%1's code could not be reconstructed on this "
                    "node%2, so there is no branch for an agent to work on.")
                    .arg(current.number)
                    .arg(materializeError.trimmed().isEmpty()
                             ? QString()
                             : QStringLiteral(" (%1)").arg(
                                   materializeError.trimmed())),
                true, QString(), forkmesh::kLocalOperationNoticeKind);
            return;
        }
    }
    const int repoIndex = repoIndexFor(repoOwner, repoName);
    if (repoIndex < 0)
        return;
    const QString prompt = pullFixAgentPrompt(current, branch);

    const QString provider = pullFixProvider();
    const QString model = pullFixModel();

    if (const AgentSession *attached =
            agentSessionForPull(current.number, current.head);
        attached && !attached->associationOnly &&
        !isExternalSession(attached->id) && attached->branchName == branch) {
        const int sessionId = attached->id;
        const bool busy = attached->status == AgentStatus::Running ||
                          attached->status == AgentStatus::Queued ||
                          runnerForSession(sessionId);
        switchToAgentsTab(sessionId);
        if (busy) {
            flashMessage(
                QStringLiteral("Agent #%1 is already working on this pull "
                               "request's branch.")
                    .arg(sessionId),
                true, QString(), forkmesh::kLocalOperationNoticeKind);
            return;
        }
        // The picked model is honoured where it can be: a different model of
        // the same agent is just the next turn's model. Handing the
        // conversation to a *different* agent is a provider handoff, which the
        // composer's selector exists to do deliberately — this button keeps the
        // session's own agent rather than doing it behind a one-click Fix.
        const QString wantModel = agentIsCodexProvider(attached->provider)
                                      ? codexChatGptModelId(model)
                                      : model;
        if (AgentSession *session = findAgentSession(sessionId);
            session && provider == session->provider &&
            !wantModel.isEmpty() && session->model != wantModel &&
            agentModelMatchesProvider(session->provider, wantModel)) {
            session->model = wantModel;
            if (m_agentStore)
                m_agentStore->saveSession(*session);
        }
        flashMessage(
            QStringLiteral("Agent #%1 already holds this pull request's branch, "
                           "so it is resolving the conflicts there.")
                .arg(sessionId));
        // Same path as the composer: steer the CLI when its process is still
        // live, otherwise resume the session under its own provider and model.
        sendPromptToAgentSession(sessionId, prompt);
        return;
    }

    startAgentOnExistingBranch(
        repoIndex, branch, current.base, current.number,
        QStringLiteral("Resolve conflicts on PR #%1").arg(current.number),
        prompt, provider, model);
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testPullFixModelRows() const
{
    QStringList rows;
    if (!m_pullFixModelCombo)
        return rows;
    for (int row = 0; row < m_pullFixModelCombo->count(); ++row)
        rows << QStringLiteral("%1|%2|%3")
                    .arg(m_pullFixModelCombo->itemText(row),
                         m_pullFixModelCombo->itemData(row, Qt::UserRole + 1)
                             .toString(),
                         m_pullFixModelCombo->itemData(row, Qt::UserRole + 2)
                             .toString());
    return rows;
}

bool MainWindow::testSelectPullFixModel(const QString &provider,
                                        const QString &model)
{
    if (!m_pullFixModelCombo)
        return false;
    const int row = m_pullFixModelCombo->findData(composerModelKey(provider, model));
    if (row < 0)
        return false;
    m_pullFixModelCombo->setCurrentIndex(row);
    return true;
}
#endif

void MainWindow::fixCurrentPullConflictsWithAi(const QString &provider)
{
    if (m_aiFix) {
        flashMessage("An AI conflict fix is already running; wait for it to finish.",
                     true);
        return;
    }
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const int number = m_currentPullNumber;

    // "claude-code" drives the real `claude` CLI (no API key, authenticates via the
    // local login); the two API providers POST each file to their endpoint.
    const bool claudeCode = provider == QLatin1String("claude-code");
    const bool claude = provider == QLatin1String("claude");
    const QString model =
        claudeCode ? QString()
                   : claude ? QStringLiteral("claude-haiku-4-5")
                            : QStringLiteral("gpt-4.1-nano");
    QString apiKey;
    if (!claudeCode) {
        apiKey = (claude ? QSettings().value(kClaudeApiKeySetting)
                         : QSettings().value(kCodexApiKeySetting))
                     .toString()
                     .trimmed();
        if (apiKey.isEmpty()) {
            flashMessage(claude ? "Add a Claude API key in Settings first."
                                : "Add an OpenAI API key in Settings first.",
                         true);
            return;
        }
    }

    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QString workTree = writableRecordFor(repo).localPath;
    if (workTree.isEmpty()) {
        QMessageBox::warning(this, "Fix conflicts",
                             "This repository is read-only on this node.");
        return;
    }

    auto *store = new PullStore(pullStoreForCurrentRepo());
    QStringList conflicted;
    bool resolvedClean = false;
    QString error;
    if (!store->startConflictAgentEdit(
            number, &conflicted, &resolvedClean, &error)) {
        delete store;
        QMessageBox::warning(this, "Fix conflicts", error);
        return;
    }

    AgentSession session;
    session.owner = repo.owner;
    session.name = repo.name;
    session.issueNumber = 0; // PR-scoped, not issue-scoped
    session.issueTitle = QStringLiteral("Resolve conflicts on PR #%1").arg(number);
    session.provider = provider; // "claude" | "openai" | "claude-code"
    session.prNumber = number;
    session.branchName = QStringLiteral("pull/%1").arg(number);
    session.status = AgentStatus::Running;
    session = m_agentStore->createSession(session);
    session.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    session.beginRun(session.startedAtMs);
    danceGenie();
    m_agentStore->saveSession(session);
    m_agentStore->appendLog(
        session,
        claudeCode
            ? QStringLiteral("==> %1 resolving merge conflicts on pull request #%2.\n")
                  .arg(agentProviderName(provider))
                  .arg(number)
            : QStringLiteral(
                  "==> %1 (%2) resolving merge conflicts on pull request #%3.\n")
                  .arg(agentProviderName(provider), model)
                  .arg(number));

    m_aiFix = new AiConflictFix;
    m_aiFix->store = store;
    m_aiFix->number = number;
    m_aiFix->repoIndex = m_repoDetailIndex;
    m_aiFix->sessionId = session.id;
    m_aiFix->provider = provider;
    m_aiFix->model = model;
    m_aiFix->apiKey = apiKey;
    m_aiFix->workTree = store->agentEditWorkTree();
    m_aiFix->files = conflicted;
    m_aiFix->claudeCode = claudeCode;

    if (m_pullMergeStatus) {
        m_pullMergeStatus->setText(QString::fromUtf8(
            "<span style='color:#58a6ff'>\xF0\x9F\xA4\x96 %1 is resolving conflicts\xE2\x80\xA6 "
            "watch it on the Agents tab.</span>").arg(agentProviderName(provider)));
        m_pullMergeStatus->show();
    }
    updatePullActionState();
    switchToAgentsTab(session.id);

    if (resolvedClean) {
        aiFixLog(QStringLiteral(
            "==> Patch applied cleanly with no conflicts left to resolve.\n"));
        aiFixFinish();
        return;
    }

    aiFixLog(QStringLiteral("==> %1 file(s) to resolve: %2\n")
                 .arg(conflicted.size())
                 .arg(conflicted.join(QStringLiteral(", "))));
    if (m_aiFix->claudeCode)
        aiFixRunClaudeCode();
    else
        aiFixResolveNextFile();
}

void MainWindow::fixCurrentPullConflictsWithOriginatingAgent()
{
    if (m_currentPullNumber < 0)
        return;
    QString head, base;
    for (const PullRequest &pr : m_currentPulls) {
        if (pr.number == m_currentPullNumber) {
            head = pr.head;
            base = pr.base;
        }
    }
    const AgentSession *agent = agentSessionForPull(m_currentPullNumber, head);
    if (!agent || agent->branchName.isEmpty() || isExternalSession(agent->id)) {
        flashMessage("No agent session is attached to this pull request.", true);
        return;
    }
    if (agent->status == AgentStatus::Running ||
        agent->status == AgentStatus::Queued || runnerForSession(agent->id)) {
        flashMessage("The agent for this pull request is already running.", true);
        return;
    }

    const int sessionId = agent->id;
    const QString prompt =
        QStringLiteral("Merge `%1` into your branch and resolve all merge conflicts. "
                       "Make sure the build and tests still pass, then commit.")
            .arg(base.isEmpty() ? QStringLiteral("main") : base);
    switchToAgentsTab(sessionId);
    // Same path as the composer: steer the CLI process if it is still live (a
    // finished turn does not tear it down), otherwise resume the session with
    // its own provider and model. A bare continue would no-op against the live
    // transport and leave the message queued forever.
    sendPromptToAgentSession(sessionId, prompt);
}

void MainWindow::aiFixResolveNextFile()
{
    if (!m_aiFix)
        return;
    if (m_aiFix->index >= m_aiFix->files.size()) {
        aiFixFinish();
        return;
    }
    const QString rel = m_aiFix->files.at(m_aiFix->index);
    QFile f(m_aiFix->workTree + QLatin1Char('/') + rel);
    if (!f.open(QIODevice::ReadOnly)) {
        aiFixFail(QStringLiteral("Could not read %1 from the working tree.").arg(rel));
        return;
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();
    if (content.size() > 60000) {
        aiFixFail(QString::fromUtf8(
                      "%1 is too large to auto-resolve \xE2\x80\x94 use \"Resolve "
                      "conflicts\xE2\x80\xA6\" for this one.").arg(rel));
        return;
    }

    aiFixLog(QString::fromUtf8("==> [net] Resolving %1 (%2/%3) with %4\xE2\x80\xA6\n")
                 .arg(rel)
                 .arg(m_aiFix->index + 1)
                 .arg(m_aiFix->files.size())
                 .arg(m_aiFix->model));

    const bool claude = m_aiFix->provider == QLatin1String("claude");
    const QString system = QStringLiteral(
        "You are a careful software engineer resolving a Git merge conflict. You "
        "output only the complete, fully merged file contents.");
    const QString task =
        QString::fromUtf8(
            "The file `%1` contains Git merge conflict markers (<<<<<<<, =======, "
            ">>>>>>>). Resolve every conflict by combining both sides into one "
            "correct, coherent file. Keep all non-conflicting content exactly as "
            "it is. Remove every conflict marker. Output ONLY the complete resolved "
            "file contents \xE2\x80\x94 no explanation, no markdown code fences."
            "\n\n----- BEGIN FILE -----\n%2\n----- END FILE -----")
            .arg(rel, content);
    const int outTok = qBound(1024, content.size() / 3 + 1024, 16000);

    QNetworkReply *reply = nullptr;
    if (claude) {
        QJsonObject payload;
        payload.insert("model", m_aiFix->model);
        payload.insert("max_tokens", outTok);
        QJsonArray messages;
        QJsonObject um;
        um.insert("role", "user");
        um.insert("content", system + "\n\n" + task);
        messages.append(um);
        payload.insert("messages", messages);
        QNetworkRequest req(QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
        req.setRawHeader("x-api-key", m_aiFix->apiKey.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    } else {
        QJsonObject payload;
        payload.insert("model", m_aiFix->model);
        payload.insert("instructions", system);
        payload.insert("input", task);
        payload.insert("max_output_tokens", outTok);
        QNetworkRequest req =
            openAiRequest(QUrl(QStringLiteral("https://api.openai.com/v1/responses")),
                          m_aiFix->apiKey);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, claude] {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        if (!m_aiFix) // torn down (e.g. app closing) — nothing to do
            return;
        if (reply->error() != QNetworkReply::NoError) {
            aiFixFail(QStringLiteral("API error: %1").arg(apiErrorSummary(reply, body)));
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        QString text;
        if (claude) {
            for (const QJsonValue &v : obj.value("content").toArray()) {
                const QJsonObject o = v.toObject();
                if (o.value("type").toString() == QLatin1String("text"))
                    text += o.value("text").toString();
            }
            const QJsonObject usage = obj.value("usage").toObject();
            const qint64 in = usage.value("input_tokens").toInt();
            const qint64 out = usage.value("output_tokens").toInt();
            m_aiFix->inTokens += in;
            m_aiFix->outTokens += out;
            m_aiFix->costUsd += in / 1e6 * 1.0 + out / 1e6 * 5.0; // Haiku 4.5 rates
        } else {
            text = openAiResponseText(obj);
            qint64 in = 0, out = 0;
            m_aiFix->costUsd += openAiAskCostUsd(obj, &in, &out);
            m_aiFix->inTokens += in;
            m_aiFix->outTokens += out;
        }
        aiFixApplyResolved(text);
    });
}

void MainWindow::aiFixRunClaudeCode()
{
    if (!m_aiFix)
        return;

    const QString promptPath =
        m_aiFix->workTree + (m_aiFix->agentEdit
                                 ? QStringLiteral("/.forkmesh-review-fix-prompt.md")
                                 : QStringLiteral("/.forkmesh-conflict-prompt.md"));
    m_aiFix->promptFile = promptPath;
    QString promptText;
    if (m_aiFix->agentEdit) {
        promptText = m_aiFix->agentEditPrompt;
    } else {
        QStringList prompt;
        prompt << QStringLiteral(
            "You are resolving Git merge conflicts in this repository checkout.");
        prompt << QStringLiteral("These files contain conflict markers "
                                 "(<<<<<<<, =======, >>>>>>>):");
        for (const QString &rel : std::as_const(m_aiFix->files))
            prompt << QStringLiteral("  - %1").arg(rel);
        prompt << QString();
        prompt << QStringLiteral(
            "Edit each of those files so every conflict is resolved by combining both "
            "sides into one correct, coherent result. Remove every conflict marker and "
            "keep all non-conflicting content exactly as it is.");
        prompt << QString::fromUtf8(
            "Do NOT run any git command, do NOT commit, and do NOT touch any other "
            "file \xE2\x80\x94 ForkMesh commits the result for you once you are done.");
        promptText = prompt.join(QLatin1Char('\n'));
    }
    QFile pf(promptPath);
    if (!pf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        aiFixFail(QStringLiteral("Could not write the agent prompt file."));
        return;
    }
    pf.write(promptText.toUtf8());
    pf.close();

    QString promptQuoted = promptPath;
    promptQuoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    promptQuoted = QLatin1Char('\'') + promptQuoted + QLatin1Char('\'');
    QString command = claudeCodeCommandSetting();
    if (!m_aiFix->model.isEmpty())
        command += QStringLiteral(" --model ") + m_aiFix->model;
    if (command.contains(QStringLiteral("{promptFile}")))
        command.replace(QStringLiteral("{promptFile}"), promptQuoted);
    else
        command += QStringLiteral(" < ") + promptQuoted;

    auto *process = new QProcess(this);
    m_aiFix->process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(m_aiFix->workTree);
    process->setStandardInputFile(QProcess::nullDevice());

    // Claude Code authenticates through its own login; strip any inherited API key
    // so it never silently uses a stale/foreign one, and widen PATH to the usual
    // user install dirs (matches AgentRunner).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("ANTHROPIC_API_KEY"));
    forkmesh::agentcli::widenPath(env);
    process->setProcessEnvironment(env);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        if (!m_aiFix || m_aiFix->process != process)
            return;
        aiFixLog(QString::fromUtf8(process->readAllStandardOutput()));
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError err) {
                if (!m_aiFix || m_aiFix->process != process)
                    return;
                if (err == QProcess::FailedToStart) {
                    m_aiFix->process = nullptr;
                    process->deleteLater();
                    QFile::remove(m_aiFix->promptFile);
                    aiFixFail(QString::fromUtf8(
                        "Could not start the `claude` CLI \xE2\x80\x94 install Claude "
                        "Code or set its command in Settings."));
                }
            });
    connect(process, &QProcess::finished, this,
            [this, process](int exitCode, QProcess::ExitStatus) {
                if (!m_aiFix || m_aiFix->process != process)
                    return;
                const QByteArray tail = process->readAllStandardOutput();
                if (!tail.isEmpty())
                    aiFixLog(QString::fromUtf8(tail));
                const QString workTree = m_aiFix->workTree;
                const QStringList files = m_aiFix->files;
                const bool agentEdit = m_aiFix->agentEdit;
                m_aiFix->process = nullptr;
                process->deleteLater();
                QFile::remove(m_aiFix->promptFile);
                if (exitCode != 0) {
                    aiFixFail(QStringLiteral(
                                  "Claude Code exited with code %1 before %2.")
                                  .arg(exitCode)
                                  .arg(agentEdit
                                           ? QStringLiteral("fixing the findings")
                                           : QStringLiteral(
                                                 "resolving the conflicts")));
                    return;
                }
                if (!agentEdit) {
                    for (const QString &rel : files) {
                        QFile f(workTree + QLatin1Char('/') + rel);
                        if (!f.open(QIODevice::ReadOnly))
                            continue;
                        const QString text = QString::fromUtf8(f.readAll());
                        if (text.contains(QStringLiteral("\n<<<<<<< ")) ||
                            text.startsWith(QStringLiteral("<<<<<<< ")) ||
                            text.contains(QStringLiteral("\n>>>>>>> "))) {
                            aiFixFail(QString::fromUtf8(
                                          "Claude Code left conflict markers in %1 "
                                          "\xE2\x80\x94 resolve it manually instead.")
                                          .arg(rel));
                            return;
                        }
                    }
                }
                aiFixLog(agentEdit
                             ? QStringLiteral("==> Claude Code finished; "
                                              "committing the review fixes.\n")
                             : QStringLiteral("==> Claude Code finished; "
                                              "committing the resolution.\n"));
                aiFixFinish();
            });

    aiFixLog(m_aiFix->agentEdit
                 ? QString::fromUtf8("==> Running Claude Code over the pull "
                                  "request's branch\xE2\x80\xA6\n")
                 : QString::fromUtf8(
                       "==> Running Claude Code over the conflict tree\xE2\x80\xA6\n"));
    trackProcessActivity(process, QStringLiteral("agent"),
                         QStringLiteral("Claude Code is editing the branch"));
#ifdef Q_OS_WIN
    process->start(QStringLiteral("cmd"), {QStringLiteral("/c"), command});
#else
    const QString shell = QFile::exists(QStringLiteral("/bin/bash"))
                              ? QStringLiteral("/bin/bash")
                              : QStringLiteral("/bin/sh");
    process->start(shell, {QStringLiteral("-lc"), command});
#endif
}

void MainWindow::aiFixApplyResolved(const QString &resolvedIn)
{
    if (!m_aiFix)
        return;
    const QString rel = m_aiFix->files.at(m_aiFix->index);
    QString resolved = resolvedIn;
    if (resolved.startsWith(QStringLiteral("```"))) {
        const int nl = resolved.indexOf(QLatin1Char('\n'));
        if (nl >= 0)
            resolved = resolved.mid(nl + 1);
        if (resolved.endsWith(QStringLiteral("```")))
            resolved.chop(3);
        else if (resolved.endsWith(QStringLiteral("```\n")))
            resolved.chop(4);
    }
    if (resolved.trimmed().isEmpty()) {
        aiFixFail(QStringLiteral("The model returned no content for %1.").arg(rel));
        return;
    }
    if (resolved.contains(QStringLiteral("<<<<<<< ")) ||
        resolved.contains(QStringLiteral("\n>>>>>>> ")) ||
        resolved.startsWith(QStringLiteral(">>>>>>> "))) {
        aiFixFail(QString::fromUtf8(
                      "The model left conflict markers in %1 \xE2\x80\x94 resolve it "
                      "manually instead.").arg(rel));
        return;
    }
    if (!resolved.endsWith(QLatin1Char('\n')))
        resolved.append(QLatin1Char('\n'));
    QFile out(m_aiFix->workTree + QLatin1Char('/') + rel);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        aiFixFail(QStringLiteral("Could not write the resolved %1.").arg(rel));
        return;
    }
    out.write(resolved.toUtf8());
    out.close();
    aiFixLog(QStringLiteral("==> Resolved %1.\n").arg(rel));
    m_aiFix->index++;
    aiFixResolveNextFile();
}

void MainWindow::aiFixFinish()
{
    if (!m_aiFix || m_aiFixFinishing)
        return;
    if (m_aiFix->branchMerge) {
        const QString dir = m_aiFix->workTree;
        const QString branch = m_aiFix->branch;
        const QString restore = m_aiFix->restoreBranch;
        m_aiFixFinishing = true;
        runOffThread<QString>(
            [dir, branch, restore] {
                QString error;
                if (!commitResolvedMerge(dir, &error))
                    return error;
                if (!restore.isEmpty() && restore != branch)
                    runGitCapture(dir, {"checkout", restore}, nullptr, nullptr);
                return QString();
            },
            [this, fix = m_aiFix](QString failure) {
                m_aiFixFinishing = false;
                if (m_aiFix != fix)
                    return;
                if (!failure.isEmpty()) {
                    aiFixFail(failure);
                    return;
                }
                aiFixFinishBranchMerge();
            });
        return;
    }
    aiFixFinishPull();
}

void MainWindow::aiFixFinishBranchMerge()
{
    const QString branch = m_aiFix->branch;
    const QString base = m_aiFix->baseBranch;
    const int repoIndex = m_aiFix->repoIndex;
    const double cost = m_aiFix->costUsd;
    aiFixLog(QStringLiteral("==> Committed the merge of %1 into %2 (cost ~$%3).\n")
                 .arg(base, branch, QString::number(cost, 'f', 4)));
    aiFixSetSessionStatus(AgentStatus::Success);
    delete m_aiFix;
    m_aiFix = nullptr;
    logSystem(QStringLiteral("AI resolved conflicts merging %1 into %2.")
                  .arg(base, branch));
    if (repoIndex == m_repoDetailIndex) {
        loadBranchesAndTags();
        loadBranchesPanel();
    }
    if (repoIndex >= 0)
        propagateRepoUpdate(repoIndex);
    flashMessage(
        QStringLiteral("Resolved conflicts: %1 merged into %2.").arg(base, branch));
}

void MainWindow::aiFixFinishPull()
{
    const int number = m_aiFix->number;
    const int repoIndex = m_aiFix->repoIndex;
    const bool agentEdit = m_aiFix->agentEdit;
    QString error;
    const bool committed =
        agentEdit
            ? m_aiFix->store->finishPullAgentEdit(
                  number,
                  QStringLiteral("pull #%1: apply AI review fixes").arg(number),
                  &error)
            : m_aiFix->store->finishConflictMerge(number, &error);
    if (!committed) {
        aiFixFail(error.isEmpty() ? QStringLiteral("Could not commit the fix.")
                                  : error);
        return;
    }
    aiFixLog(QStringLiteral(
                 "==> Committed the %1 to pull request #%2's branch "
                 "(cost ~$%3).\n")
                 .arg(agentEdit ? QStringLiteral("review fixes")
                                : QStringLiteral("conflict fix"))
                 .arg(number)
                 .arg(QString::number(m_aiFix->costUsd, 'f', 4)));
    aiFixSetSessionStatus(AgentStatus::Success);

    if (agentEdit) {
        QString commentError;
        m_aiFix->store->addComment(
            number,
            QString::fromUtf8(
                "\xF0\x9F\xA4\x96 An agent worked through %1 unresolved review "
                "finding(s) and committed fixes to this pull request's branch "
                "\xE2\x80\x94 re-check the threads and resolve the ones that "
                "are addressed.")
                .arg(m_aiFix->agentEditFindings),
            &commentError);
    }

    delete m_aiFix->store;
    delete m_aiFix;
    m_aiFix = nullptr;

    logSystem(agentEdit
                  ? QStringLiteral("AI fixed review findings on pull request "
                                   "#%1's branch; re-check the threads.")
                        .arg(number)
                  : QStringLiteral(
                        "AI resolved conflicts on pull request #%1's branch; it "
                        "is updated and ready to merge.")
                        .arg(number));
    if (repoIndex == m_repoDetailIndex) {
        reloadPulls();
        showPull(number);
    }
    if (repoIndex >= 0)
        propagateRepoUpdate(repoIndex);
    flashMessage(agentEdit
                     ? QStringLiteral(
                           "Review fixes on PR #%1 committed to its branch.")
                           .arg(number)
                     : QStringLiteral("Conflicts on PR #%1 fixed and committed.")
                           .arg(number));
}

void MainWindow::aiFixFail(const QString &message)
{
    if (!m_aiFix || m_aiFixFinishing)
        return;
    if (m_aiFix->branchMerge) {
        const QString dir = m_aiFix->workTree;
        const QString branch = m_aiFix->branch;
        const QString restore = m_aiFix->restoreBranch;
        aiFixLog(QStringLiteral("!! %1\n").arg(message));
        aiFixSetSessionStatus(AgentStatus::Failed, message);
        m_aiFixFinishing = true;
        runOffThread<bool>(
            [dir, branch, restore] {
                runGitCapture(dir, {"merge", "--abort"}, nullptr, nullptr);
                if (!restore.isEmpty() && restore != branch)
                    runGitCapture(dir, {"checkout", restore}, nullptr, nullptr);
                return true;
            },
            [this, fix = m_aiFix, message](bool) {
                m_aiFixFinishing = false;
                if (m_aiFix != fix)
                    return;
                aiFixFailBranchMerge(message);
            });
        return;
    }
    aiFixFailPull(message);
}

void MainWindow::aiFixFailBranchMerge(const QString &message)
{
    const int repoIndex = m_aiFix->repoIndex;
    delete m_aiFix;
    m_aiFix = nullptr;
    flashMessage(QStringLiteral("AI conflict fix failed: %1").arg(message), true);
    if (repoIndex == m_repoDetailIndex) {
        loadBranchesAndTags();
        loadBranchesPanel();
    }
}

void MainWindow::aiFixFailPull(const QString &message)
{
    const int number = m_aiFix->number;
    const int repoIndex = m_aiFix->repoIndex;
    const bool agentEdit = m_aiFix->agentEdit;
    aiFixLog(QStringLiteral("!! %1\n").arg(message));
    aiFixSetSessionStatus(AgentStatus::Failed, message);
    m_aiFix->store->abortConflictMerge(); // restore the working tree + drop the branch

    delete m_aiFix->store;
    delete m_aiFix;
    m_aiFix = nullptr;

    flashMessage(QStringLiteral("%1 failed: %2")
                     .arg(agentEdit ? QStringLiteral("AI review fix")
                                    : QStringLiteral("AI conflict fix"),
                          message),
                 true);
    if (repoIndex == m_repoDetailIndex) {
        reloadPulls();
        showPull(number);
    }
}

void MainWindow::resolveCurrentPullConflicts()
{
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    PullRequest current;
    bool found = false;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == m_currentPullNumber) {
            current = pr;
            found = true;
            break;
        }
    if (!found)
        return;
    const int number = m_currentPullNumber;
    const QString workTree =
        writableRecordFor(m_repositories.at(m_repoDetailIndex)).localPath;
    if (workTree.isEmpty()) {
        QMessageBox::warning(this, "Resolve conflicts",
                             "This repository is read-only on this node.");
        return;
    }

    const auto finalizeResolved = [this, current] {
        logSystem(QStringLiteral("Resolved conflicts on pull request #%1's branch; "
                                 "it is updated and ready to merge.")
                      .arg(current.number));
        reloadPulls();
        propagateRepoUpdate(m_repoDetailIndex);
    };

    PullStore store = pullStoreForCurrentRepo();
    QStringList conflicted;
    bool resolvedClean = false;
    QString error;
    if (!store.startConflictMerge(number, &conflicted, &resolvedClean, &error)) {
        QMessageBox::warning(this, "Resolve conflicts", error);
        return;
    }
    if (resolvedClean) {
        finalizeResolved();
        return;
    }

    const QString intro = QString::fromUtf8(
        "Resolve each conflict, then commit the fix to the pull request's branch. "
        "<b>Ours</b> is your base branch; <b>theirs</b> is the pull request. You "
        "can also edit the text directly. The pull request stays open and becomes "
        "ready to merge \xE2\x80\x94 your base branch is left untouched.");
    const bool committed = runMergeConflictEditor(
        QString::fromUtf8("Resolve conflicts \xE2\x80\x94 pull #%1").arg(number),
        intro, workTree, conflicted, QStringLiteral("Commit to branch"),
        [&store, number](QString *err) { return store.finishConflictMerge(number, err); });
    if (committed) {
        finalizeResolved();
    } else {
        store.abortConflictMerge();
        logSystem(QStringLiteral("Cancelled conflict resolution for pull #%1.").arg(number));
        reloadPulls();
    }
}

void MainWindow::editCurrentPullFile()
{
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    bool found = false;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == m_currentPullNumber) {
            found = true;
            break;
        }
    if (!found)
        return;
    const int number = m_currentPullNumber;
    if (writableRecordFor(m_repositories.at(m_repoDetailIndex)).localPath.isEmpty()) {
        QMessageBox::warning(this, "Edit file",
                             "This repository is read-only on this node.");
        return;
    }
    QListWidgetItem *item = m_pullFiles ? m_pullFiles->currentItem() : nullptr;
    if (!item) {
        QMessageBox::information(this, "Edit file",
                                 "Select a file from this pull request to edit.");
        return;
    }
    const QString relPath = item->data(Qt::UserRole).toString();

    PullStore store = pullStoreForCurrentRepo();
    QString content;
    QString error;
    if (!store.startPullFileEdit(number, relPath, &content, &error)) {
        QMessageBox::warning(this, "Edit file", error);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(
        QString::fromUtf8("Edit %1 \xE2\x80\x94 pull #%2").arg(relPath).arg(number));
    dlg.resize(900, 620);

    auto *intro = new QLabel(
        QString::fromUtf8("Editing <b>%1</b>. Saving commits the change to this pull "
                       "request's branch \xE2\x80\x94 the PR stays open and ready "
                       "to merge; your base branch is left untouched.")
            .arg(relPath.toHtmlEscaped()));
    intro->setObjectName("statusLine");
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);

    auto *editor = new QPlainTextEdit;
    editor->setObjectName("codeEditor");
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    applyLogFont(editor);
    editor->setPlainText(content);

    auto *saveBtn = new QPushButton(QStringLiteral("Commit to branch"));
    saveBtn->setObjectName("primaryButton");
    saveBtn->setCursor(Qt::PointingHandCursor);
    auto *cancelBtn = new QPushButton(QStringLiteral("Cancel"));
    cancelBtn->setObjectName("ghostButton");
    cancelBtn->setCursor(Qt::PointingHandCursor);
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch();
    buttonRow->addWidget(cancelBtn);
    buttonRow->addWidget(saveBtn);

    auto *outer = new QVBoxLayout(&dlg);
    outer->addWidget(intro);
    outer->addWidget(editor, 1);
    outer->addLayout(buttonRow);

    connect(cancelBtn, &QPushButton::clicked, &dlg, &QDialog::reject);
    bool committed = false;
    connect(saveBtn, &QPushButton::clicked, &dlg, [&] {
        QString err;
        if (store.finishPullFileEdit(number, relPath, editor->toPlainText(), &err)) {
            committed = true;
            dlg.accept();
            return;
        }
        QMessageBox::warning(&dlg, "Edit file", err);
        dlg.reject();
    });

    dlg.exec();
    if (committed) {
        logSystem(QStringLiteral("Committed an edit to %1 on pull request #%2's "
                                 "branch; it is updated and ready to merge.")
                      .arg(relPath)
                      .arg(number));
        reloadPulls();
        propagateRepoUpdate(m_repoDetailIndex);
    } else {
        store.abortConflictMerge();
        reloadPulls();
    }
}

void MainWindow::deleteCurrentPullFile()
{
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    bool found = false;
    for (const PullRequest &pr : std::as_const(m_currentPulls))
        if (pr.number == m_currentPullNumber) {
            found = true;
            break;
        }
    if (!found)
        return;
    const int number = m_currentPullNumber;
    if (writableRecordFor(m_repositories.at(m_repoDetailIndex)).localPath.isEmpty()) {
        QMessageBox::warning(this, "Delete file",
                             "This repository is read-only on this node.");
        return;
    }
    QListWidgetItem *item = m_pullFiles ? m_pullFiles->currentItem() : nullptr;
    if (!item) {
        QMessageBox::information(this, "Delete file",
                                 "Select a file from this pull request to delete.");
        return;
    }
    const QString relPath = item->data(Qt::UserRole).toString();
    if (QMessageBox::warning(
            this, "Delete file",
            QStringLiteral("Delete \"%1\" on pull request #%2's branch? The deletion "
                           "is committed to the PR (which stays open and ready to "
                           "merge); your base branch is left untouched.")
                .arg(relPath)
                .arg(number),
            QMessageBox::Ok | QMessageBox::Cancel) != QMessageBox::Ok)
        return;

    PullStore store = pullStoreForCurrentRepo();
    QString error;
    if (store.deletePullFile(number, relPath, &error)) {
        logSystem(QStringLiteral("Deleted %1 on pull request #%2's branch; it is "
                                 "updated and ready to merge.")
                      .arg(relPath)
                      .arg(number));
        reloadPulls();
        propagateRepoUpdate(m_repoDetailIndex);
    } else {
        store.abortConflictMerge();
        QMessageBox::warning(this, "Delete file",
                             error.isEmpty() ? "Could not delete the file." : error);
        reloadPulls();
    }
}

void MainWindow::closeIssuesLinkedFromPull(const PullRequest &pr)
{
    const QList<int> closed = closeIssuesForMerge(
        issuesLinkedFromPull(pr),
        QStringLiteral("Closed by merged pull request #%1.").arg(pr.number),
        QStringLiteral("pull request #%1").arg(pr.number));

    if (closed.size() == 1)
        flashMessage(QStringLiteral("Closed issue #%1 from merged pull request.")
                         .arg(closed.first()));
    else if (closed.size() > 1)
        flashMessage(QStringLiteral("Closed %1 issues from merged pull request.")
                         .arg(closed.size()));
}

QList<int> MainWindow::closeIssuesForMerge(const QList<int> &numbers,
                                           const QString &comment, const QString &via)
{
    return closeIssuesForMerge(issueStoreForCurrentRepo(), numbers, comment, via);
}

QList<int> MainWindow::closeIssuesForMerge(IssueStore store,
                                           const QList<int> &numbers,
                                           const QString &comment, const QString &via)
{
    if (!store.canWrite() || numbers.isEmpty())
        return {};

    QHash<int, Issue> byNumber;
    for (const Issue &issue : store.loadAll())
        byNumber.insert(issue.number, issue);

    QList<int> closed;
    for (const int number : numbers) {
        const Issue issue = byNumber.value(number);
        if (issue.number <= 0 || issue.status == QLatin1String("closed"))
            continue;

        QString err;
        if (!store.addComment(number, comment, {}, &err)) {
            logSystem(QStringLiteral("Issue #%1: could not link %2: %3")
                          .arg(number)
                          .arg(via, err));
            continue;
        }
        if (!store.setStatus(number, QStringLiteral("closed"), &err)) {
            logSystem(QStringLiteral("Issue #%1: could not close after %2: %3")
                          .arg(number)
                          .arg(via, err));
            continue;
        }
        logSystem(
            QStringLiteral("Closed issue #%1 via %2.").arg(number).arg(via));
        closed.append(number);
    }

    if (!closed.isEmpty()) {
        reloadIssues();
        updateRepoIssueCount();
    }
    return closed;
}

QList<int> MainWindow::issuesLinkedFromPull(const PullRequest &pr) const
{
    QSet<int> linked;
    if (const AgentSession *session = agentSessionForPull(pr.number, pr.head))
        if (session->issueNumber > 0)
            linked.insert(session->issueNumber);
    static const QRegularExpression issueRefRe(
        QStringLiteral("\\bissue[-\\s]+#?(\\d+)\\b|"
                       "\\b(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?)\\b\\s*:?\\s*#(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList haystackParts{pr.title, pr.description, pr.head, pr.base};
    for (const PullEvent &ev : pr.events)
        if (!ev.body.isEmpty())
            haystackParts << ev.body;
    const QString haystack = haystackParts.join('\n');
    auto it = issueRefRe.globalMatch(haystack);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int number =
            (match.captured(1).isEmpty() ? match.captured(2) : match.captured(1)).toInt();
        if (number > 0)
            linked.insert(number);
    }
    QList<int> result = linked.values();
    std::sort(result.begin(), result.end());
    return result;
}

// code-health: allow(deep-nested-loop) the issue loop only enters the one issue being asked about; below it is one regex pass per event body
QList<int> MainWindow::pullsLinkedToIssue(int issueNumber) const
{
    if (issueNumber <= 0)
        return {};
    QSet<int> linked;
    for (const PullRequest &pr : m_currentPulls)
        if (issuesLinkedFromPull(pr).contains(issueNumber))
            linked.insert(pr.number);
    static const QRegularExpression pullRefRe(
        QStringLiteral("\\b(?:pull[-\\s]request|pr)[-\\s]*#?(\\d+)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    for (const Issue &issue : m_currentIssues) {
        if (issue.number != issueNumber)
            continue;
        for (const IssueEvent &ev : issue.events) {
            if (ev.body.isEmpty())
                continue;
            auto pit = pullRefRe.globalMatch(ev.body);
            while (pit.hasNext()) {
                const int number = pit.next().captured(1).toInt();
                if (number > 0)
                    linked.insert(number);
            }
        }
        break;
    }
    QList<int> result = linked.values();
    std::sort(result.begin(), result.end());
    return result;
}

void MainWindow::postIssueLinkComment(int issueNumber, const QString &body)
{
    if (issueNumber <= 0)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        if (!store.addComment(issueNumber, body, {}, &error))
            logSystem(QStringLiteral("Issue #%1: could not post link note: %2")
                          .arg(issueNumber)
                          .arg(error));
        return;
    }
    // Read-only mirror: deliver a signed comment to the maintainer's inbox.
    const int idx = issuesRepoIndex();
    if (idx < 0 || !m_networkAccess)
        return;
    const RepositoryRecord &repo = m_repositories.at(idx);
    IssueEvent ev;
    ev.type = QStringLiteral("comment");
    ev.body = body;
    ev = store.makeSignedEvent(issueNumber, ev);
    QJsonObject eventJson = ev.toJson();
    eventJson.insert("body", ev.body);
    const QJsonObject payload{{"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", issueNumber},
                              {"event", eventJson}};
    QNetworkRequest request(issuesApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void MainWindow::postPullLinkComment(int pullNumber, const QString &body)
{
    if (pullNumber <= 0)
        return;
    PullStore store = pullStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        if (!store.addComment(pullNumber, body, &error))
            logSystem(QStringLiteral("Pull request #%1: could not post link note: %2")
                          .arg(pullNumber)
                          .arg(error));
        return;
    }
    PullEvent ev;
    ev.type = QStringLiteral("comment");
    ev.body = body;
    ev = store.makeSignedEvent(pullNumber, ev);
    submitPullEventToInbox(pullNumber, ev);
}

void MainWindow::linkAgentPullToIssue(const AgentSession &session, int prNumber)
{
    if (session.issueNumber <= 0 || prNumber <= 0)
        return;
    const int ri = repoIndexFor(session.owner, session.name);
    if (ri < 0)
        return;
    const RepositoryRecord &repo = writableRecordFor(m_repositories.at(ri));
    IssueStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity, m_userName);
    if (!store.canWrite())
        return;
    QString error;
    if (!store.addComment(session.issueNumber,
                          QStringLiteral("Linked pull request #%1.").arg(prNumber),
                          {}, &error)) {
        if (m_agentStore)
            m_agentStore->appendLog(
                session,
                QStringLiteral("!! Could not link PR #%1 to issue #%2: %3\n")
                    .arg(prNumber)
                    .arg(session.issueNumber)
                    .arg(error));
        return;
    }
    if (ri == m_repoDetailIndex)
        reloadIssues();
}

void MainWindow::linkPullToIssueFromIssuePage()
{
    if (m_currentIssueNumber <= 0) {
        setIssueInlineNotice("Open an issue first.", true);
        return;
    }
    const int issueNumber = m_currentIssueNumber;
    const QList<int> linked = pullsLinkedToIssue(issueNumber);
    const QSet<int> already(linked.cbegin(), linked.cend());
    QStringList labels;
    QList<int> numbers;
    for (const PullRequest &pr : m_currentPulls) {
        if (already.contains(pr.number))
            continue;
        labels << QStringLiteral("#%1  %2").arg(pr.number).arg(pr.title);
        numbers << pr.number;
    }
    int chosen = -1;
    if (numbers.isEmpty()) {
        bool ok = false;
        const int n = QInputDialog::getInt(
            this, QStringLiteral("Link pull request"),
            QStringLiteral("Pull request number to link to issue #%1:").arg(issueNumber),
            1, 1, 1000000, 1, &ok);
        if (!ok)
            return;
        chosen = n;
    } else {
        bool ok = false;
        const QString pick = QInputDialog::getItem(
            this, QStringLiteral("Link pull request"),
            QStringLiteral("Link a pull request to issue #%1:").arg(issueNumber),
            labels, 0, false, &ok);
        if (!ok || pick.isEmpty())
            return;
        chosen = numbers.at(labels.indexOf(pick));
    }
    if (chosen <= 0)
        return;
    postIssueLinkComment(issueNumber,
                         QStringLiteral("Linked pull request #%1.").arg(chosen));
    postPullLinkComment(chosen,
                        QStringLiteral("Linked issue #%1.").arg(issueNumber));
    reloadIssues();
    reloadPulls();
    showIssue(issueNumber);
    flashMessage(QStringLiteral("Linked pull request #%1 to issue #%2.")
                     .arg(chosen)
                     .arg(issueNumber));
}

void MainWindow::linkIssueToPullFromPullPage()
{
    if (m_currentPullNumber <= 0) {
        flashMessage(QStringLiteral("Open a pull request first."));
        return;
    }
    const int pullNumber = m_currentPullNumber;
    QSet<int> already;
    for (const PullRequest &pr : m_currentPulls)
        if (pr.number == pullNumber) {
            const QList<int> linked = issuesLinkedFromPull(pr);
            already = QSet<int>(linked.cbegin(), linked.cend());
        }
    QStringList labels;
    QList<int> numbers;
    for (const Issue &issue : m_currentIssues) {
        if (already.contains(issue.number) || issue.isDeleted())
            continue;
        labels << QStringLiteral("#%1  %2").arg(issue.number).arg(issue.title);
        numbers << issue.number;
    }
    int chosen = -1;
    if (numbers.isEmpty()) {
        bool ok = false;
        const int n = QInputDialog::getInt(
            this, QStringLiteral("Link issue"),
            QStringLiteral("Issue number to link to pull request #%1:").arg(pullNumber),
            1, 1, 1000000, 1, &ok);
        if (!ok)
            return;
        chosen = n;
    } else {
        bool ok = false;
        const QString pick = QInputDialog::getItem(
            this, QStringLiteral("Link issue"),
            QStringLiteral("Link an issue to pull request #%1:").arg(pullNumber),
            labels, 0, false, &ok);
        if (!ok || pick.isEmpty())
            return;
        chosen = numbers.at(labels.indexOf(pick));
    }
    if (chosen <= 0)
        return;
    postPullLinkComment(pullNumber,
                        QStringLiteral("Linked issue #%1.").arg(chosen));
    postIssueLinkComment(chosen,
                         QStringLiteral("Linked pull request #%1.").arg(pullNumber));
    reloadIssues();
    reloadPulls();
    showPull(pullNumber);
    flashMessage(QStringLiteral("Linked issue #%1 to pull request #%2.")
                     .arg(chosen)
                     .arg(pullNumber));
}

void MainWindow::fundBountiesForMergedPull(const PullRequest &pr)
{
    Q_UNUSED(pr);
    return;

#if 0 // Historical Worker-held bounty escrow implementation; never compiled.
    const int idx = issuesRepoIndex();
    if (idx < 0 || !m_networkAccess)
        return;
    const RepositoryRecord repo = m_repositories.at(idx);
    if (!hasOwnerSigningCapability(repo.owner))
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite())
        return;

    QHash<int, Issue> byNumber;
    for (const Issue &issue : store.loadAll())
        byNumber.insert(issue.number, issue);

    for (const int number : issuesLinkedFromPull(pr)) {
        const Issue issue = byNumber.value(number);
        if (issue.number <= 0 || issue.bountyUsd <= 0 ||
            issue.bountyStatus == QLatin1String("paid"))
            continue;
        const double amount = issue.bountyUsd;
        const QString question =
            QStringLiteral("Issue #%1 has a $%2 bounty. Show the funding QR now?\n\n"
                           "Send the SOL to the escrow address; on payout 90% goes "
                           "to the pull request author and 10% to the ForkMesh "
                           "treasury.")
                .arg(number)
                .arg(QString::number(amount, 'f', 2));
        if (QMessageBox::question(this, QStringLiteral("Fund bounty"), question) !=
            QMessageBox::Yes)
            continue;

        const QString payeeNode = pr.authorName.trimmed().toLower();
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QByteArray canonical =
            ("forkmesh-bounty-create-v1\n" + repo.owner + "\n" + repo.name + "\n" +
             QString::number(number) + "\n" + payeeNode + "\n" + ts).toUtf8();
        const QString sig = m_profileIdentity.signData(canonical);
        const QJsonObject payload{{"action", "create"},
                                  {"owner", repo.owner},
                                  {"repo", repo.name},
                                  {"number", number},
                                  {"amountUsd", amount},
                                  {"payeeNode", payeeNode},
                                  {"ts", ts},
                                  {"sig", sig}};
        QNetworkRequest request(bountyApiUrl(repo));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply *reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        const QString payeeDisplay = pr.authorName.trimmed();
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, repo, number, amount, payeeDisplay] {
                    const QByteArray body = reply->readAll();
                    reply->deleteLater();
                    const QJsonObject obj = QJsonDocument::fromJson(body).object();
                    const QString address = obj.value("address").toString();
                    if (reply->error() != QNetworkReply::NoError || address.isEmpty()) {
                        flashMessage(
                            QStringLiteral("Could not create the bounty deposit for "
                                           "#%1: %2")
                                .arg(number)
                                .arg(obj.value("error").toString(
                                    reply->errorString())),
                            true);
                        return;
                    }
                    const QString uri = obj.value("uri").toString(
                        QStringLiteral("solana:%1").arg(address));
                    const QString amountSol = obj.value("amountSol").toString();
                    IssueStore writeStore = issueStoreForCurrentRepo();
                    QString error;
                    writeStore.setBounty(number, amount, address,
                                         QStringLiteral("open"), &error);
                    logSystem(QString::fromUtf8("Bounty escrow for issue #%1 ready to "
                                             "fund ($%2 \xE2\x89\x88 %3 SOL).")
                                  .arg(number)
                                  .arg(QString::number(amount, 'f', 2))
                                  .arg(amountSol));
                    if (m_repoDetailIndex == issuesRepoIndex())
                        reloadIssues();
                    showBountyQrDialog(repo, number, uri, address, amount,
                                       amountSol, QString(), payeeDisplay);
                });
    }
#endif
}

void MainWindow::autoBountyForMergedPull(const PullRequest &pr)
{
    // Migrate stale preferences without contacting the frozen custody API.
    // A future PR-reward implementation must provide an externally signed,
    // independently verifiable transfer contract before this hook is enabled.
    Q_UNUSED(pr);
    QSettings legacySettings;
    const bool wasEnabled =
        legacySettings.value(kAutoPrBountyEnabledSetting, false).toBool();
    const bool usedWallet =
        legacySettings.value(kAutoPrBountyModeSetting).toString() ==
        QLatin1String("wallet");
    legacySettings.setValue(kAutoPrBountyEnabledSetting, false);
    legacySettings.setValue(kAutoPrBountyModeSetting,
                            QStringLiteral("perPr"));
    if (wasEnabled || usedWallet)
        logSystem(QStringLiteral(
            "Legacy automatic PR bounty funding was disabled; no Worker-held "
            "wallet or escrow request was sent."));
    return;

#if 0 // Historical Worker-held automatic bounty implementation; never compiled.
    // reward every merged PR's author with the configured fixed
    // bounty, independent of any issue bounty. Only the repo owner can create a
    // bounty (the worker requires an owner signature), so this is a no-op on a
    // node that doesn't own the repo.
    if (!QSettings().value(kAutoPrBountyEnabledSetting, false).toBool())
        return;
    if (!m_networkAccess || !m_profileIdentity.isValid())
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    if (repo.owner.isEmpty() || repo.owner != accountOwner() ||
        !hasOwnerSigningCapability(repo.owner))
        return;
    const QString payeeNode = pr.authorName.trimmed().toLower();
    if (payeeNode.isEmpty())
        return;
    const double amount = QSettings().value(kAutoPrBountyAmountSetting, 1.0).toDouble();
    if (amount < 1.0)
        return;
    const bool walletMode =
        QSettings().value(kAutoPrBountyModeSetting).toString() ==
        QLatin1String("wallet");
    const int number = pr.number;

    // Owner-signed create, keyed to the PR (kind "pr"). The canonical matches the
    // issue-bounty flow (it binds owner/repo/number/payee); "pr" only affects the
    // worker's storage key so an issue and a PR sharing a number don't collide.
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-bounty-create-v1\n" + repo.owner + "\n" + repo.name + "\n" +
         QString::number(number) + "\n" + payeeNode + "\n" + ts).toUtf8();
    const QJsonObject payload{{"action", "create"},
                              {"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", number},
                              {"kind", QStringLiteral("pr")},
                              {"amountUsd", amount},
                              {"payeeNode", payeeNode},
                              {"fromWallet", walletMode},
                              {"ts", ts},
                              {"sig", m_profileIdentity.signData(canonical)}};
    QNetworkRequest request(bountyApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    const QString payeeDisplay = pr.authorName.trimmed();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, number, amount, walletMode, payeeDisplay] {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        if (reply->error() != QNetworkReply::NoError) {
            const QString err = obj.value("error").toString(reply->errorString());
            if (err == QLatin1String("insufficient_wallet_balance"))
                flashMessage(
                    QStringLiteral("Pull #%1 merged, but the inbuilt bounty wallet "
                                   "is low on SOL — top it up in Settings to pay "
                                   "its $%2 reward.")
                        .arg(number)
                        .arg(QString::number(amount, 'f', 2)),
                    true);
            else if (err == QLatin1String("no_wallet"))
                flashMessage(
                    QStringLiteral("Pull #%1 merged, but no inbuilt bounty wallet is "
                                   "funded yet — set one up in Settings.")
                        .arg(number),
                    true);
            else if (err == QLatin1String("payee_unresolved"))
                flashMessage(
                    QStringLiteral("Pull #%1 merged, but its author has no Solana "
                                   "payout address, so no bounty was paid.")
                        .arg(number),
                    true);
            else
                flashMessage(
                    QStringLiteral("Could not reward pull #%1: %2").arg(number).arg(err),
                    true);
            return;
        }
        if (walletMode) {
            logSystem(QStringLiteral("Rewarded pull #%1's author with a $%2 bounty "
                                     "from the inbuilt wallet (tx %3).")
                          .arg(number)
                          .arg(QString::number(amount, 'f', 2))
                          .arg(obj.value("payoutSig").toString().left(12)));
            flashMessage(QStringLiteral("Paid pull #%1's author a $%2 bounty from the "
                                        "inbuilt wallet.")
                             .arg(number)
                             .arg(QString::number(amount, 'f', 2)));
            return;
        }
        const QString address = obj.value("address").toString();
        if (address.isEmpty()) {
            flashMessage(QStringLiteral("Could not create the reward deposit for "
                                        "pull #%1.").arg(number),
                         true);
            return;
        }
        const QString uri = obj.value("uri").toString(
            QStringLiteral("solana:%1").arg(address));
        const QString amountSol = obj.value("amountSol").toString();
        logSystem(QString::fromUtf8("Reward escrow for pull #%1 ready to fund "
                                    "($%2 \xE2\x89\x88 %3 SOL).")
                      .arg(number)
                      .arg(QString::number(amount, 'f', 2))
                      .arg(amountSol));
        showBountyQrDialog(repo, number, uri, address, amount, amountSol,
                           QStringLiteral("pr"), payeeDisplay);
    });
#endif
}

void MainWindow::pollBountyPayout(const RepositoryRecord &repo, int number,
                                  double amount, const QString &kind)
{
    Q_UNUSED(repo);
    Q_UNUSED(number);
    Q_UNUSED(amount);
    Q_UNUSED(kind);
    return;

#if 0 // Historical status polling could trigger a custodial payout; disabled.
    if (!m_networkAccess)
        return;
    const bool isPr = kind == QLatin1String("pr");
    auto *attempts = new int(0);
    auto *timer = new QTimer(this);
    timer->setInterval(8000);
    connect(timer, &QTimer::timeout, this,
            [this, repo, number, amount, isPr, attempts, timer] {
        if (!m_networkAccess || ++(*attempts) > 75) { // ~10 minutes
            timer->stop();
            timer->deleteLater();
            delete attempts;
            return;
        }
        QJsonObject payload{{"action", "status"},
                            {"owner", repo.owner},
                            {"repo", repo.name},
                            {"number", number}};
        if (isPr)
            payload.insert("kind", QStringLiteral("pr"));
        QNetworkRequest request(bountyApiUrl(repo));
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QNetworkReply *reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, repo, number, amount, isPr, attempts, timer] {
            const QByteArray body = reply->readAll();
            reply->deleteLater();
            const QJsonObject obj = QJsonDocument::fromJson(body).object();
            if (obj.value("status").toString() != QLatin1String("paid"))
                return;
            timer->stop();
            timer->deleteLater();
            delete attempts;
            const QString subject = isPr ? QStringLiteral("pull request #%1")
                                         : QStringLiteral("issue #%1");
            if (!isPr) {
                IssueStore writeStore = issueStoreForCurrentRepo();
                QString error;
                writeStore.setBounty(number, amount,
                                     obj.value("payee").toString(),
                                     QStringLiteral("paid"), &error);
                if (m_repoDetailIndex == issuesRepoIndex())
                    reloadIssues();
            }
            logSystem(QStringLiteral("Bounty for %1 funded and split to the "
                                     "author + treasury (tx %2).")
                          .arg(subject.arg(number))
                          .arg(obj.value("payoutSig").toString().left(12)));
            flashMessage(QStringLiteral("Bounty for %1 paid out to the author "
                                        "+ treasury.")
                             .arg(subject.arg(number)));
        });
    });
    timer->start();
#endif
}


bool MainWindow::currentPullAuthoredHere() const
{
    const QString key = m_profileIdentity.publicKey();
    if (m_currentPullNumber < 0 || key.isEmpty())
        return false;
    for (const PullRequest &pr : m_currentPulls)
        if (pr.number == m_currentPullNumber)
            return !pr.archivedOnCodeBranch && pr.author == key &&
                   (pr.status == QLatin1String("open") ||
                    pr.status == QLatin1String("closed"));
    return false;
}

void MainWindow::requestCurrentPullLifecycle(const QString &type)
{
    if (!currentPullAuthoredHere())
        return;
    const int number = m_currentPullNumber;
    const bool deleting = type == QLatin1String("delete");
    const QString title = deleting ? QStringLiteral("Delete pull request")
                                   : QStringLiteral("Close pull request");
    const QString prompt =
        deleting ? QStringLiteral("Delete your pull request #%1? The repository "
                                  "owner's node removes it when it next syncs.")
                       .arg(number)
                 : QStringLiteral("Close your pull request #%1 without merging it?")
                       .arg(number);
    if (QMessageBox::question(this, title, prompt,
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes)
        return;
    PullEvent ev;
    ev.type = deleting ? QStringLiteral("delete") : QStringLiteral("status");
    if (!deleting)
        ev.state = QStringLiteral("closed");
    PullStore store = pullStoreForCurrentRepo();
    ev = store.makeSignedEvent(number, ev);
    submitPullEventToInbox(number, ev);
}

void MainWindow::closeCurrentPull()
{
    if (m_currentPullNumber < 0)
        return;
    PullStore store = pullStoreForCurrentRepo();
    if (!store.canWrite()) {
        requestCurrentPullLifecycle(QStringLiteral("status"));
        return;
    }
    QString error;
    if (!store.setStatus(m_currentPullNumber, "closed", &error))
        QMessageBox::warning(this, "Close pull request", error);
    reloadPulls();
}

void MainWindow::reopenCurrentPull()
{
    if (m_currentPullNumber < 0)
        return;
    PullStore store = pullStoreForCurrentRepo();
    QString error;
    if (!store.setStatus(m_currentPullNumber, "open", &error))
        QMessageBox::warning(this, "Reopen pull request", error);
    reloadPulls();
}

void MainWindow::restoreCurrentArchivedPull()
{
    if (m_currentPullNumber < 0)
        return;
    const int number = m_currentPullNumber;
    const auto archived = std::find_if(
        m_currentPulls.cbegin(), m_currentPulls.cend(),
        [number](const PullRequest &pull) {
            return pull.number == number && pull.archivedOnCodeBranch;
        });
    if (archived == m_currentPulls.cend())
        return;

    PullStore store = pullStoreForCurrentRepo();
    QString error;
    if (!store.restoreArchivedPull(number, &error)) {
        QMessageBox::warning(
            this, QStringLiteral("Restore pull request"),
            error.isEmpty()
                ? QStringLiteral("Could not restore pull request #%1.").arg(number)
                : error);
        return;
    }
    logSystem(QStringLiteral(
                  "Restored archived pull request #%1 to the current pull ledger.")
                  .arg(number));
    reloadPulls();
    showPull(number);
    setRepoDetailNotice(
        QStringLiteral("Pull request #%1 is restored and ready to manage.")
            .arg(number));
}

void MainWindow::sendCurrentPullToSource()
{
    if (m_currentPullNumber < 0 || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const PullRequest *pr = nullptr;
    for (const PullRequest &candidate : std::as_const(m_currentPulls)) {
        if (candidate.number == m_currentPullNumber) {
            pr = &candidate;
            break;
        }
    }
    if (!pr)
        return;
    // The PR was synced from the mirror with its original author/signature intact;
    // deliver it as-authored so the owner's inbox can verify it. Without a
    // signature there's nothing the source of truth would accept.
    if (pr->sig.isEmpty() || pr->author.isEmpty()) {
        QMessageBox::warning(
            this, "Send to source of truth",
            "This pull request is missing its signature, so it can't be delivered "
            "to the source of truth.");
        return;
    }
    // Copy before the modal question below: its nested event loop can service a
    // reloadPulls()/repo reload that reassigns m_currentPulls / m_repositories,
    // dangling `pr` and a repo reference (git-pump UAF family).
    const PullRequest pull = *pr;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    if (QMessageBox::question(
            this, "Send to source of truth",
            QStringLiteral(
                "Deliver pull request #%1 to %2/%3's inbox?\n\n"
                "The relay queues it, so it reaches the source of truth even if "
                "that node is currently offline.")
                .arg(m_currentPullNumber)
                .arg(repo.owner, repo.name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) != QMessageBox::Yes)
        return;
    submitPullToInbox(pull, repo);
}

void MainWindow::setPullDeleteButtonsEnabled(bool enabled)
{
    if (m_pullDeleteButton)
        m_pullDeleteButton->setEnabled(enabled);
    if (m_pullDeleteBranchButton)
        m_pullDeleteBranchButton->setEnabled(enabled);
    if (m_pullMergeDeleteButton)
        m_pullMergeDeleteButton->setEnabled(enabled);
    if (m_pullDeleteClosedAndMergedButton)
        m_pullDeleteClosedAndMergedButton->setEnabled(enabled);
}

bool MainWindow::confirmPullDeletion(const QString &prompt, bool *rewriteHistory)
{
    QMessageBox box(QMessageBox::Warning, QStringLiteral("Delete pull request"),
                    prompt, QMessageBox::Ok | QMessageBox::Cancel, this);
    auto *purge = new QCheckBox(
        QStringLiteral("Also scrub the PR's diff from git history (slow)"));
    purge->setChecked(false);
    box.setCheckBox(purge); // QMessageBox takes ownership
    const bool confirmed = box.exec() == QMessageBox::Ok;
    if (rewriteHistory)
        *rewriteHistory = confirmed && purge->isChecked();
    return confirmed;
}

void MainWindow::deleteCurrentPull()
{
    if (m_currentPullNumber < 0)
        return;
    if (!pullStoreForCurrentRepo().canWrite()) {
        requestCurrentPullLifecycle(QStringLiteral("delete"));
        return;
    }
    if (m_pullDeleteInProgress) {
        setRepoDetailNotice(
            QStringLiteral("A pull request deletion is already running."));
        return;
    }
    bool rewriteHistory = false;
    if (!m_pullDeleteConfirmPending) {
        m_pullDeleteConfirmPending = true;
        const QString prompt =
            QStringLiteral("Permanently delete pull request #%1? This cannot be undone.")
                .arg(m_currentPullNumber);
        const bool confirmed = confirmPullDeletion(prompt, &rewriteHistory);
        m_pullDeleteConfirmPending = false;
        if (!confirmed)
            return;
    }

    // The plain delete just drops the PR folder at the tip and is fast. Only the
    // opt-in history rewrite (filter-branch + gc + reflog expire) is slow enough to
    // freeze the UI — either way, run it on a worker thread (deletePull only touches
    // git, no event signing, so a copied store is safe) and report back on the main
    // thread.
    const int deleted = m_currentPullNumber;
    m_pullDeleteInProgress = true;
    setPullDeleteButtonsEnabled(false);
    setRepoDetailNotice(
        rewriteHistory
            ? QStringLiteral("Deleting pull request #%1 and rewriting history… this "
                             "can take a while.")
                  .arg(deleted)
            : QStringLiteral("Deleting pull request #%1…").arg(deleted));
    QApplication::setOverrideCursor(Qt::BusyCursor);

    PullStore store = pullStoreForCurrentRepo();
    auto ok = std::make_shared<bool>(false);
    auto error = std::make_shared<QString>();
    QThread *worker =
        QThread::create([store, deleted, rewriteHistory, ok, error]() mutable {
            QString err;
            *ok = store.deletePull(deleted, rewriteHistory, &err);
            *error = err;
        });
    connect(worker, &QThread::finished, this,
            [this, worker, ok, error, deleted]() {
                m_pullDeleteInProgress = false;
                QApplication::restoreOverrideCursor();
                setPullDeleteButtonsEnabled(true);
                // Same rule as the delete-with-branch path: a record that is
                // already absent has reached the requested state, so retire the
                // stale selection and refresh rather than reporting a failure
                // for work that is done (adhoc #2088).
                const bool alreadyGone =
                    !*ok && PullStore::missingPullError(*error, deleted);
                if (!*ok && !alreadyGone) {
                    QMessageBox::warning(
                        this, "Delete pull request",
                        error->isEmpty()
                            ? QStringLiteral("Could not delete the pull request.")
                            : *error);
                    worker->deleteLater();
                    return;
                }
                if (m_currentPullNumber == deleted)
                    m_currentPullNumber = -1;
                reloadPulls();
                setRepoDetailNotice(
                    alreadyGone
                        ? QStringLiteral("Pull request #%1 was already gone.")
                              .arg(deleted)
                        : QStringLiteral("Deleted pull request #%1.").arg(deleted));
                worker->deleteLater();
            });
    worker->start();
}

// The record "Delete + branch" tears down. Both delete buttons stay enabled off
// m_currentPullNumber alone, so the selection can outlive its row in the loaded
// list: the ledger was rewritten on another node, a background reload dropped
// the record, the folder was removed by an agent cleanup. Everything the
// teardown needs follows from the number — the pull folder, and the canonical
// pr/<number> head once the signed head name is gone with the record — so a
// missing row falls back to a bare record and the click still deletes, instead
// of being a no-op that leaves the branch behind with no way to reach it from
// here (adhoc #2088).
PullRequest MainWindow::pullRecordForDeletion(int number) const
{
    for (const PullRequest &pr : m_currentPulls)
        if (pr.number == number)
            return pr;
    PullRequest fallback;
    fallback.number = number;
    return fallback;
}

void MainWindow::deleteCurrentPullAndBranch()
{
    if (m_currentPullNumber < 0)
        return;
    if (m_pullDeleteInProgress) {
        setRepoDetailNotice(
            QStringLiteral("A pull request deletion is already running."));
        return;
    }
    const PullRequest current = pullRecordForDeletion(m_currentPullNumber);
    const QString head = localPullHeadForDeletion(current);
    const bool haveBranch = !head.isEmpty();

    const QString prompt =
        haveBranch
            ? QStringLiteral("Permanently delete pull request #%1 and its branch "
                             "\"%2\"? This cannot be undone.")
                  .arg(m_currentPullNumber)
                  .arg(head)
            : QStringLiteral("Permanently delete pull request #%1? This cannot be "
                             "undone.")
                  .arg(m_currentPullNumber);
    bool rewriteHistory = false;
    if (!confirmPullDeletion(prompt, &rewriteHistory))
        return;

    deletePullAndBranchAsync(m_currentPullNumber, head, haveBranch, rewriteHistory,
                             /*propagate=*/false);
}

void MainWindow::deleteAllClosedAndMergedPullsAndBranches()
{
    if (m_pullDeleteInProgress) {
        setRepoDetailNotice(
            QStringLiteral("A pull request deletion is already running."));
        return;
    }
    QList<int> numbers;
    QStringList heads;
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        if (pr.archivedOnCodeBranch ||
            (pr.status != QLatin1String("closed") &&
             pr.status != QLatin1String("merged")))
            continue;
        numbers.append(pr.number);
        const QString head = localPullHeadForDeletion(pr);
        if (!head.isEmpty() && !heads.contains(head))
            heads.append(head);
    }
    if (numbers.isEmpty()) {
        setRepoDetailNotice(
            QStringLiteral("No closed or merged pull requests to delete."));
        return;
    }

    const QString prompt =
        QStringLiteral("Permanently delete %1 closed or merged pull request(s) "
                       "and any matching local head branches? This cannot be "
                       "undone.")
            .arg(numbers.size());
    bool rewriteHistory = false;
    if (!confirmPullDeletion(prompt, &rewriteHistory))
        return;

    // One batch: the store drops every record in a single ledger commit (and
    // a single history rewrite when asked), then the head branches go in one
    // git call and the lists reload once. The old per-pull chain committed,
    // tore down refs and reloaded the whole pull list for each of them.
    const QString dir = repoGitDir();
    const bool haveWorkTree = repoHasWorkingTree();
    const int total = numbers.size();
    m_pullDeleteInProgress = true;
    setRepoDetailNotice(
        rewriteHistory
            ? QStringLiteral("Deleting %1 pull request(s) and rewriting history… "
                             "this can take a while.")
                  .arg(total)
            : QStringLiteral("Deleting %1 pull request(s)…").arg(total));
    setPullDeleteButtonsEnabled(false);
    QApplication::setOverrideCursor(Qt::BusyCursor);

    PullStore store = pullStoreForCurrentRepo();
    auto ok = std::make_shared<bool>(false);
    auto error = std::make_shared<QString>();
    auto deleted = std::make_shared<QList<int>>();
    auto missing = std::make_shared<QList<int>>();
    auto existing = std::make_shared<QStringList>();
    auto failedBranches = std::make_shared<QStringList>();
    QThread *worker = QThread::create(
        [store, numbers, rewriteHistory, ok, error, deleted, missing, heads, dir,
         haveWorkTree, existing, failedBranches]() mutable {
            QString err;
            *ok = store.deletePulls(numbers, rewriteHistory, deleted.get(),
                                    missing.get(), &err);
            *error = err;
            if (!*ok || !haveWorkTree || dir.isEmpty())
                return;
            // PullStore::deletePulls removed the materialized refs; what is
            // left are contributor heads that still exist as local branches.
            for (const QString &head : heads)
                if (repoHasLocalBranch(dir, head))
                    *existing << head;
            if (existing->isEmpty())
                return;
            QStringList args{"branch", "-D"};
            args += *existing;
            QString branchErr;
            if (!runGitCapture(dir, args, nullptr, &branchErr)) {
                // git deletes what it can and reports the rest; name
                // exactly the branches that survived.
                for (const QString &head : std::as_const(*existing))
                    if (repoHasLocalBranch(dir, head))
                        *failedBranches << head;
            }
        });
    connect(worker, &QThread::finished, this,
            [this, worker, ok, error, deleted, missing, numbers, total, existing,
             failedBranches]() {
                m_pullDeleteInProgress = false;
                QApplication::restoreOverrideCursor();
                setPullDeleteButtonsEnabled(true);
                worker->deleteLater();
                if (!*ok) {
                    reloadPulls();
                    QMessageBox::warning(
                        this, "Delete pull requests",
                        error->isEmpty()
                            ? QStringLiteral("Could not delete the pull requests.")
                            : *error);
                    return;
                }
                if (numbers.contains(m_currentPullNumber))
                    m_currentPullNumber = -1;
                if (!missing->isEmpty()) {
                    QStringList labels;
                    for (int number : std::as_const(*missing))
                        labels << QStringLiteral("#%1").arg(number);
                    logSystem(QStringLiteral("Pull request(s) %1 were already absent "
                                             "from the ledger; cleaned up what they "
                                             "left behind.")
                                  .arg(labels.join(QStringLiteral(", "))));
                }

                if (!existing->isEmpty()) {
                    const int removed = existing->size() - failedBranches->size();
                    if (removed > 0)
                        logSystem(QStringLiteral("Git: deleted %1 branch(es) for %2 "
                                                 "deleted pull request(s).")
                                      .arg(removed)
                                      .arg(total));
                    loadBranchesAndTags();
                }
                reloadPulls();
                const QString summary =
                    QStringLiteral("Deleted %1 closed or merged pull request(s).")
                        .arg(deleted->size() + missing->size());
                if (failedBranches->isEmpty())
                    setRepoDetailNotice(summary);
                else
                    setRepoDetailNotice(
                        summary +
                            QStringLiteral(" %1 branch(es) could not be removed: %2")
                                .arg(failedBranches->size())
                                .arg(failedBranches->join(QStringLiteral(", "))),
                        true, forkmesh::kLocalOperationNoticeKind);
            });
    worker->start();
}

void MainWindow::mergeAndDeleteCurrentPull()
{
    if (m_currentPullNumber < 0)
        return;
    if (m_pullDeleteInProgress) {
        setRepoDetailNotice(
            QStringLiteral("A pull request deletion is already running."));
        return;
    }
    PullRequest current;
    bool found = false;
    for (const PullRequest &pr : std::as_const(m_currentPulls)) {
        if (pr.number == m_currentPullNumber) {
            current = pr;
            found = true;
            break;
        }
    }
    if (!found)
        return;
    const bool requirePeerApproval =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
            ? m_repositories.at(m_repoDetailIndex).requirePeerApproval
            : true;
    const bool reviewReady = current.independentReviewGateSatisfied();
    const bool repositoryOwner = currentAccountOwnsPullRepository();
    const bool reviewBlocks = PullMergePolicy::reviewGateBlocks(
        requirePeerApproval, reviewReady, repositoryOwner);
    const bool ownerReviewOverride = PullMergePolicy::ownerOverrideRequired(
        requirePeerApproval, reviewReady, repositoryOwner);
    if (reviewBlocks) {
        QMessageBox::warning(
            this, "Merge pull request",
            current.hasIndependentChangesRequested()
                ? QStringLiteral("Pull request #%1 has an unresolved peer "
                                 "\"request changes\" review. Resolve it before "
                                 "merging.")
                      .arg(m_currentPullNumber)
                : QStringLiteral("Pull request #%1 needs at least one approval "
                                 "from a peer other than its author before "
                                 "merging.")
                .arg(m_currentPullNumber));
        return;
    }

    const QString head = current.head;
    const QString cleanupHead = localPullHeadForDeletion(current);
    const bool haveBranch = !cleanupHead.isEmpty();

    QString prompt =
        haveBranch
            ? QStringLiteral("Merge pull request #%1, then permanently delete it and "
                             "its branch \"%2\"? This cannot be undone.")
                  .arg(m_currentPullNumber)
                  .arg(cleanupHead)
            : QStringLiteral("Merge pull request #%1, then permanently delete it? "
                             "This cannot be undone.")
                  .arg(m_currentPullNumber);
    if (ownerReviewOverride) {
        const QString reviewWarning =
            current.hasIndependentChangesRequested()
                ? QStringLiteral(
                      "A peer has requested changes. Merging as repository owner "
                      "will waive the required peer-review gate, and the waiver "
                      "will be recorded in the pull-request ledger.")
                : QStringLiteral(
                      "No independent peer has approved this pull request. "
                      "Merging as repository owner will waive the required "
                      "peer-review gate, and the waiver will be recorded in the "
                      "pull-request ledger.");
        prompt.prepend(reviewWarning + QStringLiteral("\n\n"));
    }
    bool rewriteHistory = false;
    if (!confirmPullDeletion(prompt, &rewriteHistory))
        return;

    PullStore store = pullStoreForCurrentRepo();
    QString error;
    if (!store.mergePull(m_currentPullNumber, &error, requirePeerApproval,
                         ownerReviewOverride)) {
        reloadPullsInBackground();
        QMessageBox warning(QMessageBox::Warning,
                            QStringLiteral("Merge pull request"), error,
                            QMessageBox::Ok, this);
        if (forkmesh::pulldecision::decidedPullError(error)
            || forkmesh::pulldecision::localChangesBlockError(error))
            warning.setProperty(forkmesh::kLocalOperationDialogProperty, true);
        warning.exec();
        return;
    }
    logSystem(ownerReviewOverride
                  ? QStringLiteral(
                        "Merged pull request #%1 (peer-review gate waived by "
                        "repository owner).")
                        .arg(m_currentPullNumber)
                  : QStringLiteral("Merged pull request #%1.")
                        .arg(m_currentPullNumber));
    closeIssuesLinkedFromPull(current);
    fundBountiesForMergedPull(current);
    autoBountyForMergedPull(current);
    refreshSourceControl(true);
    markAgentSessionsMerged(m_currentPullNumber, head, /*mergeVerified=*/true);

    const bool autoSyncOnMerge =
        QSettings().value(kAutoSyncOnMergeSetting, false).toBool();
    if (!autoSyncOnMerge)
        logSystem(QStringLiteral("Merge landed locally — click \"Sync\" to publish "
                                 "it to main (auto-sync-on-merge is off)."));
    const int merged = m_currentPullNumber;
    const auto deletePull = [this, merged, cleanupHead, haveBranch,
                             rewriteHistory, autoSyncOnMerge] {
        deletePullAndBranchAsync(merged, cleanupHead, haveBranch, rewriteHistory,
                                 /*propagate=*/autoSyncOnMerge);
    };
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size()) {
        deletePull();
        return;
    }
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    const QString pullBase = current.base.trimmed();
    const QString defaultBranch = repoDefaultBranch(repoBranches());
    const auto agentBranchUsable = [&](const QString &branch) {
        return !branch.isEmpty() && branch != pullBase && branch != defaultBranch;
    };
    QString agentBranch = cleanupHead;
    bool agentAttached = false;
    for (const AgentSession &s : std::as_const(m_agentSessions)) {
        if (s.owner != repo.owner || s.name != repo.name ||
            isExternalSession(s.id) || !agentBranchUsable(s.branchName))
            continue;
        if (s.branchName == agentBranch ||
            (s.prNumber == merged && agentBranch.isEmpty())) {
            agentAttached = true;
            agentBranch = s.branchName;
        }
    }
    if (!agentBranchUsable(agentBranch)) {
        deletePull();
        return;
    }
    const QString worktree =
        worktreePathForBranch(repo.localPath, agentBranch, WorktreeLookup::Any);
    if (!agentAttached && worktree.isEmpty()) {
        deletePull();
        return;
    }
    deleteWorktreeBranchAndAgentInBackground(
        worktree, agentBranch,
        [deletePull](bool) { deletePull(); });
}

// Shared worker: delete the PR record (optionally scrubbing its diff from
// history) on a background thread, then best-effort remove its local head branch
// and reload the lists on the main thread. The plain delete just drops the PR
// folder (and the branch ref) and is fast; only the opt-in history rewrite
// (filter-branch + gc + reflog expire) is slow enough to need a worker — either
// way deletePull only touches git (no event signing) so a copied store is safe.
void MainWindow::deletePullAndBranchAsync(int number, const QString &head,
                                          bool haveBranch, bool rewriteHistory,
                                          bool propagate,
                                          std::function<void()> onDone)
{
    const int deleted = number;
    const QString dir = repoGitDir();
    const bool haveWorkTree = repoHasWorkingTree();
    m_pullDeleteInProgress = true;
    setRepoDetailNotice(
        rewriteHistory
            ? QStringLiteral("Deleting pull request #%1 and rewriting history… this "
                             "can take a while.")
                  .arg(deleted)
            : QStringLiteral("Deleting pull request #%1…").arg(deleted));
    setPullDeleteButtonsEnabled(false);
    QApplication::setOverrideCursor(Qt::BusyCursor);

    PullStore store = pullStoreForCurrentRepo();
    auto ok = std::make_shared<bool>(false);
    auto error = std::make_shared<QString>();
    auto branchTried = std::make_shared<bool>(false);
    auto branchDeleted = std::make_shared<bool>(false);
    auto branchError = std::make_shared<QString>();
    QThread *worker = QThread::create(
        [store, deleted, rewriteHistory, ok, error, head, haveBranch, dir,
         haveWorkTree, branchTried, branchDeleted, branchError]() mutable {
            QString err;
            *ok = store.deletePull(deleted, rewriteHistory, &err);
            *error = err;
            if ((!*ok && !PullStore::missingPullError(err, deleted)) ||
                !haveBranch || !haveWorkTree || dir.isEmpty() ||
                !repoHasLocalBranch(dir, head))
                return;
            *branchTried = true;
            *branchDeleted =
                runGitCapture(dir, {"branch", "-D", head}, nullptr, branchError.get());
        });
    connect(worker, &QThread::finished, this,
            [this, worker, ok, error, deleted, head, propagate, onDone,
             branchTried, branchDeleted, branchError]() {
                m_pullDeleteInProgress = false;
                QApplication::restoreOverrideCursor();
                setPullDeleteButtonsEnabled(true);
                // A ledger record that is already gone is the outcome this was
                // asked for, however it got there — an earlier pass, a delete on
                // another node, an agent cleanup. Stopping on it strands the head
                // branch this call exists to remove, so carry on into the branch
                // step and say what actually happened (adhoc #2088).
                const bool alreadyGone =
                    !*ok && PullStore::missingPullError(*error, deleted);
                if (!*ok && !alreadyGone) {
                    QMessageBox::warning(
                        this, "Delete pull request",
                        error->isEmpty()
                            ? QStringLiteral("Could not delete the pull request.")
                            : *error);
                    worker->deleteLater();
                    if (onDone)
                        onDone();
                    return;
                }
                if (alreadyGone)
                    logSystem(QStringLiteral("Pull request #%1 was already absent "
                                             "from the ledger; cleaning up what it "
                                             "left behind.")
                                  .arg(deleted));
                if (m_currentPullNumber == deleted)
                    m_currentPullNumber = -1;

                QString branchErrorNotice;
                // PullStore::deletePull removes the materialized refs itself.
                // Re-check after that write: trying to delete the same branch
                // again turns a successful, idempotent cleanup into a false
                // "branch not found" error (and files it in the shared error
                // log). A contributor head that still exists is cleaned up
                // here as before.
                if (*branchTried) {
                    if (*branchDeleted) {
                        logSystem(QStringLiteral("Git: deleted branch %1 for PR #%2.")
                                      .arg(head)
                                      .arg(deleted));
                        loadBranchesAndTags();
                    } else if (!branchError->trimmed().isEmpty()) {
                        branchErrorNotice =
                            QStringLiteral("Deleted PR #%1, but its branch could not "
                                           "be removed: %2")
                                .arg(deleted)
                                .arg(branchError->trimmed());
                    }
                }
                reloadPulls();
                if (branchErrorNotice.isEmpty())
                    setRepoDetailNotice(
                        alreadyGone
                            ? QStringLiteral("Pull request #%1 was already gone; "
                                             "finished cleaning up after it.")
                                  .arg(deleted)
                            : QStringLiteral("Deleted pull request #%1.")
                                  .arg(deleted));
                else
                    setRepoDetailNotice(
                        branchErrorNotice, true,
                        forkmesh::kLocalOperationNoticeKind);
                if (propagate)
                    propagateRepoUpdate(m_repoDetailIndex);
                worker->deleteLater();
                if (onDone)
                    onDone();
            });
    worker->start();
}

QUrl MainWindow::pullsApiUrl(const RepositoryRecord &repo) const
{
    QUrl url = catalogApiUrl();
    url.setPath("/api/repo/" + repoSegment(repo.owner, QStringLiteral("owner")) + "/" +
                repoSegment(repo.name, QStringLiteral("repository")) + "/pulls");
    return url;
}

void MainWindow::submitPullToInbox(const PullRequest &pr)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    submitPullToInbox(pr, repo);
}

void MainWindow::submitPullToInbox(const PullRequest &pr,
                                   const RepositoryRecord &targetRepo, bool quiet)
{
    QJsonObject entry{{"owner", targetRepo.owner},
                      {"repo", targetRepo.name},
                      {"pull", pr.toJson()},
                      {"attempts", 0},
                      {"retryAt", double(QDateTime::currentMSecsSinceEpoch())},
                      {"quiet", quiet}};
    const QString key = pullSubmissionKey(entry);
    for (const QJsonObject &pending : std::as_const(m_pendingPullSubmissions)) {
        if (pullSubmissionKey(pending) == key)
            return;
    }
    m_pendingPullSubmissions.append(entry);
    savePendingPullSubmissions();
    schedulePendingPullSubmissionRetry(0);
}

void MainWindow::restorePendingPullSubmissions()
{
    if (!m_pendingPullSubmissions.isEmpty())
        return;
    const QStringList encoded =
        QSettings().value(QLatin1String(kPendingPullSubmissionsSetting)).toStringList();
    for (const QString &raw : encoded) {
        const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
        const QJsonObject entry = document.object();
        if (!entry.value("owner").toString().trimmed().isEmpty() &&
            !entry.value("repo").toString().trimmed().isEmpty() &&
            !entry.value("pull").toObject().value("sig").toString().isEmpty())
            m_pendingPullSubmissions.append(entry);
    }
    if (!m_pendingPullSubmissions.isEmpty()) {
        logSystem(QStringLiteral("Restored %1 pending pull request submission%2.")
                      .arg(m_pendingPullSubmissions.size())
                      .arg(m_pendingPullSubmissions.size() == 1 ? QString()
                                                                 : QStringLiteral("s")));
        schedulePendingPullSubmissionRetry(0);
    }
}

void MainWindow::savePendingPullSubmissions() const
{
    QStringList encoded;
    encoded.reserve(m_pendingPullSubmissions.size());
    for (const QJsonObject &entry : m_pendingPullSubmissions)
        encoded << QString::fromUtf8(
            QJsonDocument(entry).toJson(QJsonDocument::Compact));
    QSettings settings;
    if (encoded.isEmpty())
        settings.remove(QLatin1String(kPendingPullSubmissionsSetting));
    else
        settings.setValue(QLatin1String(kPendingPullSubmissionsSetting), encoded);
    settings.sync();
}

void MainWindow::schedulePendingPullSubmissionRetry(int delayMs)
{
    if (!m_networkAccess || m_pendingPullSubmissions.isEmpty())
        return;
    if (!m_pullSubmissionRetryTimer) {
        m_pullSubmissionRetryTimer = new QTimer(this);
        m_pullSubmissionRetryTimer->setSingleShot(true);
        connect(m_pullSubmissionRetryTimer, &QTimer::timeout, this,
                &MainWindow::dispatchPendingPullSubmissions);
    }
    if (delayMs < 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 earliest = std::numeric_limits<qint64>::max();
        for (const QJsonObject &entry : std::as_const(m_pendingPullSubmissions))
            earliest = qMin(earliest,
                            qint64(entry.value("retryAt").toDouble(now)));
        delayMs = int(qBound<qint64>(qint64(0), earliest - now,
                                      qint64(std::numeric_limits<int>::max())));
    }
    m_pullSubmissionRetryTimer->start(qMax(0, delayMs));
}

void MainWindow::dispatchPendingPullSubmissions()
{
    if (m_pullSubmissionInFlight || !m_networkAccess)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QJsonObject &entry : std::as_const(m_pendingPullSubmissions)) {
        if (qint64(entry.value("retryAt").toDouble()) <= now) {
            deliverPendingPullSubmission(pullSubmissionKey(entry));
            return;
        }
    }
    schedulePendingPullSubmissionRetry();
}

void MainWindow::deliverPendingPullSubmission(const QString &submissionKey)
{
    int index = -1;
    QJsonObject entry;
    for (int i = 0; i < m_pendingPullSubmissions.size(); ++i) {
        if (pullSubmissionKey(m_pendingPullSubmissions.at(i)) == submissionKey) {
            index = i;
            entry = m_pendingPullSubmissions.at(i);
            break;
        }
    }
    if (index < 0 || !m_networkAccess)
        return;

    RepositoryRecord targetRepo;
    targetRepo.owner = entry.value("owner").toString();
    targetRepo.name = entry.value("repo").toString();
    const PullRequest pull = PullRequest::fromJson(entry.value("pull").toObject());
    const QJsonObject payload{{"owner", targetRepo.owner},
                              {"repo", targetRepo.name},
                              {"pull", pull.toJson()}};
    QNetworkRequest request(pullsApiUrl(targetRepo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setTransferTimeout(15000);
    m_pullSubmissionInFlight = true;
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, submissionKey, targetRepo] {
        const QNetworkReply::NetworkError netError = reply->error();
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString errorString = reply->errorString();
        reply->deleteLater();
        m_pullSubmissionInFlight = false;

        int index = -1;
        for (int i = 0; i < m_pendingPullSubmissions.size(); ++i) {
            if (pullSubmissionKey(m_pendingPullSubmissions.at(i)) == submissionKey) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            schedulePendingPullSubmissionRetry();
            return;
        }
        const QString slug = targetRepo.owner + "/" + targetRepo.name;
        QJsonObject pending = m_pendingPullSubmissions.at(index);
        const bool quiet = pending.value("quiet").toBool();
        if (netError == QNetworkReply::NoError) {
            m_pendingPullSubmissions.removeAt(index);
            savePendingPullSubmissions();
            schedulePendingPullSubmissionRetry();
            const QJsonObject response = QJsonDocument::fromJson(body).object();
            const int number = response.value("number").toInt();
            const QString label = number > 0
                                      ? QStringLiteral("Pull request #%1").arg(number)
                                      : QStringLiteral("Pull request");
            logSystem(QStringLiteral("%1 delivered to %2%3.")
                          .arg(label, slug,
                               response.value("duplicate").toBool()
                                   ? QStringLiteral(" (confirmed replay)")
                                   : QString()));
            if (!quiet)
                flashMessage(QStringLiteral("%1 was delivered to %2.").arg(label, slug));
            return;
        }
        const QString detail = pullSubmissionErrorDetail(status, body, errorString);
        if (isTransientPullSubmissionFailure(status)) {
            const int attempts = pending.value("attempts").toInt() + 1;
            const qint64 delay = qMin(
                kPullSubmissionRetryCapMs,
                kPullSubmissionRetryBaseMs * (1LL << qMin(attempts - 1, 6)));
            pending.insert("attempts", attempts);
            pending.insert("retryAt",
                           double(QDateTime::currentMSecsSinceEpoch() + delay));
            m_pendingPullSubmissions[index] = pending;
            savePendingPullSubmissions();
            schedulePendingPullSubmissionRetry();
            logSystem(QStringLiteral("Could not deliver a pull request to %1; "
                                     "retrying in %2 seconds: %3")
                          .arg(slug)
                          .arg(delay / 1000)
                          .arg(detail));
            return;
        }
        m_pendingPullSubmissions.removeAt(index);
        savePendingPullSubmissions();
        schedulePendingPullSubmissionRetry();
        if (quiet)
            flashMessage("Could not send the pull request to " + slug + ": " + detail,
                         true);
        else
            QMessageBox::warning(this, "Pull request",
                                 "Could not send the pull request to " + slug + ": " +
                                     detail);
    });
}

void MainWindow::submitPullEventToInbox(int number, const PullEvent &ev)
{
    submitPullEventToInbox(number, ev, m_repoDetailIndex);
}

void MainWindow::submitPullEventToInbox(int number, const PullEvent &ev, int repoIndex)
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    const QJsonObject payload{{"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", number},
                              {"event", ev.toJson()}};
    QNetworkRequest request(pullsApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, repo] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError)
            flashMessage("Your review was delivered to " + repo.owner + "/" +
                         repo.name + ".");
        else
            QMessageBox::warning(this, "Review",
                                 "Could not send your review: " + reply->errorString());
    });
}

void MainWindow::syncPullsInbox()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    showPendingInbox(m_repositories.at(m_repoDetailIndex),
                     QStringLiteral("pulls"));
}

void MainWindow::refreshPullsFromMesh()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;

    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    // Show the locally available ledger immediately, then converge both ways:
    // drain every relay submission without opening the Inbox review dialog and
    // fetch the served pull ledger from configured mirrors. The latter catches
    // PRs that a mirror already drained while this desktop was offline.
    reloadPullsInBackground();
    drainPullsInboxFor(repo, /*interactive=*/false, /*force=*/true);
    reconcilePullLedgerFor(repo, /*force=*/true);
}

QString MainWindow::relayCooldownMessage(const QString &host) const
{
    const auto *network =
        qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
    const qint64 remainingMs =
        network && !host.isEmpty() ? network->hostCooldownRemainingMs(host) : 0;
    if (remainingMs <= 0)
        return QStringLiteral("ForkMesh relay is rate-limited — try again in a "
                              "moment.");
    return QStringLiteral("ForkMesh relay is rate-limited — try again in %1.")
        .arg(formatDuration(remainingMs));
}

void MainWindow::showPendingInbox(const RepositoryRecord &repo,
                                  const QString &kind)
{
    if (!m_networkAccess || !hasOwnerSigningCapability()) {
        flashMessage(QStringLiteral("Network access is unavailable."), true);
        return;
    }
    // Nobody can file into the inbox of a repository that was never published,
    // so say that instead of asking the relay about a repository it is not
    // supposed to know exists.
    if (!PrivateMirrorRuntime::repositoryRelayInboxAllowed(
            repo.publishToNetwork)) {
        flashMessage(QStringLiteral("This repository is local-only, so it has "
                                    "no inbox to check."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }

    QUrl url;
    if (kind == QLatin1String("issues"))
        url = issuesApiUrl(repo);
    else if (kind == QLatin1String("pulls"))
        url = pullsApiUrl(repo);
    else if (kind == QLatin1String("discussions"))
        url = discussionsApiUrl(repo);
    else
        return;
    url.setQuery(signedInboxQuery(
        repoSegment(repo.owner, QStringLiteral("owner"))));

    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, kind, host = url.host()] {
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QString networkErrorText = reply->errorString();
        const bool rateLimited =
            BackoffNetworkAccessManager::isBackoffSuppressed(reply);
        reply->deleteLater();
        if (networkError != QNetworkReply::NoError) {
            if (rateLimited) {
                flashMessage(relayCooldownMessage(host), true);
                return;
            }
            QMessageBox::warning(
                this, QStringLiteral("Sync inbox"),
                status > 0
                    ? QStringLiteral(
                          "Could not load the collaboration inbox (HTTP %1): %2")
                          .arg(status)
                          .arg(networkErrorText)
                    : QStringLiteral("Could not load the collaboration inbox: %1")
                          .arg(networkErrorText));
            return;
        }
        const QJsonArray pending =
            QJsonDocument::fromJson(body).object().value("pending").toArray();
        setPendingInboxCount(repo, kind, pending.size());
        showPendingInboxDialog(repo, kind, pending);
    });
}

void MainWindow::showPendingInboxDialog(const RepositoryRecord &repo,
                                        const QString &kind,
                                        const QJsonArray &pending)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pendingInboxDialog"));
    dialog.setWindowTitle(QStringLiteral("Collaboration inbox: %1")
                              .arg(kind == QLatin1String("pulls")
                                       ? QStringLiteral("pull requests")
                                       : kind));
    dialog.resize(kind == QLatin1String("pulls") ? 780 : 680,
                  qBound(280, 200 + pending.size() * 38, 620));

    const bool reviewable = kind == QLatin1String("pulls");
    const int syncColumn = reviewable ? 3 : 2;

    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        pending.isEmpty()
            ? QStringLiteral("This inbox is up to date.")
            : (reviewable
                   ? QString::fromUtf8(
                         "%1 submission%2 waiting on the nodes for %3/%4. "
                         "\"Review\" pulls one onto this computer to read, build "
                         "and run \xE2\x80\x94 nothing is merged and nothing is "
                         "written into the repository until you \"Sync\" it.")
                         .arg(pending.size())
                         .arg(pending.size() == 1 ? QString()
                                                  : QStringLiteral("s"))
                         .arg(repo.owner, repo.name)
                   : QStringLiteral(
                         "%1 submission%2 waiting to be written into %3/%4.")
                         .arg(pending.size())
                         .arg(pending.size() == 1 ? QString()
                                                  : QStringLiteral("s"))
                         .arg(repo.owner, repo.name)),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *tree = new QTreeWidget(&dialog);
    tree->setObjectName(QStringLiteral("pendingInboxList"));
    tree->setColumnCount(syncColumn + 1);
    QStringList headers{QStringLiteral("Submission"), QStringLiteral("Author")};
    while (headers.size() < syncColumn + 1)
        headers << QString();
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setSelectionMode(QAbstractItemView::NoSelection);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column <= syncColumn; ++column)
        tree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    layout->addWidget(tree, 1);

    for (const QJsonValue &value : pending) {
        const QJsonObject item = value.toObject();
        QStringList cells{pendingInboxSummary(kind, item),
                          pendingInboxAuthor(kind, item), QString()};
        while (cells.size() < syncColumn + 1)
            cells << QString();
        auto *row = new QTreeWidgetItem(tree, cells);
        if (reviewable && item.contains(QStringLiteral("pull"))) {
            auto *reviewOne = new QPushButton(QStringLiteral("Review"), tree);
            reviewOne->setObjectName(QStringLiteral("pendingInboxReviewOne"));
            reviewOne->setProperty("buttonSize", "sm");
            reviewOne->setCursor(Qt::PointingHandCursor);
            reviewOne->setToolTip(
                QStringLiteral("Fetch this pull request onto this computer and "
                               "read it, without merging it or adding it to the "
                               "repository"));
            tree->setItemWidget(row, 2, reviewOne);
            connect(reviewOne, &QPushButton::clicked, &dialog,
                    [this, repo, item] { reviewPendingPull(repo, item); });
        }
        auto *syncOne = new QPushButton(QStringLiteral("Sync"), tree);
        syncOne->setObjectName(QStringLiteral("pendingInboxSyncOne"));
        syncOne->setProperty("buttonSize", "sm");
        syncOne->setCursor(Qt::PointingHandCursor);
        syncOne->setToolTip(
            QStringLiteral("Write this submission into %1/%2")
                .arg(repo.owner, repo.name));
        tree->setItemWidget(row, syncColumn, syncOne);
        connect(syncOne, &QPushButton::clicked, &dialog,
                [this, tree, row, repo, kind, item] {
            QJsonArray selected;
            selected.append(item);
            applyPendingInboxSelection(repo, kind, selected);
            const int index = tree->indexOfTopLevelItem(row);
            if (index >= 0)
                delete tree->takeTopLevelItem(index);
            setPendingInboxCount(repo, kind, tree->topLevelItemCount());
        });
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    QPushButton *syncAll = buttons->addButton(
        QStringLiteral("Sync all"), QDialogButtonBox::ActionRole);
    syncAll->setObjectName(QStringLiteral("pendingInboxSyncAll"));
    syncAll->setEnabled(!pending.isEmpty());
    connect(syncAll, &QPushButton::clicked, &dialog,
            [this, &dialog, repo, kind, pending] {
        applyPendingInboxSelection(repo, kind, pending);
        setPendingInboxCount(repo, kind, 0);
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

QString MainWindow::pullReviewCheckoutPath(const RepositoryRecord &repo,
                                           const QString &slug) const
{
    const auto safe = [](const QString &text) {
        QString out;
        for (const QChar ch : text) {
            out += (ch.isLetterOrNumber() || ch == QLatin1Char('-') ||
                    ch == QLatin1Char('_') || ch == QLatin1Char('.'))
                       ? ch
                       : QLatin1Char('-');
        }
        return out.left(60);
    };
    return forkmesh::paths::localDataRoot() +
           QStringLiteral("/pull-review/%1__%2/pr-%3")
               .arg(safe(repo.owner), safe(repo.name), safe(slug));
}

void MainWindow::reviewPendingPull(RepositoryRecord repo, QJsonObject item)
{
    const QJsonObject payload = item.value(QStringLiteral("pull")).toObject();
    if (payload.isEmpty()) {
        flashMessage(QStringLiteral("Only a whole pull request can be pulled down "
                                    "for review; this is a comment on one."),
                     true);
        return;
    }
    const PullRequest pr = PullRequest::fromJson(payload);
    const RepositoryRecord writable = writableRecordFor(repo);
    const PullStore store(writable.localPath, writable.mirrorPath,
                          &m_profileIdentity, m_userName);
    QString slug = pr.number > 0 ? QString::number(pr.number) : QString();
    if (slug.isEmpty()) {
        const QJsonValue id = item.value(QStringLiteral("id"));
        slug = QStringLiteral("inbox-%1")
                   .arg(id.isDouble() ? QString::number(qint64(id.toDouble()))
                                      : pr.sig.left(12));
    }
    const QString dir = pullReviewCheckoutPath(repo, slug);

    PullReviewCheckout checkout;
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = store.checkoutForReview(pr, dir, &checkout, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::warning(this, QStringLiteral("Review pull request"),
                             error.isEmpty()
                                 ? QStringLiteral("Could not pull this submission "
                                                  "down for review.")
                                 : error);
        return;
    }
    logSystem(QStringLiteral("Pulled queued pull request \"%1\" from %2/%3 into "
                             "%4 for review (not merged).")
                  .arg(pr.title.left(80), repo.owner, repo.name, checkout.path));
    showPullReviewDialog(repo, pr, checkout);
}

void MainWindow::showPullReviewDialog(RepositoryRecord repo, PullRequest pr,
                                      PullReviewCheckout checkout)
{
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pullReviewDialog"));
    dialog.setWindowTitle(pr.number > 0
                              ? QStringLiteral("Review pull request #%1")
                                    .arg(pr.number)
                              : QStringLiteral("Review pull request"));
    dialog.resize(1000, 720);

    auto *layout = new QVBoxLayout(&dialog);
    auto *title = new QLabel(
        QStringLiteral("<b>%1</b>")
            .arg(pr.title.isEmpty() ? QStringLiteral("(untitled)")
                                    : pr.title.toHtmlEscaped()),
        &dialog);
    title->setTextFormat(Qt::RichText);
    title->setWordWrap(true);
    layout->addWidget(title);

    QStringList metaBits;
    const QString author =
        pr.authorName.isEmpty() ? pr.author.left(12) : pr.authorName;
    if (!author.isEmpty())
        metaBits << author;
    if (!pr.head.isEmpty() || !pr.base.isEmpty())
        metaBits << QString::fromUtf8("%1 \xE2\x86\x92 %2")
                        .arg(pr.head.isEmpty() ? QStringLiteral("(patch)")
                                               : pr.head,
                             pr.base.isEmpty() ? QStringLiteral("(base)")
                                               : pr.base);
    if (checkout.commitCount > 0)
        metaBits << QStringLiteral("%1 commit%2")
                        .arg(checkout.commitCount)
                        .arg(checkout.commitCount == 1 ? QString()
                                                       : QStringLiteral("s"));
    if (!checkout.baseOid.isEmpty())
        metaBits << QStringLiteral("on %1").arg(checkout.baseOid.left(8));
    if (!metaBits.isEmpty()) {
        auto *meta = new QLabel(metaBits.join(QString::fromUtf8(" \xC2\xB7 ")),
                                &dialog);
        meta->setObjectName(QStringLiteral("statusLine"));
        meta->setWordWrap(true);
        layout->addWidget(meta);
    }

    QString notice =
        QString::fromUtf8("Pulled onto this computer for review only \xE2\x80\x94 "
                       "nothing has been merged and no pull request has been "
                       "written into %1/%2. It stays in the inbox until you sync "
                       "it.\n\n%3")
            .arg(repo.owner, repo.name, checkout.path);
    if (checkout.uncommitted)
        notice += QStringLiteral(
            "\n\nThis submission would not replay as commits, so its patch was "
            "applied as uncommitted changes in that folder.");
    if (!checkout.conflicts.isEmpty())
        notice += QStringLiteral("\n\nConflicting file%1: %2")
                      .arg(checkout.conflicts.size() == 1 ? QString()
                                                        : QStringLiteral("s"),
                           checkout.conflicts.mid(0, 8).join(
                               QStringLiteral(", ")));
    auto *note = new QLabel(notice, &dialog);
    note->setObjectName(QStringLiteral("pullReviewNotice"));
    note->setWordWrap(true);
    note->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(note);

    if (!pr.description.trimmed().isEmpty()) {
        auto *body = new QLabel(
            QStringLiteral("<span style='white-space:pre-wrap'>%1</span>")
                .arg(pr.description.trimmed().left(4000).toHtmlEscaped()),
            &dialog);
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(body);
    }

    auto *diff = new QTextBrowser(&dialog);
    diff->setObjectName(QStringLiteral("pullReviewDiff"));
    diff->setOpenLinks(false);
    registerDiffView(diff);
    QList<DiffFileEntry> files;
    const QString html =
        renderDiffHtml(checkout.patch, files, checkout.path, checkout.baseOid,
                       checkout.headOid);
    if (html.trimmed().isEmpty())
        setDiffHtml(diff, QStringLiteral("<p style='color:#8b949e'>This "
                                         "submission carries no file "
                                         "changes.</p>"));
    else
        setDiffHtml(diff, html);
    layout->addWidget(diff, 1);

    auto *buttons = new QDialogButtonBox(&dialog);
    QPushButton *openFolder = buttons->addButton(
        QStringLiteral("Open folder"), QDialogButtonBox::ActionRole);
    openFolder->setObjectName(QStringLiteral("pullReviewOpenFolder"));
    QPushButton *discard = buttons->addButton(
        QStringLiteral("Discard review copy"), QDialogButtonBox::ActionRole);
    discard->setObjectName(QStringLiteral("pullReviewDiscard"));
    buttons->addButton(QDialogButtonBox::Close);
    const QString path = checkout.path;
    const RepositoryRecord writable = writableRecordFor(repo);
    const QString localPath = writable.localPath;
    const QString mirrorPath = writable.mirrorPath;
    connect(openFolder, &QPushButton::clicked, &dialog, [path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    });
    connect(discard, &QPushButton::clicked, &dialog,
            [this, &dialog, path, localPath, mirrorPath] {
        const PullStore store(localPath, mirrorPath, &m_profileIdentity,
                              m_userName);
        store.discardReviewCheckout(path);
        flashMessage(QStringLiteral("Removed the review copy at %1.").arg(path));
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::applyPendingInboxSelection(const RepositoryRecord &repo,
                                            const QString &kind,
                                            const QJsonArray &pending)
{
    if (pending.isEmpty())
        return;
    if (kind == QLatin1String("issues"))
        applyIssuesInboxPayload(repo, pending, /*interactive=*/true);
    else if (kind == QLatin1String("pulls"))
        applyPullsInboxPayload(repo, pending, /*interactive=*/true);
    else if (kind == QLatin1String("discussions"))
        applyDiscussionsInboxPayload(repo, pending, /*interactive=*/true);

    QTimer::singleShot(1500, this, [this, repo] {
        const QString source =
            repoSegment(repo.owner, QStringLiteral("owner")) + QLatin1Char('/') +
            repoSegment(repo.name, QStringLiteral("repository"));
        QJsonObject cached = m_mirrorPendingCache.value(source);
        cached.insert(QStringLiteral("clientFetchedAt"), 0);
        m_mirrorPendingCache.insert(source, cached);
        refreshPendingInboxBadges();
    });
}

void MainWindow::setPendingInboxCount(const RepositoryRecord &repo,
                                      const QString &kind, int count)
{
    const QString source =
        repoSegment(repo.owner, QStringLiteral("owner")) + QLatin1Char('/') +
        repoSegment(repo.name, QStringLiteral("repository"));
    QJsonObject result = m_mirrorPendingCache.value(source);
    QJsonObject counts = result.value(QStringLiteral("pending")).toObject();
    counts.insert(kind, qMax(0, count));
    result.insert(QStringLiteral("ok"), true);
    result.insert(QStringLiteral("pending"), counts);
    result.insert(QStringLiteral("clientFetchedAt"),
                  double(QDateTime::currentMSecsSinceEpoch()));
    m_mirrorPendingCache.insert(source, result);
    refreshPendingInboxBadges();
}

void MainWindow::setPendingPullsTabBadge(int pending)
{
    auto *tab = dynamic_cast<VerticalIconButton *>(m_repoPullsTab);
    if (!tab)
        return;
    tab->setAlertBadgeCount(qMax(0, pending));
    tab->setToolTip(
        pending > 0
            ? QString::fromUtf8("%1 pull request submission%2 waiting on the nodes "
                             "\xE2\x80\x94 open Pulls and click Inbox to review "
                             "them")
                  .arg(pending)
                  .arg(pending == 1 ? QString() : QStringLiteral("s"))
            : QString());
}

void MainWindow::refreshPendingInboxBadges()
{
    // Collaboration submissions are materialized by an eligible managed
    // mirror before the API returns success. The retired relay-pending count
    // must not create a second status system or a periodic Worker request.
    setPullActionBadge(m_issueSyncButton, 0);
    setPullActionBadge(m_pullSyncButton, 0);
    setPendingPullsTabBadge(0);
    if (m_discussionSyncButton)
        m_discussionSyncButton->setText(QStringLiteral("Sync inbox"));
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testSetPendingInboxCounts(int issues, int pulls,
                                           int discussions)
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    setPendingInboxCount(repo, QStringLiteral("issues"), issues);
    setPendingInboxCount(repo, QStringLiteral("pulls"), pulls);
    setPendingInboxCount(repo, QStringLiteral("discussions"), discussions);
}

int MainWindow::testIssueInboxBadgeCount() const
{
    const auto *button = dynamic_cast<const VerticalIconButton *>(m_issueSyncButton);
    return button ? int(button->badgeCount()) : 0;
}

int MainWindow::testPullInboxBadgeCount() const
{
    const auto *button = dynamic_cast<const VerticalIconButton *>(m_pullSyncButton);
    return button ? int(button->badgeCount()) : 0;
}

int MainWindow::testPullsTabAlertBadgeCount() const
{
    const auto *tab = dynamic_cast<const VerticalIconButton *>(m_repoPullsTab);
    return tab ? int(tab->alertBadgeCount()) : 0;
}

QString MainWindow::testDiscussionInboxButtonText() const
{
    return m_discussionSyncButton ? m_discussionSyncButton->text() : QString();
}

QList<int> MainWindow::testPullListRowNumbers(const QList<PullRequest> &pulls,
                                              int viewing)
{
    const QList<PullRequest> savedPulls = m_currentPulls;
    const int savedNumber = m_currentPullNumber;
    m_currentPulls = pulls;
    m_currentPullNumber = viewing;
    refreshPullList();
    QList<int> numbers;
    for (int row = 0; m_pullTable && row < m_pullTable->rowCount(); ++row)
        if (QTableWidgetItem *item = m_pullTable->item(row, 0))
            numbers.append(item->data(Qt::UserRole).toInt());
    m_currentPulls = savedPulls;
    m_currentPullNumber = savedNumber;
    refreshPullList(); // hand the page back the repository's own pulls
    return numbers;
}

QString MainWindow::testPullDisplayStatus(const PullRequest &pull) const
{
    return pullListStatusLabel(pullDisplayStatus(pull));
}

int MainWindow::testPullRowsAfterRepeatSnapshot(const QList<PullRequest> &pulls)
{
    if (!m_pullTable || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return -1;
    const QList<PullRequest> savedPulls = m_currentPulls;
    const QByteArray savedSnapshot = m_pullSnapshotDigest;
    const QByteArray savedRendered = m_pullListRenderedDigest;
    const int savedNumber = m_currentPullNumber;
    const int savedRegistered = m_pullRegisteredCount;
    const PullStore store = pullStoreForCurrentRepo();
    const QByteArray digest = QByteArrayLiteral("test-pull-landing-snapshot");
    m_currentPullNumber = -1;
    m_pullSnapshotDigest.clear();
    m_pullListRenderedDigest.clear();
    // The first load lands while the tab is still a deferred placeholder, so
    // it has no table to render into.
    QTableWidget *table = m_pullTable;
    m_pullTable = nullptr;
    applyLoadedPulls(store, pulls, QString(), digest);
    m_pullTable = table;
    m_pullTable->setRowCount(0);
    // Landing on the tab reloads, and the ledger has not moved meanwhile.
    applyLoadedPulls(store, pulls, QString(), digest);
    const int rows = m_pullTable->rowCount();
    m_currentPulls = savedPulls;
    m_pullSnapshotDigest = savedSnapshot;
    m_currentPullNumber = savedNumber;
    m_pullRegisteredCount = savedRegistered;
    refreshPullList(); // hand the page back the repository's own pulls
    m_pullListRenderedDigest = savedRendered;
    return rows;
}

bool MainWindow::testPullListShowsLoaderWhileLoading(
    const QList<PullRequest> &pulls)
{
    if (!m_pullTable || !m_pullListLoading)
        return false;
    const QList<PullRequest> savedPulls = m_currentPulls;
    const bool savedInFlight = m_pullBackgroundLoadInFlight;
    m_currentPulls.clear();
    m_pullBackgroundLoadInFlight = true;
    refreshPullList();
    const bool whileEmpty = !m_pullListLoading->isHidden();
    m_currentPulls = pulls;
    refreshPullList();
    const bool withRows = !m_pullListLoading->isHidden();
    m_pullBackgroundLoadInFlight = savedInFlight;
    m_currentPulls = savedPulls;
    refreshPullList();
    return whileEmpty && !withRows;
}

QString MainWindow::testPullListBranchMeta(int number) const
{
    for (int row = 0; m_pullTable && row < m_pullTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_pullTable->item(row, 0);
        if (!item || item->data(Qt::UserRole).toInt() != number)
            continue;
        return QStringLiteral("%1 %2 %3 %4 %5")
            .arg(item->data(kPullConflictRole).toBool() ? 1 : 0)
            .arg(item->data(kPullAheadRole).toInt())
            .arg(item->data(kPullBehindRole).toInt())
            .arg(item->data(kPullDirtyRole).toInt())
            .arg(item->data(kPullWorktreeRole).toString());
    }
    return QString();
}

QString MainWindow::testPullListRowTooltip(int number) const
{
    for (int row = 0; m_pullTable && row < m_pullTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_pullTable->item(row, 0);
        if (item && item->data(Qt::UserRole).toInt() == number)
            return item->toolTip();
    }
    return QString();
}

QString MainWindow::testQuickAddPlaceholderForPull(const PullRequest &pull)
{
    if (!m_pullDetail)
        return QString();

    const QList<PullRequest> savedPulls = m_currentPulls;
    const int savedNumber = m_currentPullNumber;
    const bool detailWasVisible = m_pullDetail->isVisible();

    m_currentPulls = {pull};
    showPull(pull.number);
    const QString placeholder =
        m_issueQuickAdd ? m_issueQuickAdd->placeholderText() : QString();

    m_currentPulls = savedPulls;
    const bool restoreSelected = std::any_of(
        savedPulls.cbegin(), savedPulls.cend(), [savedNumber](const PullRequest &saved) {
            return saved.number == savedNumber;
        });
    showPull(restoreSelected ? savedNumber : -1);
    if (!detailWasVisible)
        m_pullDetail->hide();
    updateQuickAddEnterTarget();
    return placeholder;
}

QStringList MainWindow::testPullThreadCardHeaders() const
{
    QStringList headers;
    if (!m_pullThreadLayout)
        return headers;
    for (int i = 0; i < m_pullThreadLayout->count(); ++i) {
        QWidget *row = m_pullThreadLayout->itemAt(i)->widget();
        if (!row || row->objectName() != QLatin1String("issueTimelineRow"))
            continue;
        if (QWidget *box =
                row->findChild<QWidget *>(QStringLiteral("issueTimelineHeader")))
            if (QLabel *header = box->findChild<QLabel *>())
                headers << header->text();
    }
    return headers;
}

int MainWindow::testPullThreadUnmanagedVisibleRows() const
{
    if (!m_pullThreadLayout || !m_pullThreadContainer)
        return 0;
    QSet<QWidget *> managed;
    for (int i = 0; i < m_pullThreadLayout->count(); ++i)
        if (QWidget *widget = m_pullThreadLayout->itemAt(i)->widget())
            managed.insert(widget);
    int count = 0;
    const QList<QWidget *> rows = m_pullThreadContainer->findChildren<QWidget *>(
        QStringLiteral("issueTimelineRow"), Qt::FindDirectChildrenOnly);
    for (QWidget *row : rows)
        if (!managed.contains(row) && !row->isHidden())
            ++count;
    return count;
}
#endif

void MainWindow::reconcilePullLedgerFor(RepositoryRecord repo, bool force)
{
    // Serialized and GUI-thread only: every git call below pumps the event
    // loop, so a second sweep entering mid-cherry-pick would corrupt the
    // ledger checkout. Everything is taken by value past this point.
    if (m_pullLedgerSweepInProgress)
        return;
    const RepositoryRecord writable = writableRecordFor(repo);
    PullStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                    m_userName);
    if (!store.canWrite())
        return;
    const QString key = repo.owner.trimmed().toLower() + QLatin1Char('/') +
                        repo.name.trimmed().toLower();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kSweepIntervalMs = 120 * 1000;
    constexpr qint64 kSweepFloorMs = 10 * 1000;
    const qint64 lastMs = m_pullLedgerSweepAtMs.value(key, 0);
    if (nowMs - lastMs < (force ? kSweepFloorMs : kSweepIntervalMs))
        return;
    m_pullLedgerSweepAtMs.insert(key, nowMs);
    m_pullLedgerSweepInProgress = true;

    // The served ledger can carry pulls this node never drained: a mirror
    // that won the inbox lease while its forkmesh/pulls checkout was behind
    // commits them onto a stale base, and they never reach this inbox at all
    // (how pulls #92/#93 went missing on 2026-08-09).
    // Managed mirrors are already configured as SSH remotes on the source
    // checkout. Reading their pull ledger directly keeps the Pulls page useful
    // during a relay outage and avoids a Worker invocation in the common case.
    // Bound fallback to two distinct nodes so a retired fleet cannot turn one
    // refresh into an unbounded sequence of TCP timeouts.
    const QString remoteConfigPath =
        !writable.localPath.trimmed().isEmpty() && QDir(writable.localPath).exists()
            ? writable.localPath
            : writable.mirrorPath;
    struct ActiveHost {
        QString node;
        QSet<QString> addresses;
    };
    QList<ActiveHost> activeHosts;
    {
        QSettings settings;
        const QJsonArray hosts = forkmesh::control::loadSavedHosts(
            settings, kHostsSetting, nullptr);
        for (const QJsonValue &value : hosts) {
            const QJsonObject host = value.toObject();
            const QString status =
                host.value(QStringLiteral("status"))
                    .toString(QStringLiteral("installed"))
                    .trimmed().toLower();
            if (status == QLatin1String("added") ||
                status == QLatin1String("installing") ||
                status == QLatin1String("install failed") ||
                status == QLatin1String("uninstalling") ||
                status == QLatin1String("uninstalled"))
                continue;
            ActiveHost active;
            active.node = host.value(QStringLiteral("name"))
                              .toString().trimmed().toLower();
            for (const QString &address : {
                     host.value(QStringLiteral("ip")).toString(),
                     host.value(QStringLiteral("dnsHostname")).toString(),
                     QUrl(host.value(QStringLiteral("publicOrigin")).toString())
                         .host()}) {
                const QString normalized = address.trimmed().toLower();
                if (!normalized.isEmpty())
                    active.addresses.insert(normalized);
            }
            if (!active.node.isEmpty())
                activeHosts.append(active);
        }
    }
    QUrl relayRemote = catalogApiUrl();
    relayRemote.setPath("/" + repoSegment(repo.owner, QStringLiteral("owner")) +
                        "/" +
                        repoSegment(repo.name, QStringLiteral("repository")));
    const QString relayRemoteUrl = relayRemote.toString();
    // Remote discovery is a Git subprocess too. Keeping it on the GUI thread
    // accumulated 96.8 seconds of ui-blocking waits across 13 sweeps (43.5
    // seconds worst case), even though the network fetch below was already on
    // a worker. Discover and fetch in one worker phase; the local adopt/flip
    // mutations stay on the GUI thread where every other PullStore write lives.
    struct PullLedgerFetch {
        bool fetched = false;
        QString remoteUrl;
    };
    runOffThread<PullLedgerFetch>(
        [store, remoteConfigPath, activeHosts, relayRemoteUrl] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("pulls"),
                QStringLiteral("fetch served pull ledger"),
                forkmesh::ActionTelemetry::Execution::Worker);
            QStringList remoteUrls;
            QByteArray remotesOut;
            if (!remoteConfigPath.trimmed().isEmpty() &&
                cachedRepoRemotesRaw(remoteConfigPath, &remotesOut)) {
                for (const QString &line :
                     QString::fromUtf8(remotesOut).split(QLatin1Char('\n'))) {
                    const QStringList parts =
                        line.simplified().split(QLatin1Char(' '));
                    if (parts.size() < 2)
                        continue;
                    const QString candidate = parts.at(1).trimmed();
                    if (!candidate.startsWith(QLatin1String("ssh://")) ||
                        remoteUrls.contains(candidate))
                        continue;
                    const QString candidateHost =
                        QUrl(candidate).host().trimmed().toLower();
                    bool active = false;
                    for (const ActiveHost &host : std::as_const(activeHosts)) {
                        if (host.addresses.contains(candidateHost) ||
                            mirrorGatewayHostBelongsToNode(candidateHost,
                                                           host.node)) {
                            active = true;
                            break;
                        }
                    }
                    if (!active)
                        continue;
                    remoteUrls.append(candidate);
                    if (remoteUrls.size() == 2)
                        break;
                }
            }
            if (!remoteUrls.contains(relayRemoteUrl))
                remoteUrls.append(relayRemoteUrl);
            for (const QString &remoteUrl : remoteUrls) {
                PullStore fetcher = store;
                if (fetcher.prefetchForeignLedger(remoteUrl))
                    return PullLedgerFetch{true, remoteUrl};
            }
            return PullLedgerFetch{};
        },
        [this, store, repo, key](PullLedgerFetch fetch) mutable {
            const int repoIndexAtStart = m_repoDetailIndex;
            QString adoptError;
            QList<int> adopted;
            if (fetch.fetched)
                adopted = store.adoptForeignPulls(fetch.remoteUrl, &adoptError,
                                                  /*fetchFirst=*/false);
            if (!adoptError.isEmpty())
                logSystem(QStringLiteral("Pull ledger sync for %1: %2")
                              .arg(key, adoptError));
            QString flipError;
            QList<int> rewarded;
            const QList<int> flipped =
                store.reconcileMergedPulls(&flipError, &rewarded);
            if (!flipError.isEmpty())
                logSystem(QStringLiteral("Pull merge reconcile for %1: %2")
                              .arg(key, flipError));
            m_pullLedgerSweepInProgress = false;

            if (!fetch.fetched && adopted.isEmpty() && flipped.isEmpty() &&
                rewarded.isEmpty())
                return;
            auto joined = [](const QList<int> &numbers) {
                QStringList parts;
                for (const int number : numbers)
                    parts.append(QStringLiteral("#%1").arg(number));
                return parts.join(QStringLiteral(", "));
            };
            if (!adopted.isEmpty())
                logSystem(
                    QStringLiteral("Adopted mirror-drained pull request%1 %2 in %3.")
                        .arg(adopted.size() == 1 ? QString()
                                                 : QStringLiteral("s"),
                             joined(adopted), key));
            if (!flipped.isEmpty())
                logSystem(QStringLiteral(
                              "Closed pull request%1 %2 in %3 — the branch "
                              "already landed in the base.")
                              .arg(flipped.size() == 1 ? QString()
                                                       : QStringLiteral("s"),
                                   joined(flipped), key));
            if (!rewarded.isEmpty()) {
                const QString plural =
                    rewarded.size() == 1 ? QString() : QStringLiteral("s");
                logSystem(QStringLiteral(
                              "Merged pull request%1 %2 in %3 — reward "
                              "estimate%4 queued for payout.")
                              .arg(plural, joined(rewarded), key, plural));
            }
            // The pumped git waits above may have navigated the UI; only
            // refresh the pulls view when the same repo detail is still open.
            if (repoIndexAtStart == m_repoDetailIndex && m_repoDetailIndex >= 0 &&
                m_repoDetailIndex < m_repositories.size()) {
                const RepositoryRecord &current =
                    m_repositories.at(m_repoDetailIndex);
                if (current.owner.trimmed().toLower() ==
                        repo.owner.trimmed().toLower() &&
                    current.name.trimmed().toLower() ==
                        repo.name.trimmed().toLower())
                    reloadPullsInBackground();
            }
        });
}

// Catch-up pass for pull-request checks on a repository nobody has opened. The
// interactive path (applyLoadedPulls) covers the repo on screen and the inbox
// drain covers a submission as it lands, but a pull request that arrived while
// this node was closed would otherwise wait for a click, so the same relay-sync
// trigger that reconciles the ledger also re-checks the open pulls.
void MainWindow::sweepPullChecksFor(RepositoryRecord repo)
{
    if (!m_actionStore || repo.previewOnly || !repo.actionsEnabled ||
        repo.externallyManagedActions)
        return;
    const RepositoryRecord writable = writableRecordFor(repo);
    PullStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                    m_userName);
    if (!store.canWrite())
        return; // not this node's repository to run checks for
    const QString key = repo.owner.trimmed().toLower() + QLatin1Char('/') +
                        repo.name.trimmed().toLower();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kSweepIntervalMs = 5 * 60 * 1000;
    if (nowMs - m_pullChecksSweepAtMs.value(key, 0) < kSweepIntervalMs)
        return;
    m_pullChecksSweepAtMs.insert(key, nowMs);
    runOffThread<QList<PullRequest>>(
        [store] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("pulls"),
                QStringLiteral("load pulls for check sweep"),
                forkmesh::ActionTelemetry::Execution::Worker);
            return store.loadAll();
        },
        [this, writable](QList<PullRequest> pulls) {
            queueChecksForOpenPulls(writable, std::move(pulls));
        });
}

void MainWindow::drainPullsInboxFor(RepositoryRecord repo, bool interactive,
                                    bool force)
{
    // A local-only repository has no relay inbox to drain, and the drain URL
    // would carry its owner and name to the relay. Fail closed before any
    // signing or network work.
    if (!PrivateMirrorRuntime::repositoryRelayInboxAllowed(
            repo.publishToNetwork))
        return;
    const RepositoryRecord writable = writableRecordFor(repo);
	PullStore probe(writable.localPath, writable.mirrorPath, &m_profileIdentity,
					m_userName);
	if (!probe.canWrite())
        return;
    if (!hasOwnerSigningCapability())
        return;

    QUrl url = pullsApiUrl(repo);
	const QString signer =
		repoSegment(repo.owner, QStringLiteral("owner"));
	if (signer.isEmpty())
        return;
	const QString backoffKey = url.toString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!interactive && !force && !m_pollBackoff.ready(backoffKey, nowMs))
        return;
    const QString ts = QString::number(nowMs);
    const QByteArray canonical =
        ("forkmesh-issues-pull-v1\n" + signer + "\n" + ts).toUtf8();
    const QString sig = m_profileIdentity.signData(canonical);
    QUrlQuery query;
    query.addQueryItem("owner", signer);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", sig);
    url.setQuery(query);

    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, interactive, backoffKey] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            noteInboxDrainFailure(
                backoffKey,
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
					.toInt());
            if (interactive)
                QMessageBox::warning(this, "Sync inbox",
                                     "Could not reach the inbox: " +
                                         reply->errorString());
            return;
        }
        noteInboxDrainSuccess(backoffKey);
        const QJsonArray pending =
            QJsonDocument::fromJson(reply->readAll())
                .object()
                .value("pending")
                .toArray();
		applyPullsInboxPayload(repo, pending, interactive);
    });
}

void MainWindow::applyPullsInboxPayload(const RepositoryRecord &repo,
                                        const QJsonArray &pending,
										bool interactive)
{
    if (!hasOwnerSigningCapability())
        return;
    if (pending.isEmpty()) {
        if (interactive)
            QMessageBox::information(this, "Sync inbox",
                                     "No pending pull requests.");
        return;
    }
    const RepositoryRecord writable = writableRecordFor(repo);
    PullStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                    m_userName);
    if (!store.canWrite())
        return;
    int merged = 0;
    bool branchReviewsChanged = false;
    QStringList drainedIds;
    QString lastAuthor;
    QString lastTitle;
    for (const QJsonValue &value : pending) {
        const QJsonObject obj = value.toObject();
        const QString inboxId =
            obj.value("id").isDouble()
                ? QString::number(
                      static_cast<qint64>(obj.value("id").toDouble()))
                : QString();
        const QString reviewBranch = obj.value("branch").toString();
        if (obj.contains("event") && !reviewBranch.isEmpty()) {
            const PullEvent ev =
                PullEvent::fromJson(obj.value("event").toObject());
            if (store.applyRemoteBranchEvent(reviewBranch, ev)) {
                if (!inboxId.isEmpty())
                    drainedIds << inboxId;
                ++merged;
                lastAuthor = ev.authorName.isEmpty() ? ev.author.left(8)
                                                     : ev.authorName;
                lastTitle = QStringLiteral("review on %1").arg(reviewBranch);
                branchReviewsChanged = true;
            }
            continue;
        }
        if (obj.contains("event")) {
            const int number = obj.value("number").toInt();
            const PullEvent ev =
                PullEvent::fromJson(obj.value("event").toObject());
            const bool applied = PullStore::isLifecycleEvent(ev)
                                     ? store.applyRemoteLifecycleEvent(number, ev)
                                     : store.applyRemoteEvent(number, ev);
            if (applied) {
                if (!inboxId.isEmpty())
                    drainedIds << inboxId;
                ++merged;
                lastAuthor = ev.authorName.isEmpty() ? ev.author.left(8)
                                                     : ev.authorName;
                lastTitle = QStringLiteral("review on #%1").arg(number);
            }
            continue;
        }
        const PullRequest pr =
            PullRequest::fromJson(obj.value("pull").toObject());
        if (store.applyRemotePull(pr)) {
            if (!inboxId.isEmpty())
                drainedIds << inboxId;
            ++merged;
            lastAuthor = pr.authorName.isEmpty() ? pr.author.left(8)
                                                 : pr.authorName;
            lastTitle = pr.title;
        }
    }
    if (!drainedIds.isEmpty()) {
        QUrl ackUrl = pullsApiUrl(repo);
		const QString ackSigner =
			repoSegment(repo.owner, QStringLiteral("owner"));
        QUrlQuery query = signedInboxQuery(ackSigner);
        query.addQueryItem(QStringLiteral("ids"),
                           drainedIds.join(QStringLiteral(",")));
        ackUrl.setQuery(query);
        m_networkAccess->deleteResource(QNetworkRequest(ackUrl));
    }
    const bool onThisRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size() &&
        m_repositories.at(m_repoDetailIndex).owner == repo.owner &&
        m_repositories.at(m_repoDetailIndex).name == repo.name;
    if (onThisRepo)
        reloadPulls();
	if (merged > 0) {
        propagateRepoUpdate(repoIndexFor(repo.owner, repo.name));
        scanRepoMentionsFor(writable);
        // A pull request that just arrived from another node gets its checks
        // now, whether or not this repository happens to be on screen. reloadPulls()
        // above already did it for the open one.
        if (!onThisRepo) {
            m_pullChecksSweepAtMs.remove(repo.owner.trimmed().toLower() +
                                         QLatin1Char('/') +
                                         repo.name.trimmed().toLower());
            sweepPullChecksFor(repo);
        }
    }
    if (interactive) {
        QMessageBox::information(
            this, "Sync inbox",
            QStringLiteral("Merged %1 pull request(s) into .forkmesh/pulls/.")
                .arg(merged));
    } else if (merged > 0) {
        const QString body =
            merged == 1
                ? QStringLiteral("%1 opened a pull request: %2")
                      .arg(lastAuthor, lastTitle)
                : QStringLiteral("%1 new pull requests on %2/%3")
                      .arg(merged)
                      .arg(repo.owner, repo.name);
        flashPingMessage(body);
        if (notifyEnabled(kPullAlertSetting))
            notifyIfInactive(
                QString::fromUtf8("ForkMesh \xE2\x80\x94 new pull request"), body);
        if (notifyEnabled(kPullAlertSetting))
            showPingTrayMessage(QStringLiteral("ForkMesh — new pull request"),
                                body);
        if (onThisRepo)
            triggerTeammatesForEvent(QStringLiteral("new-pulls"), body);
    }
}

void MainWindow::pollOwnedInboxes()
{
    if (!m_networkAccess || !hasOwnerSigningCapability())
        return;
    const bool autoSyncIssues =
        QSettings().value(kAutoSyncIssuesSetting, true).toBool();
    QSet<QString> seen;
    for (const RepositoryRecord &repo : m_repositories) {
        if (repo.previewOnly)
            continue;
        const QString key = repo.owner + "/" + repo.name;
        if (seen.contains(key))
            continue;
        const RepositoryRecord writable = writableRecordFor(repo);
        IssueStore probe(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                         m_userName);
        if (!probe.canWrite())
            continue; // not the owner of this repo; nothing to drain
        seen.insert(key);
        if (autoSyncIssues)
            queueAutomaticIssueSync(repo);
        drainPullsInboxFor(repo, /*interactive=*/false);
        drainDiscussionsInboxFor(repo, /*interactive=*/false);
        reconcilePullLedgerFor(repo);
    }
}

void MainWindow::queueAutomaticIssueSync(RepositoryRecord repo,
                                         QJsonArray pending,
                                         bool applyPayload,
                                         QString expectedAccount,
                                         QString expectedRelayOrigin)
{
    AutomaticIssueSyncRequest request;
    request.repo = std::move(repo);
    request.writablePath = normalizedIssueSyncPath(
        writableRecordFor(request.repo).localPath);
    if (request.writablePath.isEmpty())
        return;
    request.account = expectedAccount.trimmed().isEmpty()
        ? accountOwner().trimmed().toLower()
        : expectedAccount.trimmed().toLower();
    request.relayOrigin = expectedRelayOrigin.trimmed().isEmpty()
        ? normalizedRelayOrigin(catalogApiUrl())
        : expectedRelayOrigin.trimmed();
    request.pending = std::move(pending);
    request.applyPayload = applyPayload;
    launchAutomaticIssueSync(std::move(request));
}

void MainWindow::launchAutomaticIssueSync(AutomaticIssueSyncRequest request)
{
    const QString key = request.repo.owner.trimmed().toLower() +
                        QLatin1Char('/') +
                        request.repo.name.trimmed().toLower() +
                        QLatin1Char('|') + request.writablePath;
    if (m_automaticIssueSyncChecksInFlight.contains(key)) {
        // Relay inbox reads are snapshots of an unacknowledged queue, so the
        // newest request supersedes an older request that has not started yet.
        // It intentionally does not share the active probe's possibly stale
        // verdict: completion below launches a fresh check for this snapshot.
        m_automaticIssueSyncPending.insert(key, std::move(request));
        return;
    }

    m_automaticIssueSyncChecksInFlight.insert(key);
    const QString writablePath = request.writablePath;
    runOffThread<bool>(
        [writablePath] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("issues"),
                QStringLiteral("check automatic issue-sync worktree"),
                forkmesh::ActionTelemetry::Execution::Worker);
            return worktreeTrackedClean(writablePath);
        },
        [this, key, request = std::move(request)](bool clean) mutable {
            // Keep the key marked in flight while applying. If applying the
            // active request causes another sync notification, that newest
            // snapshot replaces the pending request instead of starting a
            // concurrent git status or being overwritten by an older one.
            bool stillCurrent = m_networkAccess &&
                accountOwner().trimmed().compare(request.account,
                                                 Qt::CaseInsensitive) == 0 &&
                normalizedRelayOrigin(catalogApiUrl()) == request.relayOrigin;
            RepositoryRecord liveRepo;
            if (stillCurrent) {
                const int liveIndex =
                    repoIndexFor(request.repo.owner, request.repo.name);
                if (liveIndex >= 0)
                    liveRepo = m_repositories.at(liveIndex);
                stillCurrent = liveIndex >= 0 && normalizedIssueSyncPath(
                    writableRecordFor(liveRepo).localPath) == request.writablePath;
            }
            if (clean && stillCurrent) {
                if (request.applyPayload) {
                    applyIssuesInboxPayload(liveRepo, request.pending,
                                            /*interactive=*/false);
                } else {
                    drainIssuesInboxFor(liveRepo, /*interactive=*/false);
                }
            }

            m_automaticIssueSyncChecksInFlight.remove(key);
            const auto pendingIt = m_automaticIssueSyncPending.find(key);
            if (pendingIt == m_automaticIssueSyncPending.end())
                return;
            AutomaticIssueSyncRequest next = std::move(pendingIt.value());
            m_automaticIssueSyncPending.erase(pendingIt);
            launchAutomaticIssueSync(std::move(next));
        });
}

// owner/ts/sig query params carrying a freshly-signed forkmesh-issues-pull-v1
// drain token — the shared auth for inbox GET/DELETE and GET /api/sync.
QUrlQuery MainWindow::signedInboxQuery(const QString &owner) const
{
    // `owner` may be a public organization alias. The server resolves the
    // alias and verifies this device against the linked org's current
    // owner/admin membership; locally we only require a valid desktop signing
    // capability and never infer authorization from the alias string.
    if (!hasOwnerSigningCapability())
        return QUrlQuery();
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-issues-pull-v1\n" + owner + "\n" + ts).toUtf8();
    QUrlQuery query;
    query.addQueryItem("owner", owner);
    query.addQueryItem("ts", ts);
    query.addQueryItem("sig", m_profileIdentity.signData(canonical));
    return query;
}

void MainWindow::scheduleRelaySync()
{
    if (!hasOwnerSigningCapability())
        return;
    if (!m_relaySyncDebounce) {
        m_relaySyncDebounce = new QTimer(this);
        m_relaySyncDebounce->setSingleShot(true);
        m_relaySyncDebounce->setInterval(2000);
        connect(m_relaySyncDebounce, &QTimer::timeout, this,
                &MainWindow::performRelaySync);
    }
    if (!m_relaySyncDebounce->isActive())
        m_relaySyncDebounce->start();
}

// One signed GET /api/sync returns everything the relay holds for this
// account across every owned repo — pending issue/pull/discussion/commit
// inbox items and queued agent prompts — and the shared apply* helpers merge
// each slice exactly as the old per-topic drains did. Runs when a relay event
// frame arrives (scheduleRelaySync), once per event-socket (re)connect as the
// catch-up for anything queued while the channel was down, and once shortly
// after launch. There is no periodic fallback tick.
void MainWindow::performRelaySync()
{
    if (!m_networkAccess)
        return;
    const QString account = m_accountName.isEmpty()
        ? QSettings().value(kAccountNameSetting).toString().trimmed()
        : m_accountName;
    if (!hasOwnerSigningCapability(account) || !m_profileIdentity.isValid())
        return;
    // Ledger convergence rides the same trigger as the inbox drains: the
    // relay frame announcing new pull activity fires whether this node or a
    // mirror wins the drain lease, so pulls a mirror committed while its
    // ledger was behind converge here (throttled inside per repo).
    {
        QSet<QString> swept;
        for (const RepositoryRecord &repo : m_repositories) {
            if (repo.previewOnly)
                continue;
            const QString sweepKey = repo.owner + "/" + repo.name;
            if (swept.contains(sweepKey))
                continue;
            swept.insert(sweepKey);
            reconcilePullLedgerFor(repo);
            sweepPullChecksFor(repo);
        }
    }
    if (!m_relaySyncSupported) {
        pollOwnedInboxes();
        drainAgentPrompts();
        return;
    }
    if (m_relaySyncInFlight)
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_pollBackoff.ready(QStringLiteral("relaySync"), nowMs))
        return;
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/sync"));
    url.setQuery(signedInboxQuery(repoSegment(account, QStringLiteral("owner"))));
    const QString syncRelayOrigin = normalizedRelayOrigin(url);
    m_relaySyncInFlight = true;
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, account, syncRelayOrigin] {
        m_relaySyncInFlight = false;
        reply->deleteLater();
        if (!hasOwnerSigningCapability(account))
            return;
        if (reply->error() != QNetworkReply::NoError) {
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 404 ||
                reply->error() == QNetworkReply::ContentNotFoundError) {
                m_relaySyncSupported = false;
                pollOwnedInboxes();
                drainAgentPrompts();
                return;
            }
            if (status == 401 || status == 403) {
                // The relay rejected this node's signed drain: its key is not one
                // the account currently trusts (never linked, or a key the relay
                // has since dropped). This used to fail totally silently, so a
                // node just "stopped syncing" with no clue — issues/chats/etc.
                // piled up online and never arrived. Surface it once, pointing at
                // the re-link flow, instead of only backing off.
                static bool s_relaySyncAuthWarned = false;
                if (!s_relaySyncAuthWarned) {
                    s_relaySyncAuthWarned = true;
                    logSystem(QStringLiteral(
                                  "The relay rejected this node's sign-in (HTTP "
                                  "%1), so new issues, chats and other updates "
                                  "can't sync down. Re-link this node to your "
                                  "account from Settings to reconnect.")
                                  .arg(status));
                }
            }
            m_pollBackoff.noteFailure(QStringLiteral("relaySync"),
                                      QDateTime::currentMSecsSinceEpoch());
            return;
        }
        m_pollBackoff.noteSuccess(QStringLiteral("relaySync"));
        const QJsonArray repos = QJsonDocument::fromJson(reply->readAll())
                                     .object()
                                     .value("repos")
                                     .toArray();
        const bool autoSyncIssues =
            QSettings().value(kAutoSyncIssuesSetting, true).toBool();
        QSet<int> covered;
        for (const QJsonValue &value : repos) {
            const QJsonObject entry = value.toObject();
            const QString entryOwner = entry.value("owner").toString();
            const QString entryName = entry.value("name").toString();
            int idx = -1;
            int aliasIdx = -1;
            for (int i = 0; i < m_repositories.size(); ++i) {
                const RepositoryRecord &r = m_repositories.at(i);
                if (r.previewOnly ||
                    r.name.compare(entryName, Qt::CaseInsensitive) != 0)
                    continue;
                if (r.owner.compare(entryOwner, Qt::CaseInsensitive) == 0) {
                    idx = i;
                    break;
                }
                if (aliasIdx < 0 &&
                    catalogOwner(r).compare(entryOwner,
                                            Qt::CaseInsensitive) == 0)
                    aliasIdx = i;
            }
            if (idx < 0)
                idx = aliasIdx;
            if (idx < 0) {
                static QSet<QString> s_unmatchedWarned;
                const QString key = entryOwner + QLatin1Char('/') + entryName;
                if (!s_unmatchedWarned.contains(key)) {
                    s_unmatchedWarned.insert(key);
                    logSystem(QStringLiteral(
                                  "Relay sync returned %1, which no local "
                                  "repository matches, so its queued issues, "
                                  "pulls and prompts cannot be applied.")
                                  .arg(key));
                }
                continue;
            }
            covered.insert(idx);
            const RepositoryRecord repo = m_repositories.at(idx);
            // Organization-agent jobs use their own authenticated lease
            // endpoint rather than the owner-E2EE agentPrompts slice below.
            // Run that drain on the normal consolidated-sync path as well as
            // the legacy fallback so startup and reconnect catch up work that
            // was durably queued while this desktop was offline.
            drainOrgAgentJobsFor(repo);
            if (autoSyncIssues)
                queueAutomaticIssueSync(repo, entry.value("issues").toArray(),
                                        /*applyPayload=*/true, account,
                                        syncRelayOrigin);
            applyPullsInboxPayload(repo, entry.value("pulls").toArray(),
                                   /*interactive=*/false);
            applyDiscussionsInboxPayload(repo,
                                         entry.value("discussions").toArray(),
                                         /*interactive=*/false);
            // An organization owner merged a pull request on the web while
            // this node was offline: a mirror landed it, the relay pinned the
            // merged state, and the durable receipt rides this sync. The
            // base branch and the pull ledger on the mesh are now ahead of
            // this checkout, so converge from the mesh right away (fast-
            // forward only; local-only work is never clobbered) instead of
            // waiting for a peer's advert. The ledger sweep above then flips
            // the merged pull locally once its branch is contained in main.
            if (!entry.value("mirrorMerges").toArray().isEmpty() &&
                !m_syncingRepos.contains(idx)) {
                const QString selfAccount = accountOwner().trimmed().toLower();
                const bool sourceOfTruth =
                    !repo.isPrivate && repo.publishToNetwork &&
                    !repo.localPath.trimmed().isEmpty() &&
                    catalogOwner(repo).trimmed().toLower() == selfAccount;
                if (sourceOfTruth)
                    convergeSourceRepoFromMesh(idx);
                else
                    syncRepository(idx, /*quiet=*/true);
            }
            applyAgentPromptsPayload(repo, entry.value("agentPrompts").toArray());
            // Branch management the owner asked for on the website. The relay
            // hands each request over exactly once (drain-on-read), so it is
            // applied here and the outcome is logged either way.
            applyWebBranchOps(repo, entry.value("branchOps").toArray());
            const QJsonObject aboutUpdate = entry.value("aboutUpdate").toObject();
            if (!aboutUpdate.isEmpty()) {
                QString aboutError;
                if (applyRepoAboutMetadataAt(
                        idx, aboutUpdate.value("about").toString(),
                        aboutUpdate.value("website").toString(), &aboutError)) {
                    logSystem(QStringLiteral(
                                  "Applied About details edited on the website "
                                  "for %1/%2.")
                                  .arg(entryOwner, entryName));
                } else {
                    logSystem(QStringLiteral(
                                  "Could not apply the website About edit for "
                                  "%1/%2: %3")
                                  .arg(entryOwner, entryName, aboutError));
                }
            }
        }
        // Repos the consolidated response never named. The per-repo drains
        // address the relay by the repo's PUBLIC owner (repo.owner) rather than
        // by the account, so they read the queue the website actually files
        // into. Each one self-gates on write/signing capability and shares
        // m_pollBackoff with every other inbox call, so this adds no polling
        // for a node whose repos the consolidated sync already covers — the
        // set is empty in that case.
        for (int i = 0; i < m_repositories.size(); ++i) {
            if (covered.contains(i))
                continue;
            const RepositoryRecord uncovered = m_repositories.at(i);
            if (uncovered.previewOnly ||
                uncovered.owner.trimmed().isEmpty() ||
                uncovered.name.trimmed().isEmpty())
                continue;
            if (autoSyncIssues)
                queueAutomaticIssueSync(uncovered, {}, /*applyPayload=*/false,
                                        account, syncRelayOrigin);
            drainPullsInboxFor(uncovered, /*interactive=*/false);
            drainDiscussionsInboxFor(uncovered, /*interactive=*/false);
            drainWebBranchOpsFor(uncovered);
        }
        reportCompletedOrgAgentJobs();
    });
}
