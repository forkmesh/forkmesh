
#include "MainWindow.h"
#include "OfficeChannelMirror.h"
#include "PingSettings.h"
#include "Tooltips.h"
#include "MainWindowInternal.h"
#include "ChatListPreview.h"
#include "DiscordClient.h"
#include "FederatedThreadView.h"
#include "CurrentPageStack.h"
#include "FlowLayout.h"
#include "KebabHeaderView.h"
#include "McpConnector.h"
#include "MirrorCrypto.h"
#include "PrivateMirrorRuntime.h"
#include "PromptDraftStore.h"
#include "PromptSpellCheckHighlighter.h"
#include "ReactionEmoji.h"
#include "UiSounds.h"

#include <QDateEdit>
#include <QDrag>
#include <QFontDatabase>
#include <QImageReader>
#include <QLayoutItem>
#include <QPair>
#include <QPixmap>
#include <QStackedLayout>
#include <QStandardPaths>
#include <QVariantAnimation>

#include <cmath>
#include <limits>

using namespace forkmesh::ui;

namespace {
// The inline assignee field holds a comma-separated list, so its completer
// matches and replaces only the name after the last comma: typing "alice, bo"
// offers "bob", and picking it keeps "alice, " in front (issue #688).
class IssueAssigneeCompleter : public QCompleter
{
public:
    using QCompleter::QCompleter;

    QStringList splitPath(const QString &path) const override
    {
        return {path.mid(path.lastIndexOf(QLatin1Char(',')) + 1).trimmed()};
    }

    QString pathFromIndex(const QModelIndex &index) const override
    {
        return withLastName(QCompleter::pathFromIndex(index));
    }

    // Enter on a highlighted suggestion only picks it. QCompleter would also
    // pass the key on to the field, whose returnPressed saves the list before
    // the next name is typed; Enter with nothing highlighted still saves.
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == popup() && event->type() == QEvent::KeyPress &&
            popup()->isVisible()) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            const QModelIndex current = popup()->currentIndex();
            if ((key == Qt::Key_Return || key == Qt::Key_Enter) &&
                current.isValid()) {
                const QString text =
                    withLastName(current.data(completionRole()).toString());
                popup()->hide();
                if (auto *edit = qobject_cast<QLineEdit *>(widget()))
                    edit->setText(text);
                return true;
            }
        }
        return QCompleter::eventFilter(watched, event);
    }

private:
    QString withLastName(const QString &name) const
    {
        const auto *edit = qobject_cast<QLineEdit *>(widget());
        const QString text = edit ? edit->text() : QString();
        const int comma = text.lastIndexOf(QLatin1Char(','));
        return comma < 0 ? name : text.left(comma + 1) + QLatin1Char(' ') + name;
    }
};

// Assigning an issue appends a signed event to that repository's own issue log,
// so the save only lands where the log is writable: the machine hosting the
// repository. Everywhere else the sidebar says so before the edit instead of
// accepting a selection and failing the save (issue #638).
const QString kIssueAssigneesHostOnlyNotice =
    QStringLiteral("Assignees are editable on the repository host.");
// Re-crediting a report appends a signed event to the same log, so it lands
// under the same rule.
const QString kIssueReporterHostOnlyNotice =
    QStringLiteral("The reporter is editable on the repository host.");

enum IssueSummaryRole {
    IssueStatusRole = Qt::UserRole + 20,
    IssuePriorityRole,
    IssueVotesRole,
    IssueLabelsRole,
    IssueLabelColorsRole,
    IssueMilestoneRole,
    IssueCreatedRole,
    IssueAuthorRole,
    IssueAssigneesRole,
    IssueProgressRole,
    IssueFilesRole,
    IssueAttachmentRole, // absolute path of the image to preview, when any
    IssueAgentRole,
    IssueAgentActiveRole, // an agent session is working on this issue right now
    IssueAgentAngleRole,  // head of the fireball orbiting the assignee badge
    IssueAuthorAvatarRole,   // reporter's face, same one the timeline shows
    IssueAssigneeAvatarRole, // first assignee's face for the corner badge
};

QString compactIssueAge(qint64 timestampMs)
{
    timestampMs = issueDisplayTimestampMs(timestampMs);
    if (timestampMs <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    const qint64 seconds = qMax<qint64>(
        0, (QDateTime::currentMSecsSinceEpoch() - timestampMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("now");
    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return QStringLiteral("%1m").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 24)
        return QStringLiteral("%1h").arg(hours);
    const qint64 days = hours / 24;
    if (days < 30)
        return QStringLiteral("%1d").arg(days);
    const qint64 months = days / 30;
    return months < 12 ? QStringLiteral("%1mo").arg(months)
                       : QStringLiteral("%1y").arg(months / 12);
}

QColor issuePriorityColor(int priority)
{
    static const QColor colors[] = {
        QColor("#B91C1C"), QColor("#EF4444"), QColor("#F97316"),
        QColor("#F59E0B"), QColor("#EAB308"), QColor("#84CC16"),
        QColor("#14B8A6"), QColor("#06B6D4"), QColor("#3B82F6")};
    return priority >= 1 && priority <= 9 ? colors[priority - 1]
                                           : QColor("#64748B");
}

QString issuePriorityMeaning(int priority)
{
    static const QStringList meanings{
        QStringLiteral("Critical"), QStringLiteral("Very high"),
        QStringLiteral("High"), QStringLiteral("Moderate"),
        QStringLiteral("Elevated"), QStringLiteral("Normal"),
        QStringLiteral("Below normal"), QStringLiteral("Low"),
        QStringLiteral("Very low")};
    return priority >= 1 && priority <= meanings.size()
               ? meanings.at(priority - 1)
               : QStringLiteral("Minimal");
}

QString issueInitials(const QString &name)
{
    const QStringList words = name.simplified().split(QLatin1Char(' '),
                                                       Qt::SkipEmptyParts);
    if (words.isEmpty())
        return QStringLiteral("?");
    return (words.first().left(1) +
            (words.size() > 1 ? words.last().left(1) : QString()))
        .toUpper();
}

QColor stableAvatarColor(const QString &name)
{
    static const QColor colors[] = {QColor("#8250df"), QColor("#0969da"),
                                    QColor("#1a7f37"), QColor("#bf8700"),
                                    QColor("#cf222e"), QColor("#0550ae")};
    return colors[qHash(name) % (sizeof(colors) / sizeof(colors[0]))];
}

QString issueFullDateTime(qint64 timestampMs)
{
    timestampMs = issueDisplayTimestampMs(timestampMs);
    if (timestampMs <= 0)
        return QStringLiteral("Unknown date and time");
    return QDateTime::fromMSecsSinceEpoch(timestampMs)
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss t"));
}

// Geometry of one summary row, shared by the delegate's painter and the list's
// hit-testing so a click always lands on the chip the user actually sees.
// Layout: the progress rail hugs the pane's left edge, then the reporter's
// avatar with the assignee badge on its bottom-right corner, then the status
// glyph with the priority number tucked underneath it. The first line carries
// the title alone; the second opens with the vote chip and the attachment
// preview and runs on into "#123 · author 4d", the milestone, the label pills
// and the agent. Nothing is drawn against the row's right edge.
struct IssueRowGeometry {
    QRect rail;       // vertical progress rail
    QRect avatar;     // reporter (author) avatar, double height
    QRect assignee;   // assignee badge, on the avatar's bottom-right corner
    QRect status;     // open/closed glyph
    QRect priority;   // priority number, under the glyph
    QRect title;      // elided title text
    QList<QPair<QString, QRect>> labels; // expanded label pills, on the meta line
    QRect meta;       // "#123 · author 4d" line
    QRect milestone;  // milestone ring + name, inside the meta line
    QString milestoneName;
    QRect agent;      // trailing "· agent", whatever the meta line has left
    QRect votes;      // thumbs-up + count, clickable
    QRect files;      // attachment preview, clickable
};

constexpr int kIssueLabelPillHeight = 17;
constexpr int kIssueLabelPillPad = 7;
constexpr int kIssueMilestoneRing = 14;
constexpr int kIssueThumbSide = 20;  // attachment preview tile
constexpr int kIssueVoteGlyph = 13;  // thumbs-up in the row's vote chip
constexpr int kIssueAssigneeBadge = 13; // assignee circle over the avatar
constexpr int kIssueAssigneeOverhang = 2; // how far it hangs off the avatar
// Who a timeline face was drawn for, stamped on the label that carries it.
// The user directory that holds everyone's real picture is fetched after the
// thread is already on screen, and rebuilding the thread to swap a pixmap in
// would scroll the reader back to the top, so refreshIssueAvatars() finds the
// labels by these and re-resolves each face in place.
constexpr const char *kIssueAvatarAuthorIdProperty = "forkmeshAvatarAuthorId";
constexpr const char *kIssueAvatarAuthorNameProperty =
    "forkmeshAvatarAuthorName";
constexpr const char *kIssueAvatarSideProperty = "forkmeshAvatarSide";
// The fireball orbits outside the badge, so its pixmap is much wider than the
// badge: agentRunningFireballPixmap puts the orbit at 0.31 of the side it is
// given, and the head's glow adds another 0.18 on top of that. 34 is as large
// as the orbit can be before the head clips the row's bottom edge.
constexpr int kIssueAssigneeOrbit = 34;
// Degrees the head travels per 110 ms list tick, the quiet-agent orbit of the
// Agents page translated into this list's slower cadence.
constexpr qreal kIssueFireballStepDegrees = 20.0;

QFont issueRowMetaFont(const QFont &base)
{
    QFont metaFont = base;
    metaFont.setPixelSize(qMax(10, base.pixelSize() - 1));
    return metaFont;
}

QFont issueRowTitleFont(const QFont &base)
{
    QFont titleFont = base;
    titleFont.setWeight(QFont::DemiBold);
    return titleFont;
}

// Assignees drawn on the right; the reporter already has the big left avatar,
// so an author who assigned the issue to themselves is not drawn twice.
QStringList issueRowAssignees(const QModelIndex &index)
{
    QStringList assignees = index.data(IssueAssigneesRole).toStringList();
    assignees.removeAll(index.data(IssueAuthorRole).toString());
    return assignees;
}

QString issueRowMetaPrefix(const QModelIndex &index)
{
    // "#123 · md-mehboob 4d" — the created age, not the updated one.
    QString prefix = QStringLiteral("#%1").arg(index.data(Qt::UserRole).toInt());
    const QString author = index.data(IssueAuthorRole).toString();
    const QString age = index.data(IssueCreatedRole).toString();
    if (!author.isEmpty())
        prefix += QString::fromUtf8(" \xC2\xB7 ") + author;
    if (!age.isEmpty())
        prefix += QLatin1Char(' ') + age;
    return prefix;
}

QString issueRowVoteText(const QModelIndex &index)
{
    return QString::number(index.data(IssueVotesRole).toInt());
}

// Square, centre-cropped preview of an attached image, so a row shows the
// screenshot someone filed rather than a generic document glyph. Decoding is
// done through QImageReader's scaled read and memoized per (path, size, DPR):
// the delegate repaints on every hover and must never re-read the file.
QPixmap issueAttachmentThumb(const QString &path, int side, qreal dpr)
{
    static QHash<QString, QPixmap> cache; // GUI thread only, like the icon cache
    const QString key = QStringLiteral("%1|%2|%3").arg(path).arg(side).arg(dpr);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();

    QPixmap thumb; // stays null when the file is gone or is not an image
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const QSize full = reader.size();
    if (full.isValid() && !full.isEmpty()) {
        const int target = qMax(1, qRound(side * dpr));
        reader.setScaledSize(
            full.scaled(target, target, Qt::KeepAspectRatioByExpanding));
        const QImage image = reader.read();
        if (!image.isNull()) {
            QPixmap square(target, target);
            square.fill(Qt::transparent);
            QPainter p(&square);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(QPoint((target - image.width()) / 2,
                               (target - image.height()) / 2),
                        image);
            p.end();
            square.setDevicePixelRatio(dpr);
            thumb = square;
        }
    }
    if (cache.size() > 256)
        cache.clear(); // bounded like tintedOcticonPixmap's; refills on demand
    cache.insert(key, thumb);
    return thumb;
}

IssueRowGeometry issueRowGeometry(const QRect &rowRect, const QFont &base,
                                  const QModelIndex &index)
{
    IssueRowGeometry g;
    const QRect r = rowRect.adjusted(1, 0, -1, -1);
    const QFont titleFont = issueRowTitleFont(base);
    const QFont metaFont = issueRowMetaFont(base);
    const QFontMetrics titleFm(titleFont);
    const QFontMetrics metaFm(metaFont);

    g.rail = QRect(r.left() + 2, r.top() + 7, 3, r.height() - 14);

    // A touch smaller than the row: the assignee badge overhangs the avatar's
    // bottom-right corner and the fireball orbits outside that badge, so the
    // whole cluster has to stay clear of the row's edge and of the gutter.
    const int avatarD = qMax(24, qMin(36, r.height() - 18));
    g.avatar = QRect(g.rail.right() + 8, r.top() + (r.height() - avatarD) / 2,
                     avatarD, avatarD);
    g.assignee =
        QRect(g.avatar.right() - kIssueAssigneeBadge + kIssueAssigneeOverhang,
              g.avatar.bottom() - kIssueAssigneeBadge + kIssueAssigneeOverhang,
              kIssueAssigneeBadge, kIssueAssigneeBadge);

    const int gutterX = g.avatar.right() + 14;
    g.status = QRect(gutterX, r.top() + 8, 16, 16);
    g.priority = QRect(gutterX - 5, r.top() + 27, 26, 15);

    const int textX = g.status.right() + 10;

    // Title line: the assignee moved onto the avatar, so the title now has the
    // row to itself.
    const int titleRight = r.right() - 10;
    const QString titleText = index.data(Qt::DisplayRole).toString();
    g.title = QRect(textX, r.top() + 6,
                    qBound(0, titleFm.horizontalAdvance(titleText),
                           qMax(40, titleRight - textX - 6)),
                    22);

    // Second line, left end first: the vote chip opens it and the attachment
    // preview follows, which leaves the row's right edge empty.
    const int metaTop = r.top() + 30;
    int metaLeft = textX;
    const int voteW =
        kIssueVoteGlyph + 4 +
        metaFm.horizontalAdvance(issueRowVoteText(index)) + 6;
    g.votes = QRect(metaLeft, metaTop, voteW, 18);
    metaLeft = g.votes.right() + 3;
    if (index.data(IssueFilesRole).toInt() > 0) {
        g.files = QRect(metaLeft, r.top() + 29, kIssueThumbSide, kIssueThumbSide);
        metaLeft = g.files.right() + 7;
    }

    g.meta = QRect(metaLeft, metaTop, qMax(20, r.right() - 10 - metaLeft), 18);
    g.milestoneName = index.data(IssueMilestoneRole).toString();
    int x = g.meta.left() + metaFm.horizontalAdvance(issueRowMetaPrefix(index));
    if (!g.milestoneName.isEmpty()) {
        const int sepW = metaFm.horizontalAdvance(QString::fromUtf8(" \xC2\xB7 "));
        const int nameW = metaFm.horizontalAdvance(g.milestoneName);
        g.milestone = QRect(x + sepW, g.meta.top(),
                            kIssueMilestoneRing + 5 + nameW, g.meta.height());
        if (g.milestone.right() <= g.meta.right())
            x = g.milestone.right();
        else
            g.milestone = QRect();
    }

    // Expanded label pills, spelled out, trailing the meta text.
    const QStringList labels = index.data(IssueLabelsRole).toStringList();
    int pillX = x + 8;
    for (const QString &label : labels) {
        const QRect pill(pillX, metaTop + 1,
                         metaFm.horizontalAdvance(label) + 2 * kIssueLabelPillPad,
                         kIssueLabelPillHeight);
        if (pill.right() > g.meta.right())
            break;
        g.labels.append({label, pill});
        pillX = pill.right() + 5;
    }
    if (!g.labels.isEmpty())
        x = g.labels.constLast().second.right();
    g.agent = QRect(x, g.meta.top(), qMax(0, g.meta.right() - x),
                    g.meta.height());
    return g;
}

class IssueSummaryDelegate final : public QStyledItemDelegate
{
public:
    explicit IssueSummaryDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        return QSize(420, 58);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const bool dark = currentThemeIsDark();
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        const QColor bg = selected ? nextSelectedFill(QColor(dark ? "#183825" : "#dafbe1"))
                                  : hovered ? Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa"))
                                            : Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff"));
        const QColor text = Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328"));
        const QColor muted = Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        const QRect r = option.rect.adjusted(1, 0, -1, -1);
        p->fillRect(r, bg);
        p->setPen(Theme::remapColor(QColor(dark ? "#21262d" : "#d8dee4")));
        p->drawLine(r.bottomLeft(), r.bottomRight());

        const IssueRowGeometry g = issueRowGeometry(option.rect, option.font, index);
        const QFont titleFont = issueRowTitleFont(option.font);
        const QFont metaFont = issueRowMetaFont(option.font);
        const QFontMetrics metaFm(metaFont);

        // Progress rail, flush against the list's left edge.
        const int progress = qBound(0, index.data(IssueProgressRole).toInt(), 100);
        p->setPen(QPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")), 3,
                       Qt::SolidLine, Qt::RoundCap));
        p->drawLine(g.rail.center().x(), g.rail.top(), g.rail.center().x(),
                    g.rail.bottom());
        const int progressTop =
            g.rail.bottom() - qRound(g.rail.height() * progress / 100.0);
        p->setPen(QPen(Theme::nextOr(Theme::Role::AccentFg, QColor("#2f81f7")), 3,
                       Qt::SolidLine, Qt::RoundCap));
        p->drawLine(g.rail.center().x(), progressTop, g.rail.center().x(),
                    g.rail.bottom());

        // Reporter avatar: leftmost, and as tall as the row.
        const QString author = index.data(IssueAuthorRole).toString();
        const QPixmap authorFace =
            index.data(IssueAuthorAvatarRole).value<QPixmap>();
        if (!authorFace.isNull()) {
            p->setRenderHint(QPainter::SmoothPixmapTransform);
            p->drawPixmap(g.avatar, authorFace);
        } else if (!author.isEmpty()) {
            p->setPen(QPen(bg, 2));
            p->setBrush(stableAvatarColor(author));
            p->drawEllipse(g.avatar);
            QFont af = metaFont;
            af.setPixelSize(qMax(9, g.avatar.height() * 2 / 5));
            af.setBold(true);
            p->setFont(af);
            p->setPen(Qt::white);
            p->drawText(g.avatar, Qt::AlignCenter, issueInitials(author));
        }

        // Whoever the issue is assigned to — usually the agent that picked it
        // up — rides the avatar's bottom-right corner as a small second circle,
        // and the fireball orbits that badge for as long as the agent works.
        const QStringList assignees = issueRowAssignees(index);
        const QPixmap assigneeFace =
            index.data(IssueAssigneeAvatarRole).value<QPixmap>();
        if (!assignees.isEmpty() && !assigneeFace.isNull()) {
            // Ring in the row colour so the badge separates from the avatar.
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawEllipse(g.assignee.adjusted(-1, -1, 1, 1));
            p->setRenderHint(QPainter::SmoothPixmapTransform);
            p->drawPixmap(g.assignee, assigneeFace);
        } else if (!assignees.isEmpty()) {
            const QString who = assignees.constFirst();
            p->setPen(QPen(bg, 2));
            p->setBrush(stableAvatarColor(who));
            p->drawEllipse(g.assignee);
            QFont af = metaFont;
            af.setPixelSize(qMax(8, g.assignee.height() / 2));
            af.setBold(true);
            p->setFont(af);
            p->setPen(Qt::white);
            p->drawText(g.assignee, Qt::AlignCenter, issueInitials(who));
        }
        if (index.data(IssueAgentActiveRole).toBool()) {
            p->drawPixmap(
                QPoint(g.assignee.center().x() - kIssueAssigneeOrbit / 2,
                       g.assignee.center().y() - kIssueAssigneeOrbit / 2),
                agentRunningFireballPixmap(
                    kIssueAssigneeOrbit,
                    index.data(IssueAgentAngleRole).toReal()));
        }

        const QString status = index.data(IssueStatusRole).toString();
        const QColor statusColor =
            status == QLatin1String("closed")
                ? Theme::nextOr(Theme::Role::AccentFg, QColor("#8250df"))
                : QColor(status == QLatin1String("deleted") ? "#8b949e"
                                                            : "#1a7f37");
        p->drawPixmap(g.status.topLeft(),
                      tintedOcticonPixmap(status == QLatin1String("closed")
                                              ? QStringLiteral("check-circle")
                                              : QStringLiteral("issue-opened"),
                                          statusColor, 16));

        // Priority sits under the status glyph rather than beside the title.
        const int priority = index.data(IssuePriorityRole).toInt();
        if (priority > 0) {
            QFont pf = metaFont;
            pf.setBold(true);
            p->setFont(pf);
            p->setPen(issuePriorityColor(priority));
            p->drawText(g.priority, Qt::AlignCenter, QString::number(priority));
        }

        // Attachment preview: the real image, scaled into the row, so a bug
        // report with a screenshot is recognisable from the list.
        if (!g.files.isNull()) {
            const QPixmap thumb = issueAttachmentThumb(
                index.data(IssueAttachmentRole).toString(), kIssueThumbSide,
                option.widget ? option.widget->devicePixelRatioF() : 1.0);
            QPainterPath clip;
            clip.addRoundedRect(g.files, 4, 4);
            if (thumb.isNull()) {
                p->setPen(Theme::nextOr(Theme::Role::Border,
                                        QColor(dark ? "#58a6ff" : "#0969da")));
                p->setBrush(Theme::nextOr(Theme::Role::SurfaceRaised,
                                          QColor(dark ? "#0c2d4a" : "#ddf4ff")));
                p->drawRoundedRect(g.files, 4, 4);
                p->drawPixmap(g.files.topLeft() + QPoint(4, 4),
                              tintedOcticonPixmap(
                                  "file",
                                  Theme::nextOr(Theme::Role::FgMuted,
                                                QColor("#2f81f7")),
                                  13));
            } else {
                p->save();
                p->setClipPath(clip);
                p->drawPixmap(g.files, thumb);
                p->restore();
                p->setPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")));
                p->setBrush(Qt::NoBrush);
                p->drawRoundedRect(g.files, 4, 4);
            }
        }

        // Thumbs up plus its count, always shown: the row is where an issue
        // gets upvoted, so the affordance cannot depend on already having votes.
        {
            const int votes = index.data(IssueVotesRole).toInt();
            const QColor voteColor =
                votes > 0 ? Theme::nextOr(Theme::Role::AccentFg,
                                          QColor(dark ? "#3fb950" : "#1a7f37"))
                          : muted;
            p->drawPixmap(g.votes.left(),
                          g.votes.center().y() - kIssueVoteGlyph / 2,
                          tintedOcticonPixmap(QStringLiteral("thumbsup"),
                                              voteColor, kIssueVoteGlyph));
            p->setFont(metaFont);
            p->setPen(voteColor);
            p->drawText(g.votes.adjusted(kIssueVoteGlyph + 4, 0, 0, 0),
                        Qt::AlignVCenter | Qt::AlignLeft, issueRowVoteText(index));
        }

        p->setFont(titleFont);
        p->setPen(text);
        p->drawText(g.title, Qt::AlignVCenter,
                    QFontMetrics(titleFont).elidedText(
                        index.data(Qt::DisplayRole).toString(), Qt::ElideRight,
                        g.title.width()));

        // Meta line: "#123 · author 4d", then the milestone with its own ring.
        p->setFont(metaFont);
        p->setPen(muted);
        const QString prefix = issueRowMetaPrefix(index);
        p->drawText(g.meta, Qt::AlignVCenter,
                    metaFm.elidedText(prefix, Qt::ElideRight, g.meta.width()));
        if (!g.milestone.isNull()) {
            p->drawText(QRect(g.meta.left() + metaFm.horizontalAdvance(prefix),
                              g.meta.top(),
                              metaFm.horizontalAdvance(
                                  QString::fromUtf8(" \xC2\xB7 ")),
                              g.meta.height()),
                        Qt::AlignVCenter, QString::fromUtf8(" \xC2\xB7 "));
            const QRectF ring(g.milestone.left(),
                              g.milestone.center().y() - kIssueMilestoneRing / 2.0,
                              kIssueMilestoneRing, kIssueMilestoneRing);
            p->setPen(QPen(Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de")), 2));
            p->setBrush(Qt::NoBrush);
            p->drawEllipse(ring);
            p->setPen(QPen(Theme::nextOr(Theme::Role::AccentFg, QColor("#2f81f7")), 2));
            p->drawArc(ring, 90 * 16, -qMax(12, progress * 360 / 100) * 16);
            p->setPen(muted);
            const QRect nameRect(g.milestone.left() + kIssueMilestoneRing + 5,
                                 g.meta.top(),
                                 g.milestone.width() - kIssueMilestoneRing - 5,
                                 g.meta.height());
            p->drawText(nameRect, Qt::AlignVCenter,
                        metaFm.elidedText(g.milestoneName, Qt::ElideRight,
                                          nameRect.width()));
        }

        // Expanded label pills, spelled out rather than reduced to initials.
        // They ride the meta line so the title above keeps the full row width.
        const QStringList labels = index.data(IssueLabelsRole).toStringList();
        const QStringList colors = index.data(IssueLabelColorsRole).toStringList();
        for (int i = 0; i < g.labels.size(); ++i) {
            const QColor c(i < colors.size() ? colors.at(i) : "#64748B");
            p->setPen(Qt::NoPen);
            p->setBrush(c);
            p->drawRoundedRect(g.labels.at(i).second, kIssueLabelPillHeight / 2.0,
                               kIssueLabelPillHeight / 2.0);
            p->setPen(QColor(pillTextColor(c.name())));
            p->drawText(g.labels.at(i).second, Qt::AlignCenter,
                        g.labels.at(i).first);
        }
        p->setPen(muted);
        const QString agent = index.data(IssueAgentRole).toString();
        const int overflow = labels.size() - g.labels.size();
        // What is left of the meta line carries the agent, or the "+N" marker
        // for the labels that did not fit.
        if (overflow > 0 && g.agent.width() >= 24) {
            p->drawText(g.agent.adjusted(6, 0, 0, 0), Qt::AlignVCenter,
                        QStringLiteral("+%1").arg(overflow));
        } else if (!agent.isEmpty() && g.agent.width() > 0) {
            p->drawText(g.agent, Qt::AlignVCenter,
                        metaFm.elidedText(QString::fromUtf8(" \xC2\xB7 ") + agent,
                                          Qt::ElideRight, g.agent.width()));
        }
        p->restore();
    }
};

const char kIssueChecklistDragMime[] =
    "application/x-forkmesh-issue-checklist-row";

// A checkbox with enough physical presence to feel like a control rather than
// a tiny Markdown glyph. Both directions get their own little constellation:
// checking bursts colour outward, while unchecking winds it back into the box.
class IssueTaskCheckButton final : public QAbstractButton
{
public:
    explicit IssueTaskCheckButton(const QString &label, bool checked,
                                  QWidget *parent = nullptr)
        : QAbstractButton(parent), m_label(label), m_checking(checked)
    {
        setObjectName(QStringLiteral("issueChecklistToggle"));
        setCheckable(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(42, 38);
        setFocusPolicy(Qt::StrongFocus);
        {
            QSignalBlocker blocker(this);
            setChecked(checked);
        }
        updateAccessibleText();

        m_animation = new QVariantAnimation(this);
        m_animation->setStartValue(0.0);
        m_animation->setEndValue(1.0);
        m_animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_animation, &QVariantAnimation::valueChanged, this,
                [this](const QVariant &value) {
                    m_progress = value.toReal();
                    update();
                });
        connect(this, &QAbstractButton::toggled, this, [this](bool checkedNow) {
            m_checking = checkedNow;
            m_progress = 0.0;
            m_animation->stop();
            m_animation->setDuration(checkedNow ? 720 : 560);
            m_animation->start();
            updateAccessibleText();
        });
    }

    QSize sizeHint() const override { return QSize(42, 38); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const bool dark = currentThemeIsDark();
        const QColor quietFill(dark ? QStringLiteral("#161b22")
                                    : QStringLiteral("#ffffff"));
        const QColor quietBorder(dark ? QStringLiteral("#6e7681")
                                      : QStringLiteral("#8c959f"));
        const QColor activeFill = Theme::nextOr(
            Theme::Role::Accent, QColor(dark ? QStringLiteral("#2ea043")
                                             : QStringLiteral("#1a7f37")));
        const qreal fillAmount = m_checking ? m_progress : 1.0 - m_progress;
        auto mixed = [](const QColor &from, const QColor &to, qreal amount) {
            amount = qBound<qreal>(0.0, amount, 1.0);
            return QColor::fromRgbF(
                from.redF() + (to.redF() - from.redF()) * amount,
                from.greenF() + (to.greenF() - from.greenF()) * amount,
                from.blueF() + (to.blueF() - from.blueF()) * amount,
                from.alphaF() + (to.alphaF() - from.alphaF()) * amount);
        };

        const QPointF center(width() / 2.0, height() / 2.0);
        const qreal pulse = m_animation->state() == QAbstractAnimation::Running
                                ? std::sin(m_progress * M_PI) *
                                      (m_checking ? 0.18 : 0.11)
                                : 0.0;
        painter.save();
        painter.translate(center);
        painter.scale(1.0 + pulse, 1.0 + pulse);
        const QRectF box(-13.0, -13.0, 26.0, 26.0);
        painter.setPen(QPen(mixed(quietBorder, activeFill, fillAmount), 2.0));
        painter.setBrush(mixed(quietFill, activeFill, fillAmount));
        painter.drawRoundedRect(box, 7.0, 7.0);

        const qreal checkAmount = m_checking ? m_progress : 1.0 - m_progress;
        if (checkAmount > 0.01) {
            QPainterPath check;
            check.moveTo(-7.0, 0.0);
            check.lineTo(-2.0, 5.0);
            check.lineTo(8.0, -6.0);
            QColor ink = Theme::nextOr(Theme::Role::OnAccent, QColor(Qt::white));
            ink.setAlphaF(qBound<qreal>(0.0, checkAmount * 1.7, 1.0));
            painter.setPen(QPen(ink, 3.0, Qt::SolidLine, Qt::RoundCap,
                                Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(check);
        }
        painter.restore();

        if (m_animation->state() == QAbstractAnimation::Running) {
            static const QColor sparkColors[] = {
                QColor("#f9c74f"), QColor("#58a6ff"), QColor("#bc8cff"),
                QColor("#3fb950"), QColor("#ff7b72"), QColor("#39c5cf")};
            const qreal shimmer = std::sin(m_progress * M_PI);
            const qreal radius = m_checking ? 7.0 + 8.0 * m_progress
                                            : 15.0 - 8.0 * m_progress;
            for (int i = 0; i < 10; ++i) {
                const qreal angle = i * (2.0 * M_PI / 10.0) +
                                    (m_checking ? 0.45 : -0.55) * m_progress;
                const QPointF point = center +
                    QPointF(std::cos(angle) * radius, std::sin(angle) * radius);
                QColor color = Theme::nextOr(Theme::Role::AccentFg, sparkColors[i % 6]);
                color.setAlphaF(qBound<qreal>(0.0, shimmer * 0.95, 1.0));
                painter.save();
                painter.translate(point);
                painter.rotate(angle * 180.0 / M_PI + 45.0);
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                const qreal side = 1.6 + shimmer * (i % 3 == 0 ? 2.4 : 1.5);
                if (i % 2 == 0)
                    painter.drawRoundedRect(
                        QRectF(-side, -side, side * 2, side * 2), 1.2, 1.2);
                else
                    painter.drawEllipse(QPointF(), side, side);
                painter.restore();
            }
        }

        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(Theme::nextOr(Theme::Role::AccentFg, QColor("#58a6ff")),
                                1.5, Qt::DashLine));
            painter.drawRoundedRect(
                QRectF(3.5, 1.5, width() - 7.0, height() - 3.0), 9.0, 9.0);
        }
    }

private:
    void updateAccessibleText()
    {
        const QString action = isChecked() ? QStringLiteral("Mark incomplete")
                                           : QStringLiteral("Mark complete");
        setAccessibleName(QStringLiteral("%1: %2").arg(action, m_label));
        setToolTip(action);
    }

    QString m_label;
    QVariantAnimation *m_animation = nullptr;
    qreal m_progress = 1.0;
    bool m_checking = false;
};

class IssueTaskDragButton final : public QPushButton
{
public:
    IssueTaskDragButton(int line, int indent, const QString &label,
                        QWidget *parent = nullptr)
        : QPushButton(parent), m_line(line), m_indent(indent)
    {
        setObjectName(QStringLiteral("issueChecklistDrag"));
        setText(QString::fromUtf8("\xE2\xA0\xBF")); // six-dot grip
        setFixedSize(28, 32);
        setCursor(Qt::OpenHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(QStringLiteral("Drag to reorder · Arrow keys also work"));
        setAccessibleName(
            QStringLiteral("Reorder %1. Use Arrow Up or Arrow Down.").arg(label));
    }

    std::function<void(int)> onKeyboardMove;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressPosition = event->position().toPoint();
            setCursor(Qt::ClosedHandCursor);
        }
        QPushButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        setCursor(Qt::OpenHandCursor);
        QPushButton::mouseReleaseEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton) ||
            (event->position().toPoint() - m_pressPosition).manhattanLength() <
                QApplication::startDragDistance()) {
            QPushButton::mouseMoveEvent(event);
            return;
        }
        auto *mime = new QMimeData;
        mime->setData(kIssueChecklistDragMime,
                      QStringLiteral("%1:%2").arg(m_line).arg(m_indent).toUtf8());
        auto *drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(grab());
        drag->setHotSpot(event->position().toPoint());
        drag->exec(Qt::MoveAction);
        setCursor(Qt::OpenHandCursor);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if ((event->key() == Qt::Key_Up || event->key() == Qt::Key_Down) &&
            onKeyboardMove) {
            onKeyboardMove(event->key() == Qt::Key_Up ? -1 : 1);
            event->accept();
            return;
        }
        QPushButton::keyPressEvent(event);
    }

private:
    int m_line = -1;
    int m_indent = 0;
    QPoint m_pressPosition;
};

class IssueChecklistRow final : public QWidget
{
public:
    explicit IssueChecklistRow(const IssueChecklistTask &task, bool editable,
                               const QString &labelMarkdown,
                               QWidget *parent = nullptr)
        : QWidget(parent), m_line(task.line), m_indent(task.indent),
          m_editable(editable)
    {
        setObjectName(QStringLiteral("issueChecklistRow"));
        setMouseTracking(true);
        setAcceptDrops(editable);
        setMinimumHeight(42);

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(qMin(task.indent * 10, 60), 2, 4, 2);
        layout->setSpacing(5);
        if (editable) {
            m_drag = new IssueTaskDragButton(task.line, task.indent, task.label,
                                             this);
            layout->addWidget(m_drag, 0, Qt::AlignVCenter);
        }
        m_check = new IssueTaskCheckButton(task.label, task.checked, this);
        m_check->setEnabled(editable);
        layout->addWidget(m_check, 0, Qt::AlignVCenter);

        m_label = new QLabel(this);
        m_label->setObjectName(QStringLiteral("issueChecklistLabel"));
        m_label->setTextFormat(Qt::MarkdownText);
        m_label->setText(labelMarkdown);
        m_label->setWordWrap(true);
        m_label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        m_label->setOpenExternalLinks(false);
        m_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setCompleted(task.checked);
        layout->addWidget(m_label, 1, Qt::AlignVCenter);

        if (task.subIssueNumber > 0) {
            m_attached = new QPushButton(
                QStringLiteral("Sub-issue #%1").arg(task.subIssueNumber), this);
            m_attached->setObjectName(QStringLiteral("issueChecklistAttached"));
            m_attached->setProperty("issueNumber", task.subIssueNumber);
            m_attached->setCursor(Qt::PointingHandCursor);
            m_attached->setToolTip(QStringLiteral("Open attached sub-issue #%1")
                                       .arg(task.subIssueNumber));
            setOcticon(m_attached, QStringLiteral("git-branch"), 13);
            layout->addWidget(m_attached, 0, Qt::AlignVCenter);
        } else if (editable) {
            m_convert = new QPushButton(QStringLiteral("Sub-issue"), this);
            m_convert->setObjectName(QStringLiteral("issueChecklistConvert"));
            m_convert->setCursor(Qt::PointingHandCursor);
            m_convert->setToolTip(
                QStringLiteral("Turn this task into an attached issue"));
            setOcticon(m_convert, QStringLiteral("issue-opened"), 13);
            QSizePolicy retained(QSizePolicy::Fixed, QSizePolicy::Fixed);
            retained.setRetainSizeWhenHidden(true);
            m_convert->setSizePolicy(retained);
            m_convert->hide();
            layout->addWidget(m_convert, 0, Qt::AlignVCenter);
        }
    }

    IssueTaskCheckButton *checkButton() const { return m_check; }
    IssueTaskDragButton *dragButton() const { return m_drag; }
    QLabel *label() const { return m_label; }
    QPushButton *convertButton() const { return m_convert; }
    QPushButton *attachedButton() const { return m_attached; }

    void setCompleted(bool completed)
    {
        QFont labelFont = m_label->font();
        labelFont.setStrikeOut(completed);
        m_label->setFont(labelFont);
        m_label->setProperty("completed", completed);
        m_label->style()->unpolish(m_label);
        m_label->style()->polish(m_label);
    }

    std::function<void(int, int, bool)> onDropped;

protected:
    void enterEvent(QEnterEvent *event) override
    {
        m_hovered = true;
        if (m_convert)
            m_convert->show();
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override
    {
        m_hovered = false;
        if (m_convert)
            m_convert->hide();
        update();
        QWidget::leaveEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (m_hovered) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(Theme::nextOr(
                Theme::Role::SurfaceRaised,
                currentThemeIsDark() ? QColor(255, 255, 255, 13)
                                     : QColor(9, 105, 218, 13)));
            painter.drawRoundedRect(rect().adjusted(0, 1, 0, -1), 8, 8);
        }
        if (m_dropActive) {
            painter.setPen(QPen(Theme::nextOr(Theme::Role::AccentFg, QColor("#2f81f7")),
                                3.0, Qt::SolidLine, Qt::RoundCap));
            const qreal y = m_dropAfter ? height() - 1.5 : 1.5;
            painter.drawLine(QPointF(8.0, y), QPointF(width() - 8.0, y));
        }
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (acceptsDrag(event->mimeData())) {
            m_dropActive = true;
            m_dropAfter = event->position().y() >= height() / 2.0;
            event->acceptProposedAction();
            update();
            return;
        }
        QWidget::dragEnterEvent(event);
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        if (acceptsDrag(event->mimeData())) {
            m_dropAfter = event->position().y() >= height() / 2.0;
            event->acceptProposedAction();
            update();
            return;
        }
        QWidget::dragMoveEvent(event);
    }

    void dragLeaveEvent(QDragLeaveEvent *event) override
    {
        m_dropActive = false;
        update();
        QWidget::dragLeaveEvent(event);
    }

    void dropEvent(QDropEvent *event) override
    {
        int fromLine = -1;
        int fromIndent = -1;
        if (!dragCoordinates(event->mimeData(), &fromLine, &fromIndent) ||
            fromLine == m_line || fromIndent != m_indent || !onDropped) {
            event->ignore();
        } else {
            const bool placeAfter = event->position().y() >= height() / 2.0;
            onDropped(fromLine, m_line, placeAfter);
            event->acceptProposedAction();
        }
        m_dropActive = false;
        update();
    }

private:
    static bool dragCoordinates(const QMimeData *mime, int *line, int *indent)
    {
        if (!mime || !mime->hasFormat(kIssueChecklistDragMime))
            return false;
        const QList<QByteArray> parts =
            mime->data(kIssueChecklistDragMime).split(':');
        if (parts.size() != 2)
            return false;
        bool lineOk = false;
        bool indentOk = false;
        const int parsedLine = parts.at(0).toInt(&lineOk);
        const int parsedIndent = parts.at(1).toInt(&indentOk);
        if (!lineOk || !indentOk)
            return false;
        if (line)
            *line = parsedLine;
        if (indent)
            *indent = parsedIndent;
        return true;
    }

    bool acceptsDrag(const QMimeData *mime) const
    {
        int fromLine = -1;
        int fromIndent = -1;
        return m_editable &&
               dragCoordinates(mime, &fromLine, &fromIndent) &&
               fromLine != m_line && fromIndent == m_indent;
    }

    int m_line = -1;
    int m_indent = 0;
    bool m_editable = false;
    bool m_hovered = false;
    bool m_dropActive = false;
    bool m_dropAfter = false;
    IssueTaskCheckButton *m_check = nullptr;
    IssueTaskDragButton *m_drag = nullptr;
    QLabel *m_label = nullptr;
    QPushButton *m_convert = nullptr;
    QPushButton *m_attached = nullptr;
};

// The status drop-down is off-screen now, so a query that names a status has to
// beat its default ("Open") — otherwise `is:closed` would filter against an
// already open-only list and always come back empty.
bool issueQuerySetsStatus(const QString &query)
{
    for (const QString &raw : QProcess::splitCommand(query)) {
        const QString key = raw.section(QLatin1Char(':'), 0, 0).toLower();
        if (raw.contains(QLatin1Char(':')) &&
            (key == QLatin1String("is") || key == QLatin1String("status")))
            return true;
    }
    return false;
}

bool issueMatchesQuery(const Issue &issue, const QString &query)
{
    QStringList freeText;
    for (const QString &raw : QProcess::splitCommand(query)) {
        const int colon = raw.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            freeText << raw;
            continue;
        }
        const QString key = raw.left(colon).toLower();
        const QString value = raw.mid(colon + 1).trimmed();
        if (key == QLatin1String("is") || key == QLatin1String("status")) {
            if (value.compare(issue.status, Qt::CaseInsensitive) != 0)
                return false;
        } else if (key == QLatin1String("label")) {
            if (!issue.labels.contains(value, Qt::CaseInsensitive))
                return false;
        } else if (key == QLatin1String("milestone")) {
            if (issue.milestone.compare(value, Qt::CaseInsensitive) != 0)
                return false;
        } else if (key == QLatin1String("author")) {
            if (!issue.authorName.contains(value, Qt::CaseInsensitive) &&
                !issue.author.contains(value, Qt::CaseInsensitive))
                return false;
        } else if (key == QLatin1String("assignee")) {
            if (std::none_of(issue.assignees.cbegin(), issue.assignees.cend(),
                             [&](const QString &a) {
                                 return a.contains(value, Qt::CaseInsensitive);
                             }))
                return false;
        } else if (key == QLatin1String("priority")) {
            if (QString::number(issue.priority) != value)
                return false;
        } else if (key == QLatin1String("no")) {
            if ((value == QLatin1String("label") && !issue.labels.isEmpty()) ||
                (value == QLatin1String("milestone") && !issue.milestone.isEmpty()) ||
                (value == QLatin1String("assignee") && !issue.assignees.isEmpty()))
                return false;
        } else {
            freeText << raw;
        }
    }
    const QString hay = QStringLiteral("#%1 %2 %3 %4 %5 %6 %7")
                            .arg(issue.number)
                            .arg(issue.title)
                            .arg(issue.priority)
                            .arg(issue.labels.join(QLatin1Char(' ')), issue.milestone)
                            .arg(issue.authorName, issue.assignees.join(QLatin1Char(' ')));
    for (const QString &term : std::as_const(freeText))
        if (!hay.contains(term, Qt::CaseInsensitive))
            return false;
    return true;
}

// The small widgets the issue sidebar is assembled from.
QLabel *issueSidebarValueLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName("statusLine");
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    return label;
}

QPushButton *issueSidebarGearButton()
{
    auto *button = new QPushButton;
    button->setObjectName("issueIconButton");
    button->setFixedSize(28, 28);
    button->setEnabled(false);
    setOcticon(button, "gear", 15);
    return button;
}

QPushButton *issueSidebarActionButton(const QString &text, const QString &icon)
{
    auto *button = new QPushButton(text);
    button->setObjectName("issueSidebarAction");
    button->setCursor(Qt::PointingHandCursor);
    if (!icon.isEmpty())
        setOcticon(button, icon, 15);
    return button;
}

QPushButton *issueSidebarEditorButton(QWidget *meta, const QString &text,
                                      const char *objectName)
{
    auto *button = new QPushButton(text, meta);
    button->setObjectName(objectName);
    button->setProperty("buttonSize", "sm");
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QWidget *issueSidebarInlineButtonRow(QWidget *meta, QPushButton *save,
                                     QPushButton *cancel)
{
    auto *rowWidget = new QWidget(meta);
    auto *row = new QHBoxLayout(rowWidget);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    row->addStretch();
    row->addWidget(cancel);
    row->addWidget(save);
    return rowWidget;
}
} // namespace


QWidget *MainWindow::buildIssuesSection()
{
    auto *page = new QWidget;

    auto *listPane = new QWidget;
    listPane->setMinimumWidth(260);
    listPane->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);

    QPushButton *issueBurnupButton = nullptr;
    QHBoxLayout *headingRow =
        buildIssueListToolbar(page, listPane, issueBurnupButton);
    buildIssueListViews();

    auto *listLayout = new QVBoxLayout(listPane);
    // No left gutter: the action row and every summary row start flush against
    // the pane's edge, so the toolbar and the rows share one left margin line.
    // The toolbar rides high under the repo tab bar and is closed off by a rule
    // rather than by a band of dead space.
    listLayout->setContentsMargins(0, 4, 12, 18);
    listLayout->setSpacing(4);
    auto makeOverflowToolbar = [this](QHBoxLayout *row,
                                      const QString &objectName) {
        auto *host = new QWidget;
        host->setLayout(row);
        host->adjustSize();
        auto *scroll = new QScrollArea;
        scroll->setObjectName(objectName);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidget(host);
        scroll->setWidgetResizable(false);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setMinimumWidth(0);
        scroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        // Only pay for the scrollbar when the row actually overflows; reserving
        // it unconditionally left a band of dead space under the icons.
        const int rowHeight = host->sizeHint().height();
        auto fitHeight = [scroll, rowHeight] {
            QScrollBar *bar = scroll->horizontalScrollBar();
            scroll->setFixedHeight(rowHeight +
                                   (bar->maximum() > 0 ? bar->sizeHint().height()
                                                       : 0));
        };
        connect(scroll->horizontalScrollBar(), &QScrollBar::rangeChanged, scroll,
                [fitHeight](int, int) { fitHeight(); });
        fitHeight();
        return scroll;
    };
    listLayout->addWidget(
        makeOverflowToolbar(headingRow, QStringLiteral("issueHeadingToolbar")));
    auto *headingDivider = new QWidget(listPane);
    headingDivider->setObjectName("issueDivider");
    headingDivider->setFixedHeight(1);
    listLayout->addWidget(headingDivider);
    listLayout->addWidget(m_issuesRepoCombo);
    listLayout->addWidget(m_issueListStack, 1);

    QPushButton *closeDetailButton = nullptr;
    QWidget *center = buildIssueDetailCenter(closeDetailButton);
    QWidget *meta = buildIssueSidebar();
    buildIssueDetailPane(center, meta);

    closeDetailButton->hide();
    auto *issuesSplit = new QSplitter(Qt::Horizontal, page);
    issuesSplit->setObjectName("issuesListDetailSplit");
    issuesSplit->setChildrenCollapsible(false);
    issuesSplit->addWidget(listPane);
    issuesSplit->addWidget(m_issueDetail);
    issuesSplit->setStretchFactor(0, 1);
    issuesSplit->setStretchFactor(1, 2);
    issuesSplit->setSizes({460, 900});
    m_issueDetail->show();
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(issuesSplit, 1);
    connectIssuesSectionSignals(m_issueTabGroup, issueBurnupButton);
    return page;
}

// The list pane's action row: view tabs, New issue, Analyze, Refresh, Burn-up,
// Reprioritize and the voting-credits tile, plus the off-screen filter state.
QHBoxLayout *MainWindow::buildIssueListToolbar(QWidget *page, QWidget *listPane,
                                              QPushButton *&issueBurnupButton)
{
    auto *issueTabGroup = new QButtonGroup(page);
    m_issueTabGroup = issueTabGroup;
    issueTabGroup->setExclusive(true);
    auto *issuesTab = new VerticalIconButton("Issues", VerticalIconButton::Tab);
    issuesTab->setObjectName("issueViewTab");
    issuesTab->setCheckable(true);
    issuesTab->setChecked(true);
    issuesTab->setCursor(Qt::PointingHandCursor);
    issuesTab->setToolTip("Issues");
    setOcticon(issuesTab, "issue-opened", 16);
    auto *milestonesTab =
        new VerticalIconButton("Milestones", VerticalIconButton::Tab);
    milestonesTab->setObjectName("issueViewTab");
    milestonesTab->setCheckable(true);
    milestonesTab->setCursor(Qt::PointingHandCursor);
    auto *labelsTab = new VerticalIconButton("Labels", VerticalIconButton::Tab);
    labelsTab->setObjectName("issueViewTab");
    labelsTab->setCheckable(true);
    labelsTab->setCursor(Qt::PointingHandCursor);
    auto *boardTab = new VerticalIconButton("Board", VerticalIconButton::Tab);
    boardTab->setObjectName("issueViewTab");
    boardTab->setCheckable(true);
    boardTab->setCursor(Qt::PointingHandCursor);
    boardTab->setToolTip("Kanban board: drag issues between status columns");
    setOcticon(milestonesTab, "graph", 16);
    setOcticon(labelsTab, "tag", 16);
    setOcticon(boardTab, "workflow", 16);
    issueTabGroup->addButton(issuesTab, 0);
    issueTabGroup->addButton(milestonesTab, 1);
    issueTabGroup->addButton(labelsTab, 2);
    issueTabGroup->addButton(boardTab, 3);
    m_issueListNewButton =
        new VerticalIconButton("New issue", VerticalIconButton::Action);
    m_issueListNewButton->setObjectName("issueHeaderAction");
    m_issueListNewButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_issueListNewButton, "plus", 16);

    auto *headingRow = new QHBoxLayout;
    headingRow->setContentsMargins(0, 0, 0, 0);
    headingRow->addWidget(m_issueListNewButton);
    if (m_looperToggle)
        headingRow->addWidget(m_looperToggle);
    headingRow->addStretch();
    headingRow->addWidget(issuesTab);
    headingRow->addWidget(boardTab);
    headingRow->addWidget(milestonesTab);
    headingRow->addWidget(labelsTab);

    m_issuesRepoCombo = new QComboBox;
    m_issuesRepoCombo->setToolTip("Repository whose issues you are viewing");
    m_issuesRepoCombo->hide();

    m_issueSearch = new QLineEdit(listPane);
    m_issueSearch->setObjectName("issueListSearchState");
    m_issueSearch->setPlaceholderText("Search issues\xE2\x80\xA6");
    m_issueSearch->setClearButtonEnabled(true);

    m_issueStatusFilter = new QComboBox;
    m_issueStatusFilter->setObjectName("issueControlSm");
    m_issueStatusFilter->addItems({"Open", "Closed", "All"});
    m_issueLabelFilter = new QComboBox;
    m_issueLabelFilter->setObjectName("issueControlSm");
    m_issueMilestoneFilter = new QComboBox;
    m_issueMilestoneFilter->setObjectName("issueControlSm");
    m_issueSearch->hide();

    m_issueNewButton = new QPushButton("New issue");
    m_issueNewButton->setObjectName("primaryButton");
    m_issueNewButton->setProperty("buttonSize", "sm");
    m_issueNewButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_issueNewButton, "plus", 16);
    m_issueSyncButton =
        new VerticalIconButton("Refresh", VerticalIconButton::Action);
    m_issueSyncButton->setObjectName("issueRefreshAction");
    m_issueSyncButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_issueSyncButton, "sync", 16);
    m_issueSyncButton->setToolTip(
        "Pull new issues and comments from the mirror mesh");
    issueBurnupButton =
        new VerticalIconButton("Burn-up", VerticalIconButton::Action);
    issueBurnupButton->setObjectName("issueBurnupAction");
    issueBurnupButton->setCursor(Qt::PointingHandCursor);
    issueBurnupButton->setToolTip(
        "Show open and closed issue totals over time");
    setOcticon(issueBurnupButton, "graph", 16);
    m_issueDetailToggle = new QPushButton(page);
    m_issueDetailToggle->hide();
    // Voting credits read as a glyph over its count, the same tile the rest of
    // the toolbar uses — the old chip asked for a "zap" icon the app never
    // shipped, so the count floated in the row with nothing above it.
    m_issueCreditsButton = new VerticalIconButton(
        QString::number(availableCredits()), VerticalIconButton::Bare);
    m_issueCreditsButton->setObjectName("issueCreditsTile");
    m_issueCreditsButton->setOcticonName(QStringLiteral("thumbsup"));
    m_issueCreditsButton->setAccentColor(
        Theme::nextOr(Theme::Role::FgSecondary, QColor("#bf8700")));
    m_issueCreditsButton->setFocusPolicy(Qt::NoFocus);
    const QString creditsTip =
        QString::fromUtf8("Voting credits \xE2\x80\x94 earn 1 per hour online");
    m_issueCreditsButton->setToolTip(creditsTip);
    connect(m_issueCreditsButton, &QPushButton::clicked, this, [this] {
        const int credits = availableCredits();
        setIssueInlineNotice(
            credits > 0
                ? QString::fromUtf8("%1 voting credit%2 available \xE2\x80\x94 you "
                                 "earn 1 per hour online.")
                      .arg(credits)
                      .arg(credits == 1 ? "" : "s")
                : QString::fromUtf8("No voting credits yet \xE2\x80\x94 you earn "
                                    "1 per hour online."));
    });
    auto *reprioritizeButton =
        new VerticalIconButton("Reprioritize", VerticalIconButton::Action);
    reprioritizeButton->setObjectName("issueReprioritizeAction");
    reprioritizeButton->setCursor(Qt::PointingHandCursor);
    reprioritizeButton->setToolTip(
        "Assign a priority + MVP/Phase 2 label to open issues with no priority, "
        "and estimate each issue's progress from whether the work has landed");
    setOcticon(reprioritizeButton, "sort-desc", 16);
    connect(reprioritizeButton, &QPushButton::clicked, this,
            &MainWindow::reprioritizeBacklog);


    m_issuePrioritizeAgentCombo = new QComboBox(page);
    m_issuePrioritizeAgentCombo->setObjectName("issueControlSm");
    m_issuePrioritizeAgentCombo->addItem(QStringLiteral("Codex"), kCodexProvider);
    m_issuePrioritizeAgentCombo->addItem(QStringLiteral("OpenAI API"),
                                         QStringLiteral("openai"));
    m_issuePrioritizeAgentCombo->addItem(QStringLiteral("Claude API"),
                                         QStringLiteral("claude-api"));
    m_issuePrioritizeAgentCombo->addItem(QStringLiteral("CC"),
                                         QStringLiteral("claude-code"));
    m_issuePrioritizeAgentCombo->addItem(QStringLiteral("Local Llama"),
                                         kLocalLlamaProvider);
    selectDefaultAgentProvider(m_issuePrioritizeAgentCombo);
    m_issuePrioritizeAgentCombo->setToolTip(
        "Agent that ranks/reviews the issues. Defaults to your default agent "
        "(Settings \xE2\x86\x92 Agents).");
    m_issuePrioritizeAgentCombo->hide();

    m_issueCompletenessButton =
        new VerticalIconButton("Analyze", VerticalIconButton::Action);
    m_issueCompletenessButton->setObjectName("issueAnalyzeAction");
    m_issueCompletenessButton->setCursor(Qt::PointingHandCursor);
    m_issueCompletenessButton->setToolTip(
        "Ask the picked agent to rate how complete each open issue is (clear "
        "problem, enough detail, acceptance criteria), then label each issue "
        "Complete/Partial/Incomplete and set its progress from the verdict.");
    setOcticon(m_issueCompletenessButton, "list-unordered", 16);
    connect(m_issueCompletenessButton, &QPushButton::clicked, this, [this] {
        if (!m_issueQuickAdd)
            return;
        recordQuickAddHistory(m_issueQuickAdd->toPlainText());
        m_issueQuickAdd->setPlainText(completenessPrompt());
        m_issueQuickAdd->moveCursor(QTextCursor::End);
        m_issueQuickAdd->setFocus();
        setRepoDetailNotice(
            "Completeness prompt drafted — review it, then send it to your agent.");
    });

    headingRow->insertWidget(0, m_issueCompletenessButton);
    headingRow->insertWidget(2, m_issueSyncButton);
    headingRow->insertWidget(3, issueBurnupButton);
    headingRow->insertWidget(4, reprioritizeButton);

    auto *bountyAllAmount = new QLineEdit(listPane);
    bountyAllAmount->setObjectName("issueControlSm");
    bountyAllAmount->setPlaceholderText("retired");
    bountyAllAmount->setMaximumWidth(70);
    bountyAllAmount->setEnabled(false);
    bountyAllAmount->setToolTip(
        "New bounty escrow is disabled; historical entries are migration-only.");
    auto *bountyAllButton = new QPushButton("Bounties retired", listPane);
    bountyAllButton->setObjectName("ghostButton");
    bountyAllButton->setProperty("buttonSize", "sm");
    bountyAllButton->setEnabled(false);
    bountyAllButton->setToolTip(
        "ForkMesh no longer creates or funds Worker-held bounty escrow.");
    setOcticon(bountyAllButton, "tag", 16);

    bountyAllAmount->hide();
    bountyAllButton->hide();
    // The status/label/milestone drop-downs are gone from the chrome: the top
    // search owns filtering now (is:open, label:x, milestone:y), and the row
    // chips write those terms into it. The combos stay alive off-screen as the
    // filter state the search and the milestone drill-down both drive.
    m_issueStatusFilter->setParent(page);
    m_issueLabelFilter->setParent(page);
    m_issueMilestoneFilter->setParent(page);
    m_issueStatusFilter->hide();
    m_issueLabelFilter->hide();
    m_issueMilestoneFilter->hide();
    headingRow->addWidget(m_issueCreditsButton);
    return headingRow;
}

// The Issues / Milestones / Labels / Board pages of the list pane.
void MainWindow::buildIssueListViews()
{
    m_issueTable = new QTableWidget(0, 17);
    m_issueTable->setObjectName("issueList");
    enableHoverRowHighlight(m_issueTable);
    m_issueTable->setHorizontalHeaderLabels(
        {"#", "Title", "Priority", "Status", "Votes", "Labels", "Milestone",
         "Created", "Updated", "Agent", "Author", "Progress", "Est. cost",
         "Bounty", "Comments", "Files", "Assignee"});
    m_issueTable->verticalHeader()->setVisible(false);
    m_issueTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_issueTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_issueTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_issueTable->setShowGrid(false);
    m_issueTable->setWordWrap(false);
    m_issueTable->setSortingEnabled(true);
    m_issueTable->sortByColumn(2, Qt::AscendingOrder);
    m_issueTable->setToolTip(
        "Select an issue to keep its detail visible. Double-click a row to edit its title.");
    QHeaderView *header = m_issueTable->horizontalHeader();
    header->setHighlightSections(false);
    header->hide();
    header->setSectionResizeMode(0, QHeaderView::ResizeToContents); // #
    header->setSectionResizeMode(1, QHeaderView::Interactive);      // Title
    m_issueTable->setColumnWidth(1, 360);
    header->setSectionResizeMode(2, QHeaderView::ResizeToContents); // Priority
    header->setSectionResizeMode(3, QHeaderView::ResizeToContents); // Status
    header->setSectionResizeMode(4, QHeaderView::ResizeToContents); // Votes
    header->setSectionResizeMode(5, QHeaderView::ResizeToContents); // Labels
    header->setSectionResizeMode(6, QHeaderView::ResizeToContents); // Milestone
    header->setSectionResizeMode(7, QHeaderView::ResizeToContents);  // Created
    header->setSectionResizeMode(8, QHeaderView::ResizeToContents);  // Updated
    header->setSectionResizeMode(9, QHeaderView::ResizeToContents);  // Agent
    header->setSectionResizeMode(10, QHeaderView::ResizeToContents); // Author
    header->setSectionResizeMode(11, QHeaderView::ResizeToContents); // Progress
    header->setSectionResizeMode(12, QHeaderView::ResizeToContents); // Est. cost
    header->setSectionResizeMode(13, QHeaderView::ResizeToContents); // Bounty
    header->setSectionResizeMode(14, QHeaderView::ResizeToContents); // Comments
    header->setSectionResizeMode(15, QHeaderView::ResizeToContents); // Files
    header->setSectionResizeMode(16, QHeaderView::ResizeToContents); // Assignee
    for (int column = 0; column < m_issueTable->columnCount(); ++column)
        m_issueTable->setColumnHidden(column, column != 1);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    m_issueTable->setItemDelegateForColumn(
        1, new IssueSummaryDelegate(m_issueTable));
    m_issueTable->setMouseTracking(true);
    m_issueTable->setFrameShape(QFrame::NoFrame);
    m_issueTable->setItemDelegateForColumn(11, new ProgressBarDelegate(m_issueTable));
    m_issueTable->viewport()->installEventFilter(this);
    m_issueTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_issueTable, &QWidget::customContextMenuRequested, this,
            &MainWindow::showIssueListContextMenu);

    m_issueMilestonesTable = new QTableWidget(0, 6);
    m_issueMilestonesTable->setObjectName("issueTable");
    installColumnHeaderMenu(m_issueMilestonesTable);
    enableHoverRowHighlight(m_issueMilestonesTable);
    m_issueMilestonesTable->setHorizontalHeaderLabels(
        {"Milestone", "Open", "Closed", "Progress", "Due", "Status"});
    m_issueMilestonesTable->verticalHeader()->setVisible(false);
    m_issueMilestonesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_issueMilestonesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_issueMilestonesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_issueMilestonesTable->setShowGrid(false);
    m_issueMilestonesTable->setSortingEnabled(true);
    QHeaderView *msHeader = m_issueMilestonesTable->horizontalHeader();
    msHeader->setHighlightSections(false);
    msHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < 6; ++i)
        msHeader->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_issueMilestonesTable);

    m_issueLabelsTable = new QTableWidget(0, 5);
    m_issueLabelsTable->setObjectName("issueTable");
    installColumnHeaderMenu(m_issueLabelsTable);
    enableHoverRowHighlight(m_issueLabelsTable);
    m_issueLabelsTable->setHorizontalHeaderLabels(
        {"Label", "Open", "Closed", "Total", ""});
    m_issueLabelsTable->verticalHeader()->setVisible(false);
    m_issueLabelsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_issueLabelsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_issueLabelsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_issueLabelsTable->setShowGrid(false);
    m_issueLabelsTable->setSortingEnabled(true);
    QHeaderView *labelHeader = m_issueLabelsTable->horizontalHeader();
    labelHeader->setHighlightSections(false);
    labelHeader->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < 5; ++i)
        labelHeader->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_issueLabelsTable);

    // Page 0 is the table plus the "no results" notice that replaces it, not
    // the bare table: the stack's indices are addressed by number (subTab
    // restore, the board check), so the notice rides inside page 0 rather than
    // becoming a page of its own.
    auto *issueListPage = new QWidget;
    auto *issueListPageLayout = new QVBoxLayout(issueListPage);
    issueListPageLayout->setContentsMargins(0, 0, 0, 0);
    issueListPageLayout->setSpacing(0);
    issueListPageLayout->addWidget(m_issueTable, 1);
    m_issueSearchEmpty = createSearchEmptyNotice();
    issueListPageLayout->addWidget(m_issueSearchEmpty, 1);

    m_issueListStack = new QStackedWidget;
    m_issueListStack->addWidget(issueListPage);         // 0 Issues
    m_issueListStack->addWidget(m_issueMilestonesTable); // 1 Milestones
    m_issueListStack->addWidget(m_issueLabelsTable);     // 2 Labels
    m_issueListStack->addWidget(buildIssueBoard());      // 3 Board (Kanban)
}

// The issue detail column: title row, status, notices, thread and composer.
QWidget *MainWindow::buildIssueDetailCenter(QPushButton *&closeDetailButton)
{
    m_issueTitle = new QLabel("Select an issue");
    m_issueTitle->setObjectName("issuePageTitle");
    m_issueTitle->setTextFormat(Qt::RichText);
    m_issueTitle->setWordWrap(true);
    m_issueParentRef = new QWidget;
    m_issueParentRef->setObjectName(QStringLiteral("issueParentRef"));
    auto *parentRefLayout = new FlowLayout(m_issueParentRef, 0, 6, 6);
    auto *parentRefCaption =
        new QLabel(QStringLiteral("Sub-issue of"), m_issueParentRef);
    parentRefCaption->setObjectName(QStringLiteral("issueParentCaption"));
    parentRefLayout->addWidget(parentRefCaption);
    m_issueParentRef->hide();
    m_issueTitleEditor = new QLineEdit;
    m_issueTitleEditor->setObjectName("issueTitleEditor");
    m_issueTitleEditor->setPlaceholderText("Issue title");
    m_issueTitleEditor->hide();
    m_issueTitleEditButton = new QPushButton;
    m_issueTitleEditButton->setObjectName("issueIconButton");
    m_issueTitleEditButton->setFixedSize(30, 30);
    m_issueTitleEditButton->setCursor(Qt::PointingHandCursor);
    m_issueTitleEditButton->setToolTip("Edit title");
    setOcticon(m_issueTitleEditButton, "pencil", 15);
    m_issueTitleSaveButton = new QPushButton("Save");
    m_issueTitleSaveButton->setObjectName("primaryButton");
    m_issueTitleSaveButton->setProperty("buttonSize", "sm");
    m_issueTitleSaveButton->setCursor(Qt::PointingHandCursor);
    m_issueTitleSaveButton->hide();
    m_issueTitleCancelButton = new QPushButton("Cancel");
    m_issueTitleCancelButton->setObjectName("ghostButton");
    m_issueTitleCancelButton->setProperty("buttonSize", "sm");
    m_issueTitleCancelButton->setCursor(Qt::PointingHandCursor);
    m_issueTitleCancelButton->hide();
    m_issueCopyButton = new QPushButton;
    m_issueCopyButton->setObjectName("issueIconButton");
    m_issueCopyButton->setFixedSize(30, 30);
    m_issueCopyButton->setCursor(Qt::PointingHandCursor);
    m_issueCopyButton->setToolTip("Copy the issue title to the clipboard");
    setOcticon(m_issueCopyButton, "copy", 16);
    m_issueCopyAllButton = new QPushButton;
    m_issueCopyAllButton->setObjectName("issueIconButton");
    m_issueCopyAllButton->setFixedSize(30, 30);
    m_issueCopyAllButton->setCursor(Qt::PointingHandCursor);
    m_issueCopyAllButton->setToolTip(
        "Copy the issue and all comments to the clipboard");
    setOcticon(m_issueCopyAllButton, "comment", 16);
    m_issueVoteButton = new QPushButton("Vote");
    m_issueVoteButton->setObjectName("ghostButton");
    m_issueVoteButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_issueVoteButton, "thumbsup", 16);
    m_issueVoteButton->setToolTip("Upvote this issue (spends 1 voting credit)");
    auto *issueTitleRow = new QHBoxLayout;
    issueTitleRow->setContentsMargins(0, 0, 0, 0);
    issueTitleRow->setSpacing(8);
    issueTitleRow->addWidget(m_issueTitle, 1);
    issueTitleRow->addWidget(m_issueTitleEditor, 1);
    issueTitleRow->addWidget(m_issueTitleSaveButton, 0, Qt::AlignTop);
    issueTitleRow->addWidget(m_issueTitleCancelButton, 0, Qt::AlignTop);
    issueTitleRow->addWidget(m_issueTitleEditButton, 0, Qt::AlignTop);
    issueTitleRow->addStretch();
    closeDetailButton = new QPushButton;
    closeDetailButton->setObjectName("issueIconButton");
    closeDetailButton->setFixedSize(30, 30);
    closeDetailButton->setCursor(Qt::PointingHandCursor);
    closeDetailButton->setToolTip("Close issue detail");
    setOcticon(closeDetailButton, "x", 16);
    issueTitleRow->addWidget(closeDetailButton, 0, Qt::AlignTop);
    issueTitleRow->addWidget(m_issueNewButton, 0, Qt::AlignTop);
    issueTitleRow->addWidget(m_issueCopyButton, 0, Qt::AlignTop);
    issueTitleRow->addWidget(m_issueCopyAllButton, 0, Qt::AlignTop);
    m_issueMeta = new QLabel; // Open/Closed status pill
    m_issueMeta->setObjectName("issueStatusPill");
    m_issueMeta->setTextFormat(Qt::PlainText);
    m_issueMeta->setAlignment(Qt::AlignCenter);
    m_issueMeta->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_issueReadonlyNote = new QLabel;
    m_issueReadonlyNote->setObjectName("statusLine");
    m_issueReadonlyNote->setWordWrap(true);
    m_issueReadonlyNote->hide();
    m_issueInlineNotice = new QLabel;
    m_issueInlineNotice->setObjectName("issueInlineNotice");
    m_issueInlineNotice->setWordWrap(true);
    m_issueInlineNotice->hide();

    m_issueThreadContainer = new QWidget;
    m_issueThreadLayout = new QVBoxLayout(m_issueThreadContainer);
    m_issueThreadLayout->setContentsMargins(0, 0, 0, 0);
    m_issueThreadLayout->setSpacing(10);
    m_issueThreadLayout->addStretch();
    m_issueThreadScroll = new QScrollArea;
    m_issueThreadScroll->setWidgetResizable(true);
    m_issueThreadScroll->setWidget(m_issueThreadContainer);
    m_issueThreadScroll->setObjectName("issuePageScroll");
    m_issueThreadScroll->setFrameShape(QFrame::NoFrame);

    auto *commentAvatar = new QLabel("FM");
    commentAvatar->setObjectName("issueAvatar");
    commentAvatar->setAlignment(Qt::AlignCenter);
    commentAvatar->setFixedSize(36, 36);
    commentAvatar->setScaledContents(true);
    m_issueComposerAvatar = commentAvatar;
    refreshIssueComposerAvatar();
    auto *commentTitle = new QLabel(
        QStringLiteral("Commenting as <b>%1</b>")
            .arg(topBarUserName().toHtmlEscaped()));
    commentTitle->setObjectName("issueCommentTitle");
    commentTitle->setTextFormat(Qt::RichText);
    m_issueComposerTitle = commentTitle;
    m_issueComposer = new MarkdownEditor;
    m_issueComposer->setObjectName("issueCommentEditor");
    m_issueComposer->layout()->setContentsMargins(1, 1, 1, 1);
    m_issueComposer->setMinimumHeight(190);
    m_issueComposer->setPlaceholderText(
        "Use Markdown to format your comment, or @opus / @fable to ask a model "
        "here (\"@fable fix this\" starts an agent)");
    m_issueAttachButton = new QPushButton("Paste, drop, or click to add files");
    m_issueAskAiButton = new QPushButton("Ask AI");
    m_issueCloseButton = new QPushButton("Close issue");
    m_issueCloseCommentButton = new QPushButton("Close with comment");
    m_issueCloseCommentButton->setToolTip(
        "Post the comment above and close the issue in one step");
    m_issueCommentButton = new QPushButton("Comment");
    m_issueCommentButton->setObjectName("primaryButton");
    m_issueCommentButton->setCursor(Qt::PointingHandCursor);
    m_issueCommentButton->setToolTip(
        "Post the comment. @-mention a model in it (\"@opus why does this "
        "hang?\") and it answers in the thread; ask one for the change "
        "(\"@fable fix this\") and it starts a coding agent on the issue.");
    for (QPushButton *b : {m_issueAttachButton, m_issueAskAiButton,
                           m_issueCloseButton, m_issueCloseCommentButton,
                           m_issueVoteButton}) {
        b->setObjectName("ghostButton");
        b->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_issueAttachButton, "paperclip", 16);
    setOcticon(m_issueAskAiButton, "comment", 16);
    m_issueAskAiButton->setToolTip(
        "Ask OpenAI a question from the comment box and post the answer");
    auto *commentButtonRow = new QHBoxLayout;
    commentButtonRow->setContentsMargins(0, 0, 0, 0);
    commentButtonRow->addWidget(m_issueAttachButton, 0, Qt::AlignLeft);
    commentButtonRow->addWidget(makeVoiceButton(m_issueComposer), 0, Qt::AlignLeft);
    commentButtonRow->addStretch();
    commentButtonRow->addWidget(m_issueAskAiButton);
    commentButtonRow->addWidget(m_issueVoteButton);
    commentButtonRow->addWidget(m_issueCloseButton);
    commentButtonRow->addWidget(m_issueCloseCommentButton);
    commentButtonRow->addWidget(m_issueCommentButton);
    auto *commentActions = new QWidget;
    commentActions->setLayout(commentButtonRow);
    commentActions->adjustSize();
    auto *commentActionsScroll = new QScrollArea;
    commentActionsScroll->setObjectName("issueCommentActionsScroll");
    commentActionsScroll->setFrameShape(QFrame::NoFrame);
    commentActionsScroll->setWidget(commentActions);
    commentActionsScroll->setWidgetResizable(false);
    commentActionsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    commentActionsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    commentActionsScroll->setMinimumWidth(0);
    commentActionsScroll->setSizePolicy(QSizePolicy::Ignored,
                                        QSizePolicy::Fixed);
    commentActionsScroll->setFixedHeight(
        commentActions->sizeHint().height() +
        style()->pixelMetric(QStyle::PM_ScrollBarExtent) + 2);
    auto *commentCard = new QWidget;
    commentCard->setObjectName("issueCommentCard");
    auto *commentColumn = new QVBoxLayout(commentCard);
    commentColumn->setContentsMargins(12, 10, 12, 12);
    commentColumn->setSpacing(8);
    commentColumn->addWidget(commentTitle);
    commentColumn->addWidget(m_issueComposer);
    commentColumn->addWidget(commentActionsScroll);
    m_issueComposerRow = new QWidget(m_issueThreadContainer);
    m_issueComposerRow->setObjectName("issueComposerRow");
    auto *composerRow = new QHBoxLayout(m_issueComposerRow);
    composerRow->setContentsMargins(0, 0, 0, 0);
    composerRow->setSpacing(14);
    composerRow->addWidget(commentAvatar, 0, Qt::AlignTop);
    composerRow->addWidget(commentCard, 1);
    m_issueComposerRow->hide();

    auto *center = new QWidget;
    center->setMinimumWidth(0);
    center->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    auto *centerLayout = new QVBoxLayout(center);
    centerLayout->setContentsMargins(24, 22, 22, 22);
    centerLayout->setSpacing(12);
    centerLayout->addLayout(issueTitleRow);
    centerLayout->addWidget(m_issueMeta);
    centerLayout->addWidget(m_issueParentRef);
    centerLayout->addWidget(m_issueReadonlyNote);
    centerLayout->addWidget(m_issueInlineNotice);
    auto *issueDivider = new QWidget;
    issueDivider->setObjectName("issueDivider");
    issueDivider->setFixedHeight(1);
    centerLayout->addWidget(issueDivider);
    centerLayout->addWidget(m_issueThreadScroll, 1);
    return center;
}

QWidget *MainWindow::buildIssueSidebar()
{
    auto *meta = new QWidget;
    meta->setObjectName("issueSidebar");
    meta->setMinimumWidth(190);
    meta->setMaximumWidth(300);
    buildIssueSidebarValues();
    buildIssueSidebarEditors(meta);
    QWidget *agentBox = buildIssueAgentBox(meta);
    layoutIssueSidebar(meta, agentBox);
    return meta;
}

// The sidebar's value labels, progress slider and the gear/nudge buttons.
void MainWindow::buildIssueSidebarValues()
{
    m_issueAssigneesValue = new QLabel("No one - <a href='#'>Assign yourself</a>");
    m_issueReporterValue = new QLabel("Unknown - <a href='#'>Credit yourself</a>");
    m_issueLabelsValue = new QLabel("No labels");
    m_issueMilestoneValue = new QLabel("No milestone");
    m_issueDatesValue = new QLabel("No dates");
    m_issuePriorityValue = new QLabel("No priority");
    m_issueEstimateValue = new QLabel("\xE2\x80\x94");
    m_issueBountyValue = new QLabel("No bounty");
    m_issueCommentsValue = new QLabel("No comments");
    auto *progressSlider = new ProgressSlider;
    m_issueProgressSlider = progressSlider;
    progressSlider->onCommitted = [this](int pct) {
        if (m_currentIssueNumber < 0)
            return;
        IssueStore store = issueStoreForCurrentRepo();
        QString error;
        if (!store.setProgress(m_currentIssueNumber, pct, &error)) {
            setIssueInlineNotice(error.isEmpty() ? "Could not update progress." : error,
                                 true);
            return;
        }
        setIssueInlineNotice(QStringLiteral("Progress set to %1%.").arg(pct));
        reloadIssues();
    };
    for (QLabel *v : {m_issueAssigneesValue, m_issueReporterValue,
                      m_issueLabelsValue,
                      m_issueMilestoneValue, m_issueDatesValue,
                      m_issuePriorityValue,
                      m_issueEstimateValue,
                      m_issueBountyValue,
                      m_issueCommentsValue}) {
        v->setObjectName("statusLine");
        v->setWordWrap(true);
        v->setTextFormat(Qt::RichText);
        v->setOpenExternalLinks(false);
        v->setTextInteractionFlags(Qt::TextBrowserInteraction);
    }
    connect(m_issueAssigneesValue, &QLabel::linkActivated, this,
            [this](const QString &href) {
        if (href.startsWith(kUserLinkScheme)) {
            openBodyReference(href);
            return;
        }
        editIssueAssignees();
        if (!m_issueAssigneesEdit || !m_issueAssigneesStack ||
            m_issueAssigneesStack->currentIndex() != 1)
            return; // read-only repository: editIssueAssignees() explained why
        const QString who = m_userName.trimmed();
        if (who.isEmpty()) {
            setIssueInlineNotice("Set your profile name before assigning yourself.", true);
            return;
        }
        QStringList assignees = splitIssueFieldList(m_issueAssigneesEdit->text());
        if (!assignees.contains(who))
            assignees << who;
        m_issueAssigneesEdit->setText(assignees.join(", "));
    });
    connect(m_issueReporterValue, &QLabel::linkActivated, this,
            [this](const QString &href) {
        if (href.startsWith(kUserLinkScheme)) {
            openBodyReference(href);
            return;
        }
        editIssueReporter();
        if (!m_issueReporterEdit || !m_issueReporterStack ||
            m_issueReporterStack->currentIndex() != 1)
            return; // read-only repository: editIssueReporter() explained why
        const QString who = m_userName.trimmed();
        if (who.isEmpty()) {
            setIssueInlineNotice("Set your profile name before crediting yourself.",
                                 true);
            return;
        }
        m_issueReporterEdit->setText(who);
    });
    m_issueLabelsButton = new QPushButton;
    m_issueMilestoneButton = new QPushButton;
    m_issueDatesButton = new QPushButton;
    m_issuePriorityButton = new QPushButton;
    m_issueProgressButton = new QPushButton;
    m_issueBountyButton = new QPushButton;
    m_issueAssigneesButton = new QPushButton;
    m_issueReporterButton = new QPushButton;
    m_issueDeleteButton = new QPushButton("Delete issue");
    for (QPushButton *b : {m_issueLabelsButton, m_issueMilestoneButton,
                           m_issueDatesButton,
                           m_issuePriorityButton, m_issueProgressButton,
                           m_issueBountyButton,
                           m_issueAssigneesButton, m_issueReporterButton}) {
        b->setObjectName("issueIconButton");
        b->setFixedSize(28, 28);
        b->setCursor(Qt::PointingHandCursor);
        setOcticon(b, "gear", 15);
    }
    m_issueBountyButton->setObjectName(
        QStringLiteral("legacyIssueBountyDisabled"));
    m_issueBountyButton->setEnabled(false);
    m_issueBountyButton->setToolTip(
        QStringLiteral("New Worker-held bounty escrow is disabled; existing "
                       "records are retained for migration only."));
    m_issueDeleteButton->setObjectName("issueDangerLink");
    m_issueDeleteButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_issueDeleteButton, "trash", 15);
    m_issuePriorityRaiseButton = new QPushButton;
    m_issuePriorityLowerButton = new QPushButton;
    for (QPushButton *b : {m_issuePriorityRaiseButton, m_issuePriorityLowerButton}) {
        b->setObjectName("issueIconButton");
        b->setFixedSize(28, 28);
        b->setCursor(Qt::PointingHandCursor);
    }
    setOcticon(m_issuePriorityRaiseButton, "chevron-up", 15);
    setOcticon(m_issuePriorityLowerButton, "chevron-down", 15);
    m_issuePriorityRaiseButton->setToolTip("Raise priority (more important)");
    m_issuePriorityLowerButton->setToolTip("Lower priority (less important)");
    connect(m_issuePriorityRaiseButton, &QPushButton::clicked, this,
            [this] { nudgeIssuePriority(-1); });
    connect(m_issuePriorityLowerButton, &QPushButton::clicked, this,
            [this] { nudgeIssuePriority(1); });
    m_issueProgressBoostButton = new QPushButton;
    m_issueProgressBoostButton->setObjectName("issueIconButton");
    m_issueProgressBoostButton->setFixedSize(28, 28);
    m_issueProgressBoostButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_issueProgressBoostButton, "plus", 15);
    m_issueProgressBoostButton->setToolTip("Mark in progress (+10%)");
    connect(m_issueProgressBoostButton, &QPushButton::clicked, this,
            [this] { nudgeIssueProgress(10); });
}

// The inline editors behind each sidebar gear: assignees, reporter, labels,
// milestone, dates and priority.
void MainWindow::buildIssueSidebarEditors(QWidget *meta)
{
    auto makeEditorButton = [meta](const QString &text, const char *objectName) {
        return issueSidebarEditorButton(meta, text, objectName);
    };
    auto makeInlineButtonRow = [meta](QPushButton *save, QPushButton *cancel) {
        return issueSidebarInlineButtonRow(meta, save, cancel);
    };

    m_issueAssigneesStack = new CurrentPageStack(meta);
    m_issueAssigneesStack->addWidget(m_issueAssigneesValue);
    auto *assigneesEditBox = new QWidget(meta);
    auto *assigneesEditLayout = new QVBoxLayout(assigneesEditBox);
    assigneesEditLayout->setContentsMargins(0, 0, 0, 0);
    assigneesEditLayout->setSpacing(6);
    m_issueAssigneesEdit = new QLineEdit(meta);
    m_issueAssigneesEdit->setPlaceholderText("No one");
    // Suggest people while a name is typed instead of making the reporter
    // recall an exact account (issue #688): the same people the gear's picker
    // lists - you, everyone on this issue, then the user directory.
    m_issueAssigneesSuggestionModel = new QStringListModel(this);
    auto *assigneesCompleter = new IssueAssigneeCompleter(
        m_issueAssigneesSuggestionModel, m_issueAssigneesEdit);
    assigneesCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    assigneesCompleter->setFilterMode(Qt::MatchContains);
    assigneesCompleter->setCompletionMode(QCompleter::PopupCompletion);
    assigneesCompleter->setMaxVisibleItems(10);
    m_issueAssigneesEdit->setCompleter(assigneesCompleter);
    connect(this, &MainWindow::chatUserDirectoryChanged, this,
            &MainWindow::refreshIssueAssigneeSuggestions);
    m_issueAssigneesSelfButton = makeEditorButton("Assign yourself", "ghostButton");
    m_issueAssigneesSaveButton = makeEditorButton("Save", "primaryButton");
    auto *assigneesCancel = makeEditorButton("Cancel", "ghostButton");
    assigneesEditLayout->addWidget(m_issueAssigneesEdit);
    assigneesEditLayout->addWidget(m_issueAssigneesSelfButton, 0, Qt::AlignLeft);
    assigneesEditLayout->addWidget(
        makeInlineButtonRow(m_issueAssigneesSaveButton, assigneesCancel));
    m_issueAssigneesStack->addWidget(assigneesEditBox);
    connect(m_issueAssigneesSelfButton, &QPushButton::clicked, this, [this] {
        if (!m_issueAssigneesEdit)
            return;
        const QString who = m_userName.trimmed();
        if (who.isEmpty()) {
            setIssueInlineNotice("Set your profile name before assigning yourself.", true);
            return;
        }
        QStringList assignees = splitIssueFieldList(m_issueAssigneesEdit->text());
        if (!assignees.contains(who))
            assignees << who;
        m_issueAssigneesEdit->setText(assignees.join(", "));
    });
    connect(m_issueAssigneesSaveButton, &QPushButton::clicked, this,
            &MainWindow::saveIssueAssigneesInline);
    connect(assigneesCancel, &QPushButton::clicked, this,
            &MainWindow::cancelIssueSidebarEditors);
    connect(m_issueAssigneesEdit, &QLineEdit::returnPressed, this,
            &MainWindow::saveIssueAssigneesInline);

    m_issueReporterStack = new CurrentPageStack(meta);
    m_issueReporterStack->addWidget(m_issueReporterValue);
    auto *reporterEditBox = new QWidget(meta);
    auto *reporterEditLayout = new QVBoxLayout(reporterEditBox);
    reporterEditLayout->setContentsMargins(0, 0, 0, 0);
    reporterEditLayout->setSpacing(6);
    m_issueReporterEdit = new QLineEdit(meta);
    m_issueReporterEdit->setPlaceholderText("Who reported this");
    m_issueReporterSelfButton = makeEditorButton("Credit yourself", "ghostButton");
    m_issueReporterSaveButton = makeEditorButton("Save", "primaryButton");
    auto *reporterCancel = makeEditorButton("Cancel", "ghostButton");
    reporterEditLayout->addWidget(m_issueReporterEdit);
    reporterEditLayout->addWidget(m_issueReporterSelfButton, 0, Qt::AlignLeft);
    reporterEditLayout->addWidget(
        makeInlineButtonRow(m_issueReporterSaveButton, reporterCancel));
    m_issueReporterStack->addWidget(reporterEditBox);
    connect(m_issueReporterSelfButton, &QPushButton::clicked, this, [this] {
        if (!m_issueReporterEdit)
            return;
        const QString who = m_userName.trimmed();
        if (who.isEmpty()) {
            setIssueInlineNotice("Set your profile name before crediting yourself.",
                                 true);
            return;
        }
        m_issueReporterEdit->setText(who);
    });
    connect(m_issueReporterSaveButton, &QPushButton::clicked, this,
            &MainWindow::saveIssueReporterInline);
    connect(reporterCancel, &QPushButton::clicked, this,
            &MainWindow::cancelIssueSidebarEditors);
    connect(m_issueReporterEdit, &QLineEdit::returnPressed, this,
            &MainWindow::saveIssueReporterInline);

    m_issueLabelsStack = new CurrentPageStack(meta);
    m_issueLabelsView = new QWidget(meta);
    // Tags wrap across the sidebar's width instead of stacking one per line.
    m_issueLabelsPillsLayout = new FlowLayout(m_issueLabelsView, 0, 4, 4);
    m_issueLabelsPillsLayout->addWidget(m_issueLabelsValue);
    m_issueLabelsStack->addWidget(m_issueLabelsView);
    auto *labelsEditBox = new QWidget(meta);
    auto *labelsEditLayout = new QVBoxLayout(labelsEditBox);
    labelsEditLayout->setContentsMargins(0, 0, 0, 0);
    labelsEditLayout->setSpacing(6);
    m_issueLabelsEdit = new QLineEdit(meta);
    m_issueLabelsEdit->setPlaceholderText("No labels");
    auto *labelsSave = makeEditorButton("Save", "primaryButton");
    auto *labelsCancel = makeEditorButton("Cancel", "ghostButton");
    labelsEditLayout->addWidget(m_issueLabelsEdit);
    labelsEditLayout->addWidget(makeInlineButtonRow(labelsSave, labelsCancel));
    m_issueLabelsStack->addWidget(labelsEditBox);
    connect(labelsSave, &QPushButton::clicked, this,
            &MainWindow::saveIssueLabelsInline);
    connect(labelsCancel, &QPushButton::clicked, this,
            &MainWindow::cancelIssueSidebarEditors);
    connect(m_issueLabelsEdit, &QLineEdit::returnPressed, this,
            &MainWindow::saveIssueLabelsInline);

    m_issueMilestoneStack = new CurrentPageStack(meta);
    m_issueMilestoneStack->addWidget(m_issueMilestoneValue);
    auto *milestoneEditBox = new QWidget(meta);
    auto *milestoneEditLayout = new QVBoxLayout(milestoneEditBox);
    milestoneEditLayout->setContentsMargins(0, 0, 0, 0);
    milestoneEditLayout->setSpacing(6);
    m_issueMilestoneEdit = new QComboBox(meta);
    auto *milestoneSave = makeEditorButton("Save", "primaryButton");
    auto *milestoneCancel = makeEditorButton("Cancel", "ghostButton");
    milestoneEditLayout->addWidget(m_issueMilestoneEdit);
    milestoneEditLayout->addWidget(makeInlineButtonRow(milestoneSave, milestoneCancel));
    m_issueMilestoneStack->addWidget(milestoneEditBox);
    connect(milestoneSave, &QPushButton::clicked, this,
            &MainWindow::saveIssueMilestoneInline);
    connect(milestoneCancel, &QPushButton::clicked, this,
            &MainWindow::cancelIssueSidebarEditors);

    m_issueDatesStack = new CurrentPageStack(meta);
    m_issueDatesStack->addWidget(m_issueDatesValue);
    auto *datesEditBox = new QWidget(meta);
    auto *datesEditLayout = new QVBoxLayout(datesEditBox);
    datesEditLayout->setContentsMargins(0, 0, 0, 0);
    datesEditLayout->setSpacing(6);
    auto makeIssueDateRow = [&](const QString &label, QCheckBox *&enableOut,
                                QDateEdit *&editOut) {
        auto *rowWidget = new QWidget(datesEditBox);
        auto *row = new QHBoxLayout(rowWidget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        enableOut = new QCheckBox(label, rowWidget);
        enableOut->setToolTip("Untick to leave this date unset");
        editOut = new QDateEdit(QDate::currentDate(), rowWidget);
        editOut->setCalendarPopup(true);
        editOut->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        editOut->setEnabled(false);
        connect(enableOut, &QCheckBox::toggled, editOut, &QWidget::setEnabled);
        row->addWidget(enableOut);
        row->addWidget(editOut, 1);
        return rowWidget;
    };
    datesEditLayout->addWidget(
        makeIssueDateRow("Start", m_issueStartDateEnable, m_issueStartDateEdit));
    datesEditLayout->addWidget(
        makeIssueDateRow("End", m_issueEndDateEnable, m_issueEndDateEdit));
    auto *datesSave = makeEditorButton("Save", "primaryButton");
    auto *datesCancel = makeEditorButton("Cancel", "ghostButton");
    datesEditLayout->addWidget(makeInlineButtonRow(datesSave, datesCancel));
    m_issueDatesStack->addWidget(datesEditBox);
    connect(datesSave, &QPushButton::clicked, this,
            &MainWindow::saveIssueDatesInline);
    connect(datesCancel, &QPushButton::clicked, this,
            &MainWindow::cancelIssueSidebarEditors);

    m_issuePriorityStack = new CurrentPageStack(meta);
    m_issuePriorityStack->addWidget(m_issuePriorityValue);
    auto *priorityEditBox = new QWidget(meta);
    auto *priorityEditLayout = new QVBoxLayout(priorityEditBox);
    priorityEditLayout->setContentsMargins(0, 0, 0, 0);
    priorityEditLayout->setSpacing(6);
    m_issuePriorityEdit = new QSpinBox(meta);
    m_issuePriorityEdit->setRange(0, 8999);
    m_issuePriorityEdit->setSpecialValueText("No priority");
    m_issuePriorityEdit->setToolTip("1 is highest priority; 8999 is lowest");
    auto *prioritySave = makeEditorButton("Save", "primaryButton");
    auto *priorityCancel = makeEditorButton("Cancel", "ghostButton");
    priorityEditLayout->addWidget(m_issuePriorityEdit);
    priorityEditLayout->addWidget(makeInlineButtonRow(prioritySave, priorityCancel));
    m_issuePriorityStack->addWidget(priorityEditBox);
    connect(prioritySave, &QPushButton::clicked, this,
            &MainWindow::saveIssuePriorityInline);
    connect(priorityCancel, &QPushButton::clicked, this,
            &MainWindow::cancelIssueSidebarEditors);
}

// The "Assign to agent" section: the agent/model picker and its actions.
QWidget *MainWindow::buildIssueAgentBox(QWidget *meta)
{
    auto makeEditorButton = [meta](const QString &text, const char *objectName) {
        return issueSidebarEditorButton(meta, text, objectName);
    };
    auto *agentBox = new QWidget(meta);
    auto *agentLayout = new QVBoxLayout(agentBox);
    agentLayout->setContentsMargins(0, 0, 0, 0);
    agentLayout->setSpacing(6);
    m_issueAgentValue = new QLabel("No agent assigned", meta);
    m_issueAgentValue->setObjectName("statusLine");
    m_issueAgentValue->setWordWrap(true);
    m_issueAgentValue->setTextFormat(Qt::RichText);
    // The whole summary is the session's link, not just the chevron beside it:
    // the first thing a reader reaches for is the agent's own name.
    m_issueAgentValue->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                               Qt::LinksAccessibleByMouse);
    m_issueAgentValue->setOpenExternalLinks(false);
    connect(m_issueAgentValue, &QLabel::linkActivated, this,
            [this](const QString &href) {
                if (!href.startsWith(kAgentLinkScheme))
                    return;
                const int sessionId = href.mid(kAgentLinkScheme.size()).toInt();
                if (sessionId > 0)
                    switchToAgentsTab(sessionId);
            });
    m_issueAgentProvider = new QComboBox(meta);
    auto addAgentProvider = [this](const QString &label,
                                   const QString &provider) {
        m_issueAgentProvider->addItem(
            agentModelIcon(provider, QString()),
            label, provider);
    };
    addAgentProvider(QStringLiteral("Codex"), kCodexProvider);
    addAgentProvider(QStringLiteral("OpenAI API"), QStringLiteral("openai"));
    addAgentProvider(QStringLiteral("Claude API"), QStringLiteral("claude-api"));
    addAgentProvider(QStringLiteral("Claude Code"), QStringLiteral("claude-code"));
    addAgentProvider(QStringLiteral("Cloudflare AI"), kCloudflareAiProvider);
    addAgentProvider(QStringLiteral("Local Llama"), kLocalLlamaProvider);
    selectDefaultAgentProvider(m_issueAgentProvider);
    m_issueAgentProvider->setToolTip("Which agent to run on this issue");
    m_issueAgentModel = new QComboBox(meta);
    m_issueAgentModel->setToolTip("Which model the agent uses");
    m_issueAgentModel->setProperty("claudeModelCombo", true);
    m_issueAgentModel->view()->installEventFilter(this);
    fillAgentFixModelCombo(m_issueAgentModel,
                           m_issueAgentProvider->currentData().toString());
    // The agent and its model are one choice, picked from the same combined
    // list the prompt composer offers. The two combos above stay as the pair's
    // storage — every reader of the assignment still goes through them — but
    // only this row is on screen.
    m_issueAgentSelector = new FullPopupComboBox(meta);
    m_issueAgentSelector->setObjectName("issueAgentModelSelector");
    m_issueAgentSelector->setIconSize(QSize(22, 22));
    m_issueAgentSelector->view()->setIconSize(QSize(26, 26));
    m_issueAgentSelector->setMaxVisibleItems(30);
    m_issueAgentSelector->setItemDelegate(
        new AgentChoiceDescriptionDelegate(m_issueAgentSelector));
    m_issueAgentSelector->setToolTip(
        "Choose the agent and model that will run on this issue.");
    m_issueAgentProvider->setVisible(false);
    m_issueAgentModel->setVisible(false);
    connect(m_issueAgentProvider,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
                if (m_issueAgentProvider && m_issueAgentModel) {
                    fillAgentFixModelCombo(
                        m_issueAgentModel,
                        m_issueAgentProvider->currentData().toString());
                    applyLiveClaudeModelsToCombos();
                }
                refreshIssueAgentModelSelector();
            });
    connect(m_issueAgentSelector,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                if (index < 0 || !m_issueAgentProvider || !m_issueAgentModel)
                    return;
                // Read the row before touching the provider: that write
                // rebuilds this list, after which `index` names something else.
                const QString provider =
                    m_issueAgentSelector->itemData(index).toString();
                const QString model =
                    m_issueAgentSelector->itemData(index, Qt::UserRole + 1)
                        .toString();
                const QString label = m_issueAgentSelector->itemText(index);
                const QSignalBlocker selectorBlock(m_issueAgentSelector);
                const int providerIndex = m_issueAgentProvider->findData(provider);
                if (providerIndex >= 0 &&
                    providerIndex != m_issueAgentProvider->currentIndex())
                    m_issueAgentProvider->setCurrentIndex(providerIndex);
                if (!model.isEmpty()) {
                    // A live model the static per-provider list has never heard
                    // of would otherwise fall back to that list's first row and
                    // silently start the wrong one.
                    if (m_issueAgentModel->findData(model) < 0)
                        m_issueAgentModel->addItem(label, model);
                    selectModelComboValue(m_issueAgentModel, model);
                }
                refreshIssueAgentModelSelector();
            });
    applyLiveClaudeModelsToCombos();
    refreshIssueAgentModelSelector();
    m_issueAssignAgentButton = makeEditorButton("Assign agent", "ghostButton");
    m_issueAgentCreatePrCheck = new QCheckBox("Create a PR", meta);
    m_issueAgentCreatePrCheck->setToolTip(
        "If the agent produces a patch, create a ForkMesh pull request from it.");
    m_issueAgentViewButton = makeEditorButton("View session", "primaryButton");
    m_issueAgentViewButton->hide();
    m_issueMcpPromptButton =
        makeEditorButton("Copy MCP prompt", "ghostButton");
    m_issueMcpPromptButton->setToolTip(
        "Copy a ready-to-paste prompt that asks an MCP-capable agent to solve "
        "this issue on a branch and open a pull request.");
    setOcticon(m_issueAssignAgentButton, "rocket", 15);
    setOcticon(m_issueAgentViewButton, "chevron-right", 15);
    setOcticon(m_issueMcpPromptButton, "copy", 15);
    agentLayout->addWidget(m_issueAgentValue);
    agentLayout->addWidget(m_issueAgentSelector);
    agentLayout->addWidget(m_issueAgentProvider);
    agentLayout->addWidget(m_issueAgentModel);
    agentLayout->addWidget(m_issueAssignAgentButton);
    agentLayout->addWidget(m_issueAgentCreatePrCheck);
    agentLayout->addWidget(m_issueMcpPromptButton, 0, Qt::AlignLeft);
    agentLayout->addWidget(m_issueAgentViewButton, 0, Qt::AlignLeft);
    connect(m_issueAssignAgentButton, &QPushButton::clicked, this, [this] {
        if (m_issueAgentProvider)
            assignIssueToAgent(m_issueAgentProvider->currentData().toString(),
                               m_issueAgentModel
                                   ? m_issueAgentModel->currentData().toString()
                                   : QString());
    });
    connect(m_issueAgentViewButton, &QPushButton::clicked, this,
            &MainWindow::openAgentSessionFromIssue);
    connect(m_issueMcpPromptButton, &QPushButton::clicked, this,
            [this] { copyIssueMcpPrompt(); });
    return agentBox;
}

// Copies a prompt that asks an MCP-capable agent to work the current issue on a
// branch and open a pull request, minting a connector token when none exists.
void MainWindow::copyIssueMcpPrompt()
{
    if (m_currentIssueNumber <= 0)
        return;
    const int repoIndex = issuesRepoIndex();
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    QString title = m_currentIssueTitle.trimmed();
    QString slug = title.toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                 QStringLiteral("-"));
    slug.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    slug = slug.left(40);
    const QString branch =
        slug.isEmpty()
            ? QStringLiteral("issue-%1").arg(m_currentIssueNumber)
            : QStringLiteral("issue-%1-%2").arg(m_currentIssueNumber).arg(slug);

    const QString appData =
        forkmesh::paths::dataRoot();
    forkmesh::mcp::Connector connector =
        forkmesh::mcp::loadConnector(appData);
    if (!connector.isValid()) {
        connector.token = forkmesh::mcp::generateToken();
        connector.node = m_profileIdentity.publicKey();
        connector.label = QStringLiteral("Issue MCP prompt");
        connector.createdMs = QDateTime::currentMSecsSinceEpoch();
        QString error;
        if (!forkmesh::mcp::saveConnector(appData, connector, &error)) {
            setIssueInlineNotice(
                error.isEmpty() ? QStringLiteral("Could not create an MCP connector token.")
                                : error,
                true);
            return;
        }
        refreshMcpConnectorTab();
    }

    const QString checkout = repo.localPath.trimmed().isEmpty()
                                 ? QStringLiteral("<CHECKOUT_OF_%1_%2>")
                                       .arg(repo.owner, repo.name)
                                 : repo.localPath;
    const QString script = mcpServerScriptPath().trimmed().isEmpty()
                               ? QStringLiteral(
                                     "<FORKMESH_CHECKOUT>/tools/forkmesh_mcp_server.py")
                               : mcpServerScriptPath();
    const QJsonObject server{
        {QStringLiteral("command"), QStringLiteral("python3")},
        {QStringLiteral("args"), QJsonArray{script}},
        {QStringLiteral("env"),
         QJsonObject{{QStringLiteral("FORKMESH_REPO"), checkout},
                     {QStringLiteral("FORKMESH_MCP_TOKEN"), connector.token}}},
    };
    const QString config = QString::fromUtf8(
        QJsonDocument(QJsonObject{
                          {QStringLiteral("mcpServers"),
                           QJsonObject{{QStringLiteral("forkmesh"), server}}}})
            .toJson(QJsonDocument::Indented));
    const QString repoSlug = repo.owner + QLatin1Char('/') + repo.name;
    const QString clone = repo.cloneUrl.trimmed().isEmpty()
                              ? repoSlug
                              : repo.cloneUrl.trimmed();
    const QString prompt = QStringLiteral(
        "Work ForkMesh issue #%1 in %2 end to end through the forkmesh MCP server.\n\n"
        "Issue: %3\nRepository: %4\n\n"
        "Connect the forkmesh MCP server first with this configuration:\n\n%5\n"
        "Treat FORKMESH_MCP_TOKEN as a secret; never print it in logs, commits, "
        "pull requests, or chat.\n\n"
        "1. Call whoami, then use search_issues to load issue #%1 and read_file "
        "for every relevant file before planning.\n"
        "2. Create branch %6 from the latest main. Never commit directly to main.\n"
        "3. Implement the smallest complete fix and run the repository's required "
        "tests and checks.\n"
        "4. Commit the result with a clear message referencing issue #%1.\n"
        "5. Use open_pr_from_branch to open a PR against main with the test results "
        "in its description.\n"
        "6. Use comment_on_issue to link the PR and report the model and reasoning "
        "setting used.\n\n"
        "If the connector is read-only or a safe submission is blocked, report the "
        "blocker instead of forcing a PR.")
                               .arg(m_currentIssueNumber)
                               .arg(repoSlug, title, clone, config, branch);
    QApplication::clipboard()->setText(prompt);
    setIssueInlineNotice(QStringLiteral("MCP prompt copied."));
}

void MainWindow::layoutIssueSidebar(QWidget *meta, QWidget *agentBox)
{
    auto makeValue = [](const QString &text) { return issueSidebarValueLabel(text); };
    auto makeGear = []() { return issueSidebarGearButton(); };
    auto makeAction = [](const QString &text, const QString &icon = QString()) {
        return issueSidebarActionButton(text, icon);
    };
    auto *metaLayout = new QVBoxLayout(meta);
    metaLayout->setContentsMargins(12, 14, 2, 14);
    metaLayout->setSpacing(0);
    auto addDivider = [&] {
        auto *line = new QWidget(meta);
        line->setObjectName("issueSidebarDivider");
        line->setFixedHeight(1);
        metaLayout->addWidget(line);
    };
    auto addMetaSection = [&](const QString &label, QWidget *value,
                              QPushButton *btn = nullptr,
                              const QList<QPushButton *> &extraBtns = {}) {
        auto *header = new QHBoxLayout;
        header->setContentsMargins(0, 0, 0, 0);
        auto *l = new QLabel(label);
        l->setObjectName("issueSidebarHeading");
        header->addWidget(l);
        header->addStretch();
        for (QPushButton *extra : extraBtns)
            header->addWidget(extra);
        if (btn)
            header->addWidget(btn);
        metaLayout->addLayout(header);
        // Each section is only as tall as its value. Left growable, the column's
        // spare height is shared out among them and every heading ends up
        // separated from its value by a band of empty space.
        const QSizePolicy::Policy vertical = value->sizePolicy().verticalPolicy();
        if (vertical == QSizePolicy::Preferred ||
            vertical == QSizePolicy::Minimum ||
            vertical == QSizePolicy::Expanding ||
            vertical == QSizePolicy::MinimumExpanding) {
            value->setSizePolicy(value->sizePolicy().horizontalPolicy(),
                                 QSizePolicy::Maximum);
        }
        metaLayout->addWidget(value);
        metaLayout->addSpacing(9);
        addDivider();
        metaLayout->addSpacing(9);
    };
    addMetaSection("Reporter", m_issueReporterStack, m_issueReporterButton);
    addMetaSection("Assignees", m_issueAssigneesStack, m_issueAssigneesButton);
    addMetaSection("Assign to agent", agentBox);
    addMetaSection("Labels", m_issueLabelsStack, m_issueLabelsButton);
    addMetaSection("Type", makeValue("No type"), makeGear());
    addMetaSection("Priority", m_issuePriorityStack, m_issuePriorityButton,
                   {m_issuePriorityRaiseButton, m_issuePriorityLowerButton});
    addMetaSection("Progress", m_issueProgressSlider, m_issueProgressButton,
                   {m_issueProgressBoostButton});
    addMetaSection("Est. OpenAI cost", m_issueEstimateValue);
    addMetaSection("Bounty", m_issueBountyValue, m_issueBountyButton);
    addMetaSection("Comments", m_issueCommentsValue);
    addMetaSection("Projects", makeValue("No projects"), makeGear());
    addMetaSection("Milestone", m_issueMilestoneStack, m_issueMilestoneButton);
    addMetaSection("Dates", m_issueDatesStack, m_issueDatesButton);
    addMetaSection("Relationships", makeValue("None yet"), makeGear());
    m_issueDevelopmentValue = makeValue("No linked pull requests.");
    m_issueDevelopmentValue->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_issueDevelopmentValue->setOpenExternalLinks(false);
    connect(m_issueDevelopmentValue, &QLabel::linkActivated, this,
            [this](const QString &href) {
                const int n = href.section(QLatin1Char(':'), 1).toInt();
                if (n > 0)
                    switchToPullTab(n);
            });
    m_issueLinkPullButton = new QPushButton;
    m_issueLinkPullButton->setObjectName("issueIconButton");
    m_issueLinkPullButton->setFixedSize(28, 28);
    m_issueLinkPullButton->setCursor(Qt::PointingHandCursor);
    m_issueLinkPullButton->setToolTip("Link a pull request to this issue");
    setOcticon(m_issueLinkPullButton, "git-pull-request", 15);
    connect(m_issueLinkPullButton, &QPushButton::clicked, this,
            &MainWindow::linkPullToIssueFromIssuePage);
    addMetaSection("Development", m_issueDevelopmentValue, m_issueLinkPullButton);
    addMetaSection("Pings", makeValue("You are receiving pings because you're subscribed to this thread."));
    addMetaSection("Participants", makeValue("No participants"));
    auto *transferIssue = makeAction("Transfer issue", "arrow-left");
    auto *cloneIssue = makeAction("Clone issue", "copy");
    auto *lockIssue = makeAction("Lock conversation", "lock");
    auto *pinIssue = makeAction("Pin issue", "tag");
    auto *copyIssueLink = makeAction("Copy link", "link");
    copyIssueLink->setToolTip(
        "Copy a link to this issue you can paste into an issue or PR comment");
    connect(copyIssueLink, &QPushButton::clicked, this, [this] {
        if (m_currentIssueNumber > 0)
            copyReferenceLink(QStringLiteral("issue"),
                              QString::number(m_currentIssueNumber));
    });
    m_issueMoveToTaskButton = makeAction("Move to task", "checklist");
    m_issueMoveToTaskButton->setToolTip(
        "Create an organization task from this issue and delete the issue "
        "once the task exists");
    connect(m_issueMoveToTaskButton, &QPushButton::clicked, this,
            &MainWindow::moveCurrentIssueToTask);
    auto *feedbackIssue = makeAction("Give feedback", "comment");
    for (QPushButton *action :
         {transferIssue, cloneIssue, lockIssue, pinIssue, copyIssueLink,
          m_issueMoveToTaskButton, m_issueDeleteButton, feedbackIssue})
        metaLayout->addWidget(action);
    metaLayout->addStretch();
}

// The detail pane: the thread/sidebar split and the "Changes in Git" tab.
void MainWindow::buildIssueDetailPane(QWidget *center, QWidget *meta)
{
    auto *metaScroll = new QScrollArea;
    metaScroll->setObjectName("issueSidebarScroll");
    metaScroll->setFrameShape(QFrame::NoFrame);
    metaScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    metaScroll->setWidgetResizable(true);
    metaScroll->setMinimumWidth(190);
    metaScroll->setMaximumWidth(270);
    metaScroll->setMinimumHeight(0);
    metaScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
    metaScroll->setWidget(meta);

    auto *issueDetailView = new QWidget;
    auto *detailSplit = new QSplitter(Qt::Horizontal);
    detailSplit->setChildrenCollapsible(true);
    detailSplit->addWidget(center);
    detailSplit->addWidget(metaScroll);
    detailSplit->setCollapsible(0, false);
    detailSplit->setCollapsible(1, true);
    detailSplit->setStretchFactor(0, 1);
    detailSplit->setStretchFactor(1, 0);
    detailSplit->setSizes({520, 220});

    m_issueFilesList = new QListWidget;
    m_issueFilesList->setObjectName("agentFilesList");
    m_issueFilesList->setMinimumWidth(140);
    m_issueFilesList->setItemDelegate(
        new SelectionBorderRowDelegate(m_issueFilesList));
    connect(m_issueFilesList, &QListWidget::itemActivated, this,
            [](QListWidgetItem *it) {
                const QString path = it->data(Qt::UserRole).toString();
                if (!path.isEmpty() && QFileInfo::exists(path))
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            });
    m_issueFilesChangedSummary = new QLabel;
    m_issueFilesChangedSummary->setObjectName("agentFilesHeading");
    auto *issueFilesV = new QVBoxLayout;
    issueFilesV->setContentsMargins(0, 0, 0, 0);
    issueFilesV->setSpacing(4);
    issueFilesV->addWidget(m_issueFilesChangedSummary);
    issueFilesV->addWidget(m_issueFilesList, 1);
    auto *issueFilesPanel = new QWidget;
    issueFilesPanel->setLayout(issueFilesV);

    m_issueDiffView = new QTextBrowser;
    m_issueDiffView->setObjectName("diffView");
    m_issueDiffView->setOpenExternalLinks(false);
    m_issueDiffView->setMinimumWidth(0);
    m_issueDiffView->setSizePolicy(QSizePolicy::Ignored,
                                   QSizePolicy::Expanding);
    registerDiffView(m_issueDiffView);

    auto *issueFilesSplit = new QSplitter(Qt::Horizontal);
    issueFilesSplit->setChildrenCollapsible(true);
    issueFilesSplit->addWidget(issueFilesPanel);
    issueFilesSplit->addWidget(m_issueDiffView);
    issueFilesSplit->setCollapsible(0, true);
    issueFilesSplit->setCollapsible(1, false);
    issueFilesSplit->setStretchFactor(0, 0);
    issueFilesSplit->setStretchFactor(1, 1);
    issueFilesSplit->setSizes({220, 700});
    auto *issueFilesPage = new QWidget;
    auto *issueFilesPageLayout = new QVBoxLayout(issueFilesPage);
    issueFilesPageLayout->setContentsMargins(0, 8, 0, 0);
    issueFilesPageLayout->addWidget(issueFilesSplit, 1);

    m_issueDetailTabs = new QTabWidget;
    m_issueDetailTabs->setObjectName("issueDetailTabs");
    m_issueDetailTabs->setDocumentMode(true);
    m_issueDetailTabs->setTabBarAutoHide(true);
    m_issueDetailTabs->addTab(detailSplit, QStringLiteral("Issue"));
    m_issueFilesTabIndex =
        m_issueDetailTabs->addTab(issueFilesPage, QStringLiteral("Changes in Git"));
    m_issueDetailTabs->setTabVisible(m_issueFilesTabIndex, false);
    // setTabBarAutoHide() counts hidden tabs, so a lone "Issue" tab kept the bar
    // on screen and pushed the title down. Show the bar only when the Changes
    // shortcut is actually there to click.
    syncIssueDetailTabBar();
    connect(m_issueDetailTabs, &QTabWidget::currentChanged, this,
            [this](int index) {
                if (index != m_issueFilesTabIndex || m_currentIssueNumber <= 0)
                    return;
                {
                    QSignalBlocker block(m_issueDetailTabs);
                    m_issueDetailTabs->setCurrentIndex(0);
                }
                if (const AgentSession *session =
                        latestAgentSessionForIssue(m_currentIssueNumber)) {
                    if (!session->branchName.isEmpty()) {
                        switchToAgentBranch(session->id);
                        return;
                    }
                }
                const QList<int> pulls = pullsLinkedToIssue(m_currentIssueNumber);
                if (!pulls.isEmpty())
                    openPullDiffInGitView(pulls.constLast());
            });

    auto *detailLayout = new QVBoxLayout(issueDetailView);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->addWidget(m_issueDetailTabs);

    m_issueDetailStack = new QStackedWidget;
    m_issueDetailStack->setObjectName(QStringLiteral("issueDetailOverlay"));
    m_issueDetailStack->addWidget(issueDetailView);
    m_issueDetail = m_issueDetailStack;
    m_issueDetail->setMinimumWidth(0);
    m_issueDetail->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
}

void MainWindow::connectIssuesSectionSignals(QButtonGroup *issueTabGroup,
                                             QPushButton *issueBurnupButton)
{
    connect(m_issuesRepoCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { reloadIssues(); });
    connect(m_issueStatusFilter, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshIssueList(); });
    connect(m_issueLabelFilter, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshIssueList(); });
    connect(m_issueMilestoneFilter, &QComboBox::currentIndexChanged, this,
            [this](int) { refreshIssueList(); });
    connect(m_issueSearch, &QLineEdit::textChanged, this,
            [this] { refreshIssueList(); });
    connect(issueTabGroup, &QButtonGroup::idClicked, this,
            [this](int id) { selectIssueListTab(id); });
    // Milestones and labels drill through to the issues they hold: clicking a
    // count switches to the Issues tab with the matching search already typed.
    connect(m_issueMilestonesTable, &QTableWidget::cellClicked, this,
            [this](int row, int col) {
        if (col != 1 && col != 2)
            return;
        QTableWidgetItem *titleItem = m_issueMilestonesTable->item(row, 0);
        if (!titleItem)
            return;
        showIssueListForQuery(
            QStringLiteral("milestone:\"%1\" is:%2")
                .arg(titleItem->text(), col == 1 ? QStringLiteral("open")
                                                 : QStringLiteral("closed")));
    });
    connect(m_issueLabelsTable, &QTableWidget::cellClicked, this,
            [this](int row, int col) {
        if (col < 1 || col > 3) // Open, Closed, Total
            return;
        QTableWidgetItem *nameItem = m_issueLabelsTable->item(row, 0);
        if (!nameItem)
            return;
        QString query = QStringLiteral("label:\"%1\"").arg(nameItem->text());
        if (col == 1)
            query += QStringLiteral(" is:open");
        else if (col == 2)
            query += QStringLiteral(" is:closed");
        showIssueListForQuery(query);
    });
    connect(m_issueTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const QModelIndexList rows = m_issueTable->selectionModel()->selectedRows();
        if (rows.isEmpty())
            return;
        QTableWidgetItem *first = m_issueTable->item(rows.first().row(), 0);
        if (first)
            showIssue(first->data(Qt::UserRole).toInt());
    });
    connect(m_issueTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int col) {
        QTableWidgetItem *first = m_issueTable->item(row, 0);
        if (!first)
            return;
        showIssue(first->data(Qt::UserRole).toInt());
        if (m_currentIssueNumber < 0)
            return;
        switch (col) {
        case 1:  promptEditIssueTitle(); break; // Title
        case 2:  editIssuePriority();    break; // Priority
        case 3:  toggleIssueStatus();    break; // Status (open <-> closed)
        case 5:  editIssueLabels();      break; // Labels
        case 6:  editIssueMilestone();   break; // Milestone
        case 11: editIssueProgress();    break; // Progress
        case 13: editIssueBounty();      break; // Bounty
        default: break; // #, Votes, Created, Updated, Agent, Author, Est. cost
        }
    });
    connect(m_issueLabelsTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { editIssueLabelDefinition(row); });
    connect(m_issueNewButton, &QPushButton::clicked, this, &MainWindow::promptNewIssue);
    connect(m_issueListNewButton, &QPushButton::clicked, this,
            &MainWindow::promptNewIssue);
    connect(m_issueSyncButton, &QPushButton::clicked, this, [this] {
        const int index = issuesRepoIndex();
        if (index < 0 || index >= m_repositories.size())
            return;
        // A button click is an explicit refresh, so it is not subject to the
        // short tab-flipping floor used by automatic page-load convergence.
        const RepositoryRecord &repo = m_repositories.at(index);
        const QString key = repo.owner.trimmed().toLower() + QLatin1Char('/') +
                            repo.name.trimmed().toLower();
        m_sourceConvergeAttemptMs.remove(key);
        syncIssuesFromMeshOnOpen(index);
    });
    connect(issueBurnupButton, &QPushButton::clicked, this,
            &MainWindow::showIssueBurnupChart);
    connect(m_issueTitleEditButton, &QPushButton::clicked, this,
            &MainWindow::promptEditIssueTitle);
    connect(m_issueTitleSaveButton, &QPushButton::clicked, this,
            &MainWindow::saveIssueTitleEdit);
    connect(m_issueTitleCancelButton, &QPushButton::clicked, this,
            &MainWindow::cancelIssueTitleEdit);
    connect(m_issueTitleEditor, &QLineEdit::returnPressed, this,
            &MainWindow::saveIssueTitleEdit);
    connect(m_issueCopyButton, &QPushButton::clicked, this,
            &MainWindow::copyIssueToClipboard);
    connect(m_issueCopyAllButton, &QPushButton::clicked, this,
            &MainWindow::copyIssueThreadToClipboard);
    connect(m_issueVoteButton, &QPushButton::clicked, this,
            &MainWindow::voteOnCurrentIssue);
    connect(m_issueCommentButton, &QPushButton::clicked, this,
            &MainWindow::addIssueComment);
    connect(m_issueAskAiButton, &QPushButton::clicked, this,
            &MainWindow::askAiForCurrentIssue);
    connect(m_issueAttachButton, &QPushButton::clicked, this,
            &MainWindow::attachIssueImage);
    connect(m_issueCloseButton, &QPushButton::clicked, this,
            &MainWindow::toggleIssueStatus);
    connect(m_issueCloseCommentButton, &QPushButton::clicked, this,
            &MainWindow::closeIssueWithComment);
    connect(m_issueDeleteButton, &QPushButton::clicked, this,
            &MainWindow::deleteCurrentIssue);
    connect(m_issueLabelsButton, &QPushButton::clicked, this,
            &MainWindow::editIssueLabels);
    connect(m_issueMilestoneButton, &QPushButton::clicked, this,
            &MainWindow::editIssueMilestone);
    connect(m_issueDatesButton, &QPushButton::clicked, this,
            &MainWindow::editIssueDates);
    connect(m_issuePriorityButton, &QPushButton::clicked, this,
            &MainWindow::editIssuePriority);
    connect(m_issueProgressButton, &QPushButton::clicked, this,
            &MainWindow::editIssueProgress);
    connect(m_issueBountyButton, &QPushButton::clicked, this,
            &MainWindow::editIssueBounty);
    connect(m_issueAssigneesButton, &QPushButton::clicked, this,
            &MainWindow::pickIssueAssignees);
    connect(m_issueReporterButton, &QPushButton::clicked, this,
            &MainWindow::pickIssueReporter);
}



int MainWindow::issuesRepoIndex() const
{
    if (!m_issuesRepoCombo || m_issuesRepoCombo->currentIndex() < 0)
        return -1;
    bool ok = false;
    const int idx = m_issuesRepoCombo->currentData().toInt(&ok);
    if (!ok || idx < 0 || idx >= m_repositories.size())
        return -1;
    return idx;
}

const RepositoryRecord &MainWindow::writableRecordFor(
    const RepositoryRecord &repo) const
{
    if (!repo.localPath.trimmed().isEmpty() &&
        QFileInfo::exists(repo.localPath + QStringLiteral("/.git")))
        return repo;
    for (const RepositoryRecord &r : m_repositories) {
        if (r.previewOnly || &r == &repo)
            continue;
        if (r.owner == repo.owner && r.name == repo.name &&
            !r.localPath.trimmed().isEmpty() &&
            QFileInfo::exists(r.localPath + QStringLiteral("/.git")))
            return r;
    }
    return repo;
}

QString MainWindow::repoAgentGitDir(const RepositoryRecord &repo) const
{
    const RepositoryRecord &writable = writableRecordFor(repo);
    if (!writable.localPath.trimmed().isEmpty() &&
        QFileInfo::exists(writable.localPath + QStringLiteral("/.git")))
        return writable.localPath;
    if (repo.previewOnly)
        return QString();
    const QString mirror = repo.mirrorPath.trimmed();
    if (!mirror.isEmpty() && QDir(mirror).exists())
        return mirror;
    return QString();
}

IssueStore MainWindow::issueStoreForCurrentRepo() const
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return IssueStore(QString(), QString(), &m_profileIdentity, chatDisplayName());
    const RepositoryRecord &target = m_repositories.at(idx);
    const RepositoryRecord &repo = writableRecordFor(target);
    IssueStore store(repo.localPath, repo.mirrorPath, &m_profileIdentity, chatDisplayName());
    store.setSigningRepository(target.owner, target.name);
    return store;
}

void MainWindow::refreshIssuesRepoCombo()
{
    if (!m_issuesRepoCombo)
        return;
    const QVariant previous =
        m_issuesRepoCombo->count() ? m_issuesRepoCombo->currentData() : QVariant();
    QSignalBlocker blocker(m_issuesRepoCombo);
    m_issuesRepoCombo->clear();
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        m_issuesRepoCombo->addItem(repo.owner + "/" + repo.name, i);
    }
    if (previous.isValid()) {
        const int restore = m_issuesRepoCombo->findData(previous);
        if (restore >= 0)
            m_issuesRepoCombo->setCurrentIndex(restore);
    }
    blocker.unblock();
    if (m_agentComposeRepo) {
        const QVariant prev = m_agentComposeRepo->count()
                                  ? m_agentComposeRepo->currentData()
                                  : QVariant();
        QSignalBlocker agentBlocker(m_agentComposeRepo);
        m_agentComposeRepo->clear();
        for (int i = 0; i < m_repositories.size(); ++i) {
            const RepositoryRecord &repo = m_repositories.at(i);
            if (repo.localPath.trimmed().isEmpty())
                continue; // no checkout to run an agent in
            m_agentComposeRepo->addItem(repo.owner + "/" + repo.name, i);
        }
        if (prev.isValid()) {
            const int restore = m_agentComposeRepo->findData(prev);
            if (restore >= 0)
                m_agentComposeRepo->setCurrentIndex(restore);
        }
    }
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testIssueSidebarSectionsFitTheirPage() const
{
    for (QStackedWidget *stack :
         {m_issueAssigneesStack, m_issueReporterStack, m_issueLabelsStack,
          m_issueMilestoneStack,
          m_issueDatesStack, m_issuePriorityStack}) {
        if (!stack || !stack->currentWidget())
            continue;
        if (stack->sizeHint().height() >
            stack->currentWidget()->sizeHint().height())
            return false;
    }
    return true;
}
#endif

QIcon MainWindow::issueAssigneeAvatar(const QString &name)
{
    const QString key = name.trimmed();
    if (key.isEmpty())
        return QIcon();
    auto it = m_assigneeAvatarCache.constFind(key);
    if (it != m_assigneeAvatarCache.constEnd())
        return it.value();
    // The seeded portrait is the stand-in for someone the user directory has
    // no picture for, not the picture itself. Reading it directly meant an
    // assignee who has uploaded a photo never wore it here, however long the
    // directory had been loaded. issueTimelineAvatar() prefers the real face
    // and falls back to the same seed, so one person keeps one face across the
    // issue views. refreshIssueAvatars() empties this cache when the directory
    // lands, so a stand-in cached before the fetch is not what the row keeps.
    QIcon icon(issueTimelineAvatar(QString(), key, 18));
    m_assigneeAvatarCache.insert(key, icon);
    return icon;
}

// True when `name` is one of the names this install signs with. Which one an
// event carries depends on how it was written (account, chat display name, the
// local profile name), so all of them have to count as "me".
bool MainWindow::isLocalAuthorName(const QString &name) const
{
    const QString candidate = name.trimmed();
    if (candidate.isEmpty())
        return false;
    for (const QString &mine :
         {topBarUserName(), chatDisplayName(), accountOwner(), m_userName}) {
        const QString trimmed = mine.trimmed();
        if (!trimmed.isEmpty() && trimmed.compare(candidate, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

// The face beside a timeline entry. `authorId` is the key that signed the
// event, `authorName` the name printed next to it. Reports filed on someone
// else's behalf — ForkBot intake, a Discord bug report — are signed by this
// node but carry the reporter's name, so the two disagree and the picture has
// to follow the name; keying it off the signature alone put the local user's
// own face on every proxied issue. Falls back to the generated portrait for
// the displayed name, which is the same seed the assignee badges use, so one
// person wears one face across the issue views.
QPixmap MainWindow::issueTimelineAvatar(const QString &authorId,
                                        const QString &authorName, int side)
{
    const QString name = authorName.trimmed();
    const bool signedHere =
        !authorId.isEmpty() && authorId == m_profileIdentity.publicKey();
    const bool self = signedHere && (name.isEmpty() || isLocalAuthorName(name));

    QPixmap known;
    if (!name.isEmpty())
        known = m_avatars.value(QStringLiteral("user:") + name.toLower());
    // A picture cached against the signing key only describes the reporter
    // when that key really is theirs.
    if (known.isNull() && !authorId.isEmpty() && (self || !signedHere))
        known = m_avatars.value(authorId);
    if (!known.isNull())
        return roundedAvatar(known, side, 0.5);
    if (self)
        return roundedAvatar(effectiveUserAvatar(), side, 0.5);
    const QString seed = name.isEmpty() ? authorId : name;
    return roundedAvatar(forkMeshAvatarPng(seed.toLower()), side, 0.5);
}

// Everyone's real picture rides in the user directory, and that is fetched
// after the issue views have already been painted: until it lands, every face
// here is the seeded stand-in portrait. The list bakes its faces into the row
// items and the sidebar caches its markup, so nothing noticed the pictures
// arriving — they only appeared once some unrelated edit (assigning the issue,
// most visibly) forced a full reload, which is issue #699.
//
// Swap the real pictures into the surfaces that are already drawn rather than
// rebuilding them: refreshIssueList() would drop the scroll position and
// renderIssueThread() would delete the cards under the reader, both for what
// is a pixmap change.
void MainWindow::refreshIssueAvatars()
{
    // Built before the directory, so it is full of stand-ins.
    m_assigneeAvatarCache.clear();

    if (m_issueTable && m_issueTable->rowCount() > 0) {
        QHash<int, const Issue *> byNumber;
        byNumber.reserve(m_currentIssues.size());
        for (const Issue &issue : std::as_const(m_currentIssues))
            byNumber.insert(issue.number, &issue);
        // Most rows share a handful of reporters; build each face once, the
        // way refreshIssueList() does when it fills them.
        QHash<QString, QPixmap> faces;
        const auto face = [&](const QString &authorId, const QString &name,
                              int side) {
            const QString key = authorId + QLatin1Char('\n') + name +
                                QLatin1Char('\n') + QString::number(side);
            auto it = faces.constFind(key);
            if (it == faces.constEnd())
                it = faces.insert(key,
                                  issueTimelineAvatar(authorId, name, side));
            return it.value();
        };
        for (int row = 0; row < m_issueTable->rowCount(); ++row) {
            QTableWidgetItem *titleItem = m_issueTable->item(row, 1);
            if (!titleItem)
                continue;
            const Issue *issue =
                byNumber.value(titleItem->data(Qt::UserRole).toInt());
            if (!issue)
                continue;
            if (!issue->assignees.isEmpty())
                titleItem->setIcon(
                    issueAssigneeAvatar(issue->assignees.constFirst()));
            if (!issue->author.isEmpty() ||
                !issue->authorName.trimmed().isEmpty())
                titleItem->setData(IssueAuthorAvatarRole,
                                   face(issue->author, issue->authorName, 36));
            // Same name the row prints, so the badge drops the same assignee
            // refreshIssueList() drops — issueRowAssignees() draws it that way.
            const QString author =
                issue->authorName.trimmed().isEmpty()
                    ? (issue->author.isEmpty()
                           ? QString::fromUtf8("\xE2\x80\x94")
                           : issue->author.left(8))
                    : issue->authorName.trimmed();
            QStringList badgeAssignees = issue->assignees;
            badgeAssignees.removeAll(author);
            if (!badgeAssignees.isEmpty())
                titleItem->setData(IssueAssigneeAvatarRole,
                                   face(QString(),
                                        badgeAssignees.constFirst(),
                                        kIssueAssigneeBadge));
        }
        m_issueTable->viewport()->update();
    }

    // Both sidebar values skip their markup when the names have not moved, and
    // the names have not — only the pictures behind them.
    m_issueReporterRenderKey.clear();
    m_issueAssigneesRenderKey.clear();
    renderIssueReporterValue(m_issueReporterShown);
    renderIssueAssigneesValue(m_issueAssigneesShown);

    if (m_issueThreadContainer) {
        const QList<QLabel *> faceLabels =
            m_issueThreadContainer->findChildren<QLabel *>();
        for (QLabel *label : faceLabels) {
            const QVariant side = label->property(kIssueAvatarSideProperty);
            if (!side.isValid())
                continue;
            const QPixmap refreshed = issueTimelineAvatar(
                label->property(kIssueAvatarAuthorIdProperty).toString(),
                label->property(kIssueAvatarAuthorNameProperty).toString(),
                side.toInt());
            if (refreshed.isNull())
                continue;
            label->setText(QString());
            label->setPixmap(refreshed);
        }
    }
}

// code-health: allow(blocking-wait) ~59 callers across MainWindow*.cpp (after issue writes, on repo and tab switches) read m_currentIssues/refreshed tables as soon as this returns; it returns early on an unchanged content signature, pumps keep-alive every 100 ms while loading, and the refresh paths without that need (startup restore, sync completion, repo tab counts) use reloadIssuesInBackground instead
void MainWindow::reloadIssues()
{
    if (deferUiRefresh(UiRefreshIssueReload))
        return;
    if (!m_issueTable)
        return;
    ++m_issueLoadGen; // supersede an older worker result
    if (issuesRepoIndex() < 0) {
        m_currentIssues.clear();
        m_currentLabels.clear();
        m_currentMilestones.clear();
        m_issueTable->setRowCount(0);
        // This path skips refreshIssueList, so retire the notice by hand rather
        // than leaving a closed repo showing the last repo's search result.
        updateSearchEmptyNotice(m_issueSearchEmpty, m_issueTable, QString(), false);
        if (m_issueMilestonesTable)
            m_issueMilestonesTable->setRowCount(0);
        if (m_issueLabelsTable)
            m_issueLabelsTable->setRowCount(0);
        m_currentIssueNumber = -1;
        m_issuesLoadedSig.clear(); // force a full reload when a repo reopens
        renderIssueThread(Issue());
        updateIssueActionState();
        updateRepoIssueCount();
        return;
    }
    const IssueStore store = issueStoreForCurrentRepo();
    const QString sig = store.contentSignature();
    if (!sig.isEmpty() && sig == m_issuesLoadedSig)
        return;
    m_issuesLoadedSig = sig;
    m_currentIssues = store.loadAll(nullptr, [] {
        if (keepAliveClock().elapsed() - g_lastKeepAlivePumpMs >= 100)
            pumpKeepAlive();
    });
    m_currentLabels = store.loadLabels();
    m_currentMilestones = store.loadMilestones();

    QSignalBlocker labelBlock(m_issueLabelFilter);
    m_issueLabelFilter->clear();
    m_issueLabelFilter->addItem("All labels", QString());
    for (const IssueLabel &label : m_currentLabels)
        m_issueLabelFilter->addItem(label.name, label.name);
    labelBlock.unblock();

    QSignalBlocker msBlock(m_issueMilestoneFilter);
    m_issueMilestoneFilter->clear();
    m_issueMilestoneFilter->addItem("All milestones", QString());
    for (const IssueMilestone &ms : m_currentMilestones)
        m_issueMilestoneFilter->addItem(ms.title, ms.title);
    msBlock.unblock();

    refreshIssueList();
    refreshIssueMilestones();
    refreshIssueLabels();
    updateIssueActionState();
    updateRepoIssueCount();
    maybeRestoreIssueLooper();
}

void MainWindow::reloadIssuesInBackground()
{
    if (!m_issueTable || issuesRepoIndex() < 0)
        return;
    const int generation = ++m_issueLoadGen;
    if (m_issueBackgroundLoadInFlight) {
        m_issueBackgroundReloadQueued = true;
        return;
    }
    m_issueBackgroundLoadInFlight = true;
    const int repoIndex = issuesRepoIndex();
    const IssueStore store = issueStoreForCurrentRepo();
    const QString oldSignature = m_issuesLoadedSig;
    struct LoadedIssues {
        QString signature;
        QList<Issue> issues;
        QList<IssueLabel> labels;
        QList<IssueMilestone> milestones;
    };
    runOffThread<LoadedIssues>(
        [store, oldSignature] {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("issues"), QStringLiteral("load issue metadata"),
                forkmesh::ActionTelemetry::Execution::Worker);
            LoadedIssues loaded;
            loaded.signature = store.contentSignature();
            if (!loaded.signature.isEmpty() &&
                loaded.signature == oldSignature)
                return loaded;
            loaded.issues = store.loadAll();
            loaded.labels = store.loadLabels();
            loaded.milestones = store.loadMilestones();
            return loaded;
        },
        [this, generation, repoIndex](LoadedIssues loaded) {
            m_issueBackgroundLoadInFlight = false;
            if (generation == m_issueLoadGen &&
                repoIndex == issuesRepoIndex() &&
                (loaded.signature.isEmpty() ||
                 loaded.signature != m_issuesLoadedSig)) {
                applyLoadedIssues(loaded.signature, std::move(loaded.issues),
                                  std::move(loaded.labels),
                                  std::move(loaded.milestones));
            }
            if (m_issueBackgroundReloadQueued) {
                m_issueBackgroundReloadQueued = false;
                reloadIssuesInBackground();
            }
        });
}

void MainWindow::applyLoadedIssues(const QString &signature, QList<Issue> issues,
                                   QList<IssueLabel> labels,
                                   QList<IssueMilestone> milestones)
{
    m_issuesLoadedSig = signature;
    m_currentIssues = std::move(issues);
    m_currentLabels = std::move(labels);
    m_currentMilestones = std::move(milestones);

    QSignalBlocker labelBlock(m_issueLabelFilter);
    m_issueLabelFilter->clear();
    m_issueLabelFilter->addItem(QStringLiteral("All labels"), QString());
    for (const IssueLabel &label : std::as_const(m_currentLabels))
        m_issueLabelFilter->addItem(label.name, label.name);
    labelBlock.unblock();

    QSignalBlocker milestoneBlock(m_issueMilestoneFilter);
    m_issueMilestoneFilter->clear();
    m_issueMilestoneFilter->addItem(QStringLiteral("All milestones"), QString());
    for (const IssueMilestone &milestone : std::as_const(m_currentMilestones))
        m_issueMilestoneFilter->addItem(milestone.title, milestone.title);
    milestoneBlock.unblock();

    refreshIssueList();
    refreshIssueMilestones();
    refreshIssueLabels();
    updateIssueActionState();
    updateRepoIssueCount();
}

void MainWindow::appendCreatedIssue(const IssueStore &store, const Issue &issue)
{
    ++m_issueLoadGen;
    m_currentIssues.append(issue);
    std::sort(m_currentIssues.begin(), m_currentIssues.end(),
              [](const Issue &a, const Issue &b) { return a.number < b.number; });
    // Keep the cheap reload-skip signature in sync with what we just wrote so a
    // later background reload (e.g. from a push notification) doesn't redo
    // this work for nothing.
    const QString sig = store.contentSignature();
    if (!sig.isEmpty())
        m_issuesLoadedSig = sig;
    refreshIssueList();
    updateIssueActionState();
    updateRepoIssueCount();
}

void MainWindow::removeIssueFromCurrentList(int number)
{
    auto it = std::find_if(m_currentIssues.begin(), m_currentIssues.end(),
                           [number](const Issue &issue) {
                               return issue.number == number;
                           });
    if (it == m_currentIssues.end())
        return;

    ++m_issueLoadGen;
    m_currentIssues.erase(it);
    m_issuesLoadedSig.clear();
    if (m_currentIssueNumber == number)
        m_currentIssueNumber = -1;
    refreshIssueList();
    refreshIssueMilestones();
    refreshIssueLabels();
    updateIssueActionState();
    updateRepoIssueCount();
}

#ifdef FORKMESH_WINDOW_TESTS
#ifdef FORKMESH_WINDOW_TESTS
// The summary row's own layout, keyed by rect name. Taken from the delegate's
// row size rather than the view's, so the contract holds whether or not the
// issues page has ever been laid out.
QVariantMap MainWindow::testIssueRowGeometry(int issueNumber) const
{
    QVariantMap out;
    if (!m_issueTable)
        return out;
    for (int r = 0; r < m_issueTable->rowCount(); ++r) {
        const QTableWidgetItem *numItem = m_issueTable->item(r, 0);
        if (!numItem || numItem->data(Qt::UserRole).toInt() != issueNumber)
            continue;
        const QModelIndex idx = m_issueTable->model()->index(r, 1);
        QStyleOptionViewItem option;
        option.initFrom(m_issueTable);
        const QSize hint =
            m_issueTable->itemDelegateForColumn(1)->sizeHint(option, idx);
        const QRect rowRect(0, 0,
                            qMax(hint.width(), m_issueTable->viewport()->width()),
                            hint.height());
        const IssueRowGeometry g =
            issueRowGeometry(rowRect, m_issueTable->font(), idx);
        out.insert(QStringLiteral("row"), rowRect);
        out.insert(QStringLiteral("avatar"), g.avatar);
        out.insert(QStringLiteral("assignee"), g.assignee);
        out.insert(QStringLiteral("votes"), g.votes);
        out.insert(QStringLiteral("title"), g.title);
        out.insert(QStringLiteral("meta"), g.meta);
        break;
    }
    return out;
}
#endif // FORKMESH_WINDOW_TESTS

QList<int> MainWindow::testIssueListNumbers() const
{
    QList<int> numbers;
    if (!m_issueTable)
        return numbers;
    for (int row = 0; row < m_issueTable->rowCount(); ++row) {
        if (const QTableWidgetItem *item = m_issueTable->item(row, 0))
            numbers.append(item->data(Qt::UserRole).toInt());
    }
    return numbers;
}
#endif

void MainWindow::selectIssueListTab(int id)
{
    if (!m_issueListStack)
        return;
    m_issueListStack->setCurrentIndex(id);
    const bool boardMode = id == 3; // the Kanban board
    m_issueSearch->hide();
    // The filter combos are off-screen state now; the top search is the control.
    m_issueLabelFilter->hide();
    m_issueMilestoneFilter->hide();
    m_issueStatusFilter->hide();
    m_issueDetailToggle->hide();
    if (boardMode)
        refreshIssueBoard();
    scheduleNavRecord();
}

namespace {
const QStringList kDefaultBoardColumns = {QStringLiteral("Backlog"),
                                          QStringLiteral("Todo"),
                                          QStringLiteral("In Progress"),
                                          QStringLiteral("Done")};

QString boardStatusLabel(const QString &column)
{
    return QStringLiteral("status:") + column.trimmed().toLower();
}

bool isBoardStatusLabel(const QString &label)
{
    return label.startsWith(QStringLiteral("status:"), Qt::CaseInsensitive);
}
} // namespace

QStringList MainWindow::boardColumns() const
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return kDefaultBoardColumns;
    const RepositoryRecord &repo = m_repositories.at(idx);
    QSettings settings;
    const QString key =
        QStringLiteral("issueBoard/columns/%1/%2").arg(repo.owner, repo.name);
    const QStringList saved = settings.value(key).toStringList();
    QStringList cleaned;
    for (const QString &c : saved) {
        const QString t = c.trimmed();
        if (!t.isEmpty() && !cleaned.contains(t, Qt::CaseInsensitive))
            cleaned << t;
    }
    return cleaned.isEmpty() ? kDefaultBoardColumns : cleaned;
}

void MainWindow::setBoardColumns(const QStringList &cols)
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return;
    QStringList cleaned;
    for (const QString &c : cols) {
        const QString t = c.trimmed();
        if (!t.isEmpty() && !cleaned.contains(t, Qt::CaseInsensitive))
            cleaned << t;
    }
    if (cleaned.size() < 2) {
        setIssueInlineNotice("A board needs at least two columns.", true);
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(idx);
    QSettings settings;
    const QString key =
        QStringLiteral("issueBoard/columns/%1/%2").arg(repo.owner, repo.name);
    settings.setValue(key, cleaned);
    refreshIssueBoard();
}

void MainWindow::editBoardColumns()
{
    bool ok = false;
    const QString current = boardColumns().join(QStringLiteral(", "));
    const QString text = QInputDialog::getText(
        this, tr("Edit board columns"),
        tr("Column names, left to right (comma separated).\nThe last column is the "
           "\"done\" column and maps to closed issues."),
        QLineEdit::Normal, current, &ok);
    if (!ok)
        return;
    const QStringList cols = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
    setBoardColumns(cols);
}

QString MainWindow::issueBoardColumn(const Issue &issue) const
{
    const QStringList cols = boardColumns();
    if (cols.isEmpty())
        return QString();
    if (issue.status == QStringLiteral("closed"))
        return cols.last();
    for (const QString &col : cols) {
        const QString want = boardStatusLabel(col);
        for (const QString &lbl : issue.labels)
            if (lbl.compare(want, Qt::CaseInsensitive) == 0)
                return col;
    }
    return cols.first();
}

void MainWindow::moveIssueToColumn(int number, const QString &column)
{
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        setIssueInlineNotice(
            "This repository is read-only here, so issues can't be moved. Open it "
            "on the owning node to organize the board.",
            true);
        return;
    }
    const Issue *cur = nullptr;
    for (const Issue &i : m_currentIssues)
        if (i.number == number) {
            cur = &i;
            break;
        }
    if (!cur)
        return;
    const QStringList cols = boardColumns();
    if (cols.isEmpty() || issueBoardColumn(*cur).compare(column, Qt::CaseInsensitive) == 0)
        return; // already there (or nothing to move to)
    const bool toDone = column.compare(cols.last(), Qt::CaseInsensitive) == 0;

    QStringList labels;
    for (const QString &lbl : cur->labels)
        if (!isBoardStatusLabel(lbl))
            labels << lbl;
    if (!toDone)
        labels << boardStatusLabel(column);

    QString error;
    if (!store.setLabels(number, labels, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not move the issue." : error,
                             true);
        return;
    }
    const QString wantStatus =
        toDone ? QStringLiteral("closed") : QStringLiteral("open");
    if (cur->status != wantStatus &&
        !store.setStatus(number, wantStatus, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update issue status."
                                             : error,
                             true);
        return;
    }
    propagateRepoUpdate(issuesRepoIndex());
    reloadIssues();
    setIssueInlineNotice(
        QStringLiteral("Moved #%1 to %2.").arg(number).arg(column));
}

QWidget *MainWindow::buildIssueBoard()
{
    auto *wrap = new QWidget;
    auto *outer = new QVBoxLayout(wrap);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(8);

    auto *editCols = new QPushButton("Edit columns");
    editCols->setObjectName("ghostButton");
    editCols->setProperty("buttonSize", "sm");
    editCols->setCursor(Qt::PointingHandCursor);
    editCols->setToolTip("Rename, add or remove the board's status columns");
    setOcticon(editCols, "gear", 16);
    connect(editCols, &QPushButton::clicked, this, &MainWindow::editBoardColumns);
    auto *hint = new QLabel("Drag a card to another column to change its status.");
    hint->setObjectName("statusLine");
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->addWidget(hint);
    bar->addStretch();
    bar->addWidget(editCols);
    outer->addLayout(bar);

    auto *scroll = new QScrollArea;
    scroll->setObjectName("issueBoardScroll");
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *inner = new QWidget;
    m_issueBoardColumns = new QHBoxLayout(inner);
    m_issueBoardColumns->setContentsMargins(2, 2, 2, 2);
    m_issueBoardColumns->setSpacing(10);
    scroll->setWidget(inner);
    outer->addWidget(scroll, 1);

    m_issueBoard = wrap;
    return wrap;
}

// code-health: allow(deep-nested-loop) the columns partition the filtered issues, so columns x cards x labels is one pass over the issues and their few labels
void MainWindow::refreshIssueBoard()
{
    if (!m_issueBoardColumns)
        return;

    while (QLayoutItem *child = m_issueBoardColumns->takeAt(0)) {
        if (QWidget *w = child->widget())
            w->deleteLater();
        delete child;
    }

    const QStringList cols = boardColumns();
    QHash<QString, QString> labelColors;
    for (const IssueLabel &label : std::as_const(m_currentLabels))
        labelColors.insert(label.name, label.color);

    const QString labelFilter =
        m_issueLabelFilter ? m_issueLabelFilter->currentData().toString() : QString();
    const QString msFilter =
        m_issueMilestoneFilter ? m_issueMilestoneFilter->currentData().toString()
                               : QString();
    const QString search =
        m_issueSearch ? m_issueSearch->text().trimmed() : QString();

    QHash<QString, QList<const Issue *>> buckets;
    for (const Issue &issue : std::as_const(m_currentIssues)) {
        if (issue.isDeleted())
            continue; // tombstoned by its author - never a card, never counted
        if (!labelFilter.isEmpty() && !issue.labels.contains(labelFilter))
            continue;
        if (!msFilter.isEmpty() && issue.milestone != msFilter)
            continue;
        if (!search.isEmpty() && !issueMatchesQuery(issue, search))
            continue;
        buckets[issueBoardColumn(issue)].append(&issue);
    }

    const bool writable = issueStoreForCurrentRepo().canWrite();
    for (const QString &col : cols) {
        const QList<const Issue *> items = buckets.value(col);

        auto *column = new QWidget;
        column->setObjectName("issueBoardColumn");
        column->setMinimumWidth(230);
        column->setMaximumWidth(320);
        auto *cl = new QVBoxLayout(column);
        cl->setContentsMargins(8, 8, 8, 8);
        cl->setSpacing(6);

        auto *hdr = new QLabel(QStringLiteral("%1  ·  %2").arg(col).arg(items.size()));
        hdr->setObjectName("issueBoardHeader");
        cl->addWidget(hdr);

        auto *list = new BoardColumnList(col);
        enableHoverRowHighlight(list);
        list->setDragEnabled(writable);
        list->setAcceptDrops(writable);
        if (writable)
            list->onDrop = [this](int number, const QString &target) {
                moveIssueToColumn(number, target);
            };
        for (const Issue *ip : items) {
            const Issue &issue = *ip;
            QString text = QStringLiteral("#%1  %2").arg(issue.number).arg(issue.title);
            QStringList shownLabels;
            for (const QString &lbl : issue.labels)
                if (!isBoardStatusLabel(lbl))
                    shownLabels << lbl;
            if (!shownLabels.isEmpty())
                text += QStringLiteral("\n") + shownLabels.join(QStringLiteral(", "));
            auto *item = new QListWidgetItem(text);
            item->setData(Qt::UserRole, issue.number);

            QStringList tip;
            tip << QStringLiteral("#%1  %2").arg(issue.number).arg(issue.title);
            if (issue.priority > 0)
                tip << QStringLiteral("Priority %1").arg(issue.priority);
            if (issue.progress > 0)
                tip << QStringLiteral("%1%% complete").arg(issue.progress);
            if (!issue.milestone.isEmpty())
                tip << QStringLiteral("Milestone: %1").arg(issue.milestone);
            if (issue.bountyUsd > 0)
                tip << QStringLiteral("Bounty $%1")
                           .arg(QString::number(issue.bountyUsd, 'f', 2));
            if (writable)
                tip << QStringLiteral("Drag to another column to change status");
            item->setToolTip(tip.join(QStringLiteral("\n")));
            if (issue.status == QStringLiteral("closed"))
                item->setForeground(Theme::remapColor(QColor("#8b949e")));
            list->addItem(item);
        }
        connect(list, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *it) {
                    if (it)
                        showIssue(it->data(Qt::UserRole).toInt());
                });
        cl->addWidget(list, 1);
        m_issueBoardColumns->addWidget(column);
    }
    m_issueBoardColumns->addStretch();
}

namespace {
const char *kAgentSpinFrames[] = {"\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9",
                                  "\xE2\xA0\xB8", "\xE2\xA0\xBC", "\xE2\xA0\xB4",
                                  "\xE2\xA0\xA6", "\xE2\xA0\xA7", "\xE2\xA0\x87",
                                  "\xE2\xA0\x8F"};
} // namespace

void MainWindow::resetIssueFilters()
{
    if (!m_issueStatusFilter)
        return;
    bool changed = false;
    {
        QSignalBlocker statusBlock(m_issueStatusFilter);
        QSignalBlocker labelBlock(m_issueLabelFilter);
        QSignalBlocker msBlock(m_issueMilestoneFilter);
        if (m_issueStatusFilter->currentIndex() != 0) {
            m_issueStatusFilter->setCurrentIndex(0); // "Open"
            changed = true;
        }
        if (m_issueLabelFilter->currentIndex() != 0) {
            m_issueLabelFilter->setCurrentIndex(0); // "All labels"
            changed = true;
        }
        if (m_issueMilestoneFilter->currentIndex() != 0) {
            m_issueMilestoneFilter->setCurrentIndex(0); // "All milestones"
            changed = true;
        }
    }
    if (m_issueSearch && !m_issueSearch->text().isEmpty()) {
        QSignalBlocker searchBlock(m_issueSearch);
        m_issueSearch->clear();
        changed = true;
    }
    if (changed)
        refreshIssueList();
}

void MainWindow::refreshIssueList()
{
    if (deferUiRefresh(UiRefreshIssues))
        return;
    if (!m_issueTable)
        return;
    const QString statusFilter = m_issueStatusFilter->currentText();
    const QString labelFilter = m_issueLabelFilter->currentData().toString();
    const QString msFilter = m_issueMilestoneFilter->currentData().toString();
    const QString search =
        m_issueSearch ? m_issueSearch->text().trimmed() : QString();
    const bool queryOwnsStatus = issueQuerySetsStatus(search);
    const int selectNext = m_selectIssueOnReload;
    m_selectIssueOnReload = -1;
    const int keep = selectNext > 0 ? selectNext : m_currentIssueNumber;
    const bool keepCurrent = m_keepCurrentOnReload;
    m_keepCurrentOnReload = false;
    const bool detailWasOpen = true;

    TableRepaintGuard repaintGuard(m_issueTable);
    m_issueTable->blockSignals(true);
    m_issueTable->setSortingEnabled(false);
    QList<const Issue *> visible;
    visible.reserve(m_currentIssues.size());
    for (const Issue &issue : m_currentIssues) {
        if (!queryOwnsStatus && statusFilter == "Open" && issue.status != "open")
            continue;
        if (!queryOwnsStatus && statusFilter == "Closed" &&
            issue.status != "closed")
            continue;
        if (!labelFilter.isEmpty() && !issue.labels.contains(labelFilter))
            continue;
        if (!msFilter.isEmpty() && issue.milestone != msFilter)
            continue;
        if (issue.isDeleted() && statusFilter != "All" && !queryOwnsStatus)
            continue;
        if (!search.isEmpty() && !issueMatchesQuery(issue, search))
            continue;
        visible.append(&issue);
    }

    int openCount = 0;
    int closedCount = 0;
    int unprioritizedCount = 0;
    for (const Issue &issue : std::as_const(m_currentIssues)) {
        if (issue.isDeleted())
            continue;
        if (issue.status == QLatin1String("closed"))
            ++closedCount;
        else {
            ++openCount;
            if (issue.priority <= 0)
                ++unprioritizedCount;
        }
    }
    auto badge = [](QAbstractButton *button, qint64 count) {
        if (auto *tile = dynamic_cast<VerticalIconButton *>(button))
            tile->setBadgeCount(count);
    };
    if (m_issueTabGroup) {
        badge(m_issueTabGroup->button(0), visible.size());
        badge(m_issueTabGroup->button(1), m_currentMilestones.size());
        badge(m_issueTabGroup->button(2), m_currentLabels.size());
        badge(m_issueTabGroup->button(3), m_currentIssues.size());
    }
    badge(m_issuePrioritizeButton, openCount);
    badge(m_issueCompletenessButton, openCount);
    badge(findChild<QPushButton *>("issueBurnupAction"), closedCount);
    badge(findChild<QPushButton *>("issueReprioritizeAction"),
          unprioritizedCount);

    // The generated portraits are drawn from scratch on every call, and most
    // rows share a handful of reporters, so each face is built once per fill.
    QHash<QString, QPixmap> rowFaces;
    const auto rowFace = [&](const QString &authorId, const QString &name,
                             int side) {
        const QString key = authorId + QLatin1Char('\n') + name +
                            QLatin1Char('\n') + QString::number(side);
        auto it = rowFaces.constFind(key);
        if (it == rowFaces.constEnd())
            it = rowFaces.insert(key, issueTimelineAvatar(authorId, name, side));
        return it.value();
    };
    QHash<QString, QString> labelColorByName;
    for (const IssueLabel &label : std::as_const(m_currentLabels))
        labelColorByName.insert(label.name, label.color);

    m_issueTable->setRowCount(visible.size());
    for (int row = 0; row < visible.size(); ++row) {
        const Issue &issue = *visible.at(row);

        auto *numItem = new QTableWidgetItem;
        numItem->setData(Qt::DisplayRole, issue.number);
        numItem->setData(Qt::UserRole, issue.number); // lookup key
        m_issueTable->setItem(row, 0, numItem);
        auto *titleItem = new QTableWidgetItem(issue.title);
        if (!issue.assignees.isEmpty()) {
            titleItem->setIcon(issueAssigneeAvatar(issue.assignees.first()));
            titleItem->setToolTip(
                QStringLiteral("Assigned to %1").arg(issue.assignees.join(", ")));
        }
        m_issueTable->setItem(row, 1, titleItem);
        m_issueTable->setRowHeight(row, 58);

        auto *priority = new SortTableWidgetItem(
            issue.priority > 0 ? QString::number(issue.priority)
                               : QString::fromUtf8("\xE2\x80\x94"));
        priority->setData(kTableSortRole,
                          issue.priority > 0 ? issue.priority : 9000);
        priority->setTextAlignment(Qt::AlignCenter);
        priority->setToolTip("1 is highest priority; 8999 is lowest");
        m_issueTable->setItem(row, 2, priority);

        QString statusText = issue.status == "closed" ? "Closed" : "Open";
        QColor statusColor(issue.status == "closed" ? "#f85149" : "#3fb950");
        QString statusTip;
        if (issue.isDeleted()) {
            statusText = QStringLiteral("Deleted");
            statusColor = Theme::remapColor(QColor("#8b949e"));
            statusTip = QStringLiteral("Deleted by its author.");
        } else if (issue.hasUnauthorizedDeleteAttempt()) {
            statusText += QStringLiteral(" ⚠");
            statusColor = QColor("#d29922");
            statusTip = QStringLiteral(
                "Someone who did not open this issue tried to delete it; the "
                "deletion was not applied.");
        }
        auto *status = new QTableWidgetItem(statusText);
        status->setForeground(statusColor);
        if (!statusTip.isEmpty())
            status->setToolTip(statusTip);
        m_issueTable->setItem(row, 3, status);
        auto *votes = new QTableWidgetItem;
        votes->setData(Qt::DisplayRole, issue.votes); // numeric sort
        votes->setTextAlignment(Qt::AlignCenter);
        m_issueTable->setItem(row, 4, votes);
        m_issueTable->setItem(row, 5, new QTableWidgetItem(issue.labels.join(", ")));
        m_issueTable->setItem(row, 6, new QTableWidgetItem(issue.milestone));
        auto *created = new QTableWidgetItem(
            issue.createdAt > 0
                ? QDateTime::fromMSecsSinceEpoch(issueDisplayTimestampMs(issue.createdAt))
                      .toString("yyyy-MM-dd HH:mm")
                : QString());
        created->setToolTip(formatIssueRelativeTime(issue.createdAt));
        m_issueTable->setItem(row, 7, created);

        // Updated date/time: the most recent activity on the issue (latest signed
        // event, falling back to the created time). ISO yyyy-MM-dd HH:mm sorts
        // chronologically as plain text; the tooltip carries the "x ago" form.
        qint64 updatedAt = issue.createdAt;
        for (const IssueEvent &ev : issue.events)
            updatedAt = qMax(updatedAt, ev.ts);
        auto *updated = new QTableWidgetItem(
            updatedAt > 0
                ? QDateTime::fromMSecsSinceEpoch(issueDisplayTimestampMs(updatedAt))
                      .toString("yyyy-MM-dd HH:mm")
                : QString());
        updated->setToolTip(formatIssueRelativeTime(updatedAt));
        m_issueTable->setItem(row, 8, updated);

        QString agentText;
        bool agentWorking = false;
        if (const AgentSession *session = latestAgentSessionForIssue(issue.number)) {
            QString text = agentProviderName(session->provider);
            const bool working = agentSessionActive(session);
            agentWorking = working;
            if (working)
                text = QString::fromUtf8(kAgentSpinFrames[m_issueSpinFrame % 10]) +
                       QStringLiteral(" ") + text;
            agentText = text;
            auto *agentItem = new QTableWidgetItem(text);
            if (working)
                agentItem->setForeground(
                    Theme::nextOr(Theme::Role::AccentFg, QColor(Theme::kRunning)));
            agentItem->setData(Qt::UserRole, session->id);
            m_issueTable->setItem(row, 9, agentItem);
        } else {
            m_issueTable->setItem(row, 9, new QTableWidgetItem(QString()));
        }
        const QString author =
            issue.authorName.trimmed().isEmpty()
                ? (issue.author.isEmpty() ? QString::fromUtf8("\xE2\x80\x94")
                                          : issue.author.left(8))
                : issue.authorName.trimmed();
        auto *authorItem = new QTableWidgetItem(author);
        authorItem->setToolTip(issue.author);
        m_issueTable->setItem(row, 10, authorItem);

        const int pct = qBound(0, issue.progress, 100);
        auto *progressItem = new SortTableWidgetItem(QString());
        progressItem->setData(kTableSortRole, pct);
        progressItem->setData(kProgressBarRole, pct);
        progressItem->setToolTip(QStringLiteral("%1% complete").arg(pct));
        m_issueTable->setItem(row, 11, progressItem);

        const double estUsd = openAiEstimateUsd(issue);
        auto *estItem = new SortTableWidgetItem(
            QStringLiteral("$%1").arg(QString::number(estUsd, 'f', 2)));
        estItem->setData(kTableSortRole, estUsd);
        estItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_issueTable->setItem(row, 12, estItem);

        auto *bountyItem = new SortTableWidgetItem(
            issue.bountyUsd > 0
                ? QStringLiteral("$%1").arg(QString::number(issue.bountyUsd, 'f', 2))
                : QString::fromUtf8("\xE2\x80\x94"));
        bountyItem->setData(kTableSortRole, issue.bountyUsd);
        bountyItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (!issue.bountyStatus.isEmpty())
            bountyItem->setToolTip(
                QStringLiteral("Legacy bounty (migration-only), historical "
                               "status: %1")
                    .arg(issue.bountyStatus));
        m_issueTable->setItem(row, 13, bountyItem);

        QSet<QString> deletedComments;
        for (const IssueEvent &ev : issue.events) {
            if (ev.type == "delete" && !ev.target.isEmpty() && ev.target != "self")
                deletedComments.insert(ev.target);
        }
        int commentCount = 0;
        qint64 latestCommentTs = -1;
        QString latestCommenter;
        for (const IssueEvent &ev : issue.events) {
            if (ev.type != "comment" || deletedComments.contains(ev.id))
                continue;
            ++commentCount;
            if (ev.ts >= latestCommentTs) {
                latestCommentTs = ev.ts;
                latestCommenter = ev.authorName.trimmed().isEmpty()
                                      ? ev.author.left(8)
                                      : ev.authorName.trimmed();
            }
        }
        auto *commentsItem = new SortTableWidgetItem(
            commentCount > 0 && !latestCommenter.isEmpty()
                ? QString::fromUtf8("%1 \xC2\xB7 %2")
                      .arg(commentCount)
                      .arg(latestCommenter)
                : QString::number(commentCount));
        commentsItem->setData(kTableSortRole, commentCount);
        if (!latestCommenter.isEmpty())
            commentsItem->setToolTip(
                QStringLiteral("Latest comment by %1").arg(latestCommenter));
        commentsItem->setTextAlignment(Qt::AlignCenter);
        m_issueTable->setItem(row, 14, commentsItem);

        populateIssueFilesCell(row, issue);

        m_issueTable->setItem(
            row, 16,
            new QTableWidgetItem(issue.assignees.isEmpty()
                                     ? QString::fromUtf8("\xE2\x80\x94")
                                     : issue.assignees.join(QStringLiteral(", "))));

        QStringList colors;
        for (const QString &label : issue.labels)
            colors << labelColorByName.value(label, QStringLiteral("#64748B"));
        titleItem->setData(Qt::UserRole, issue.number);
        titleItem->setData(IssueStatusRole,
                           issue.isDeleted() ? QStringLiteral("deleted")
                                             : issue.status);
        titleItem->setData(IssuePriorityRole, issue.priority);
        titleItem->setData(IssueVotesRole, issue.votes);
        titleItem->setData(IssueLabelsRole, issue.labels);
        titleItem->setData(IssueLabelColorsRole, colors);
        titleItem->setData(IssueMilestoneRole, issue.milestone);
        titleItem->setData(IssueCreatedRole, compactIssueAge(issue.createdAt));
        titleItem->setData(IssueAuthorRole, author);
        titleItem->setData(IssueAssigneesRole, issue.assignees);
        // Same faces the issue's timeline shows, so the list stops falling
        // back to initials for people who have a real picture.
        if (!issue.author.isEmpty() || !issue.authorName.trimmed().isEmpty())
            titleItem->setData(IssueAuthorAvatarRole,
                               rowFace(issue.author, issue.authorName, 36));
        QStringList badgeAssignees = issue.assignees;
        badgeAssignees.removeAll(author); // as issueRowAssignees() draws it
        if (!badgeAssignees.isEmpty())
            titleItem->setData(IssueAssigneeAvatarRole,
                               rowFace(QString(), badgeAssignees.constFirst(),
                                       kIssueAssigneeBadge));
        titleItem->setData(IssueProgressRole, pct);
        const QTableWidgetItem *filesItem = m_issueTable->item(row, 15);
        titleItem->setData(IssueFilesRole,
                           filesItem ? filesItem->data(kTableSortRole).toInt() : 0);
        titleItem->setData(IssueAttachmentRole,
                           filesItem ? filesItem->data(Qt::UserRole) : QVariant());
        titleItem->setData(IssueAgentRole, agentText);
        titleItem->setData(IssueAgentActiveRole, agentWorking);
        titleItem->setData(IssueAgentAngleRole, m_issueSpinAngle);
        QStringList tooltip{
            QStringLiteral("#%1 — %2").arg(issue.number).arg(issue.title)};
        if (issue.priority > 0)
            tooltip << QStringLiteral("Priority %1 · %2 · %3")
                           .arg(issue.priority)
                           .arg(issuePriorityMeaning(issue.priority),
                                issuePriorityColor(issue.priority).name());
        if (!issue.labels.isEmpty())
            tooltip << QStringLiteral("Labels: %1").arg(issue.labels.join(", "));
        if (!issue.milestone.isEmpty())
            tooltip << QStringLiteral("Milestone: %1").arg(issue.milestone);
        // Only the first assignee fits in the badge on the avatar, so the rest
        // are readable from the row's tooltip.
        if (!issue.assignees.isEmpty())
            tooltip << QStringLiteral("Assigned to: %1")
                           .arg(issue.assignees.join(QStringLiteral(", ")));
        titleItem->setToolTip(tooltip.join(QLatin1Char('\n')));
    }
    m_issueTable->setSortingEnabled(true);
    m_issueTable->blockSignals(false);

    // selectRow() only emits itemSelectionChanged when the selected row index
    // moves. After a refill the same index can hold a different issue (the
    // shown one was filtered out and the next one slid into its row), so the
    // detail pane is synced to the row directly instead of via the signal.
    const auto selectIssueRow = [this](int row) {
        m_issueTable->selectRow(row);
        const int number = m_issueTable->item(row, 0)->data(Qt::UserRole).toInt();
        if (number != m_currentIssueNumber)
            showIssue(number);
    };
    int selRow = -1;
    for (int r = 0; r < m_issueTable->rowCount(); ++r) {
        if (m_issueTable->item(r, 0)->data(Qt::UserRole).toInt() == keep) {
            selRow = r;
            break;
        }
    }
    if (selRow >= 0) {
        selectIssueRow(selRow);
    } else if (keepCurrent && detailWasOpen && keep > 0) {
        bool stillThere = false;
        for (const Issue &issue : m_currentIssues) {
            if (issue.number == keep) {
                m_currentIssueNumber = keep;
                renderIssueThread(issue);
                updateIssueActionState();
                stillThere = true;
                break;
            }
        }
        if (!stillThere) {
            m_issueTable->clearSelection();
            m_currentIssueNumber = -1;
            renderIssueThread(Issue());
            updateIssueActionState();
        }
    } else {
        if (m_issueTable->rowCount() > 0) {
            selectIssueRow(0);
        } else {
            m_issueTable->clearSelection();
            m_currentIssueNumber = -1;
            renderIssueThread(Issue());
            updateIssueActionState();
        }
    }

    bool anyActive = false;
    for (const Issue &issue : m_currentIssues) {
        if (agentSessionActive(latestAgentSessionForIssue(issue.number))) {
            anyActive = true;
            break;
        }
    }
    if (anyActive) {
        if (!m_issueSpinTimer) {
            m_issueSpinTimer = new QTimer(this);
            connect(m_issueSpinTimer, &QTimer::timeout, this,
                    &MainWindow::tickIssueListSpinners);
        }
        if (!m_issueSpinTimer->isActive())
            m_issueSpinTimer->start(110);
    } else if (m_issueSpinTimer) {
        m_issueSpinTimer->stop();
    }

    // Only the search term earns the notice. A status/label/milestone filter
    // that empties the table leaves its own combo boxes on screen saying why,
    // and "(clear search)" would not put those rows back anyway.
    updateSearchEmptyNotice(m_issueSearchEmpty, m_issueTable, search,
                            visible.isEmpty());

    if (m_issueListStack && m_issueListStack->currentIndex() == 3)
        refreshIssueBoard();
}

void MainWindow::tickIssueListSpinners()
{
    if (!m_issueTable)
        return;
    // Same idea as the agent/action spinners: no per-row text and role
    // writes while the issue list is not on screen.
    if (!m_issueTable->isVisible()) {
        if (m_issueSpinTimer && m_issueSpinTimer->interval() != kAgentSpinHiddenTickMs)
            m_issueSpinTimer->setInterval(kAgentSpinHiddenTickMs);
        return;
    }
    if (m_issueSpinTimer && m_issueSpinTimer->interval() != 110)
        m_issueSpinTimer->setInterval(110);
    m_issueSpinFrame = (m_issueSpinFrame + 1) % 10;
    m_issueSpinAngle =
        std::fmod(m_issueSpinAngle + kIssueFireballStepDegrees, 360.0);
    const QString frame = QString::fromUtf8(kAgentSpinFrames[m_issueSpinFrame]);
    bool anyActive = false;
    for (int r = 0; r < m_issueTable->rowCount(); ++r) {
        QTableWidgetItem *numItem = m_issueTable->item(r, 0);
        QTableWidgetItem *cell = m_issueTable->item(r, 9);
        if (!numItem || !cell)
            continue;
        const AgentSession *session =
            latestAgentSessionForIssue(numItem->data(Qt::UserRole).toInt());
        QTableWidgetItem *summary = m_issueTable->item(r, 1);
        if (!agentSessionActive(session)) {
            // Nothing rebuilds the row until the next reload, so a session that
            // has just stopped would leave its fireball frozen mid-orbit.
            if (summary && summary->data(IssueAgentActiveRole).toBool())
                summary->setData(IssueAgentActiveRole, false);
            continue;
        }
        anyActive = true;
        cell->setText(frame + QStringLiteral(" ") +
                      agentProviderName(session->provider));
        // Carrying the head forward repaints the summary cell, which is what
        // keeps the fireball orbiting the row's assignee badge.
        if (summary)
            summary->setData(IssueAgentAngleRole, m_issueSpinAngle);
    }
    if (!anyActive && m_issueSpinTimer)
        m_issueSpinTimer->stop();
}

void MainWindow::refreshIssueMilestones()
{
    if (!m_issueMilestonesTable)
        return;

    struct Counts {
        int open = 0;
        int closed = 0;
    };
    QHash<QString, Counts> counts;
    QHash<QString, IssueMilestone> defs;
    QStringList order;
    for (const IssueMilestone &ms : std::as_const(m_currentMilestones)) {
        if (ms.title.trimmed().isEmpty())
            continue;
        defs.insert(ms.title, ms);
        if (!order.contains(ms.title))
            order << ms.title;
    }
    for (const Issue &issue : std::as_const(m_currentIssues)) {
        if (issue.isDeleted())
            continue;
        if (issue.milestone.trimmed().isEmpty())
            continue;
        if (!order.contains(issue.milestone))
            order << issue.milestone;
        Counts &c = counts[issue.milestone];
        if (issue.status == "closed")
            ++c.closed;
        else
            ++c.open;
    }

    // The progress bars share one sheet on the table, set once, rather than a
    // sheet per bar per row; a finished milestone's bar is picked out by its
    // milestoneDone property. A parent's sheet still outranks the application
    // sheet, so the bars look as they did.
    const bool dark = currentThemeIsDark();
    const QString progressSheet =
        QStringLiteral("QProgressBar {"
                       "  border: none;"
                       "  border-radius: 6px;"
                       "  background-color: %1;"
                       "  color: %2;"
                       "  text-align: center;"
                       "  font-size: 11px;"
                       "  min-height: 14px;"
                       "  max-height: 16px;"
                       "}"
                       "QProgressBar::chunk {"
                       "  border-radius: 6px;"
                       "  background-color: %3;"
                       "}"
                       "QProgressBar[milestoneDone=\"true\"]::chunk {"
                       "  background-color: #3fb950;"
                       "}")
            .arg(dark ? QStringLiteral("#30363d") : QStringLiteral("#d0d7de"),
                 dark ? QStringLiteral("#e6edf3") : QStringLiteral("#1f2328"),
                 Theme::nextOrHex(Theme::Role::Accent, QStringLiteral("#388bfd")));
    if (m_issueMilestonesTable->styleSheet() != progressSheet)
        m_issueMilestonesTable->setStyleSheet(Theme::remapSheet(progressSheet));

    TableRepaintGuard repaintGuard(m_issueMilestonesTable);
    m_issueMilestonesTable->setSortingEnabled(false);
    m_issueMilestonesTable->setRowCount(0);
    for (const QString &title : std::as_const(order)) {
        const Counts c = counts.value(title);
        const int total = c.open + c.closed;
        const int pct = total > 0 ? (c.closed * 100) / total : 0;
        const IssueMilestone ms = defs.value(title);

        const int row = m_issueMilestonesTable->rowCount();
        m_issueMilestonesTable->insertRow(row);
        auto *titleItem = new QTableWidgetItem(title);
        titleItem->setIcon(themedOcticon("graph", Theme::remapColor(QColor("#8b949e")), 14));
        m_issueMilestonesTable->setItem(row, 0, titleItem);
        auto addNumber = [&](int column, int value, const QString &which) {
            auto *item = new QTableWidgetItem;
            item->setData(Qt::DisplayRole, value);
            item->setTextAlignment(Qt::AlignCenter);
            item->setForeground(
                Theme::nextOr(Theme::Role::AccentFg, QColor("#388bfd")));
            item->setToolTip(
                QStringLiteral("Show %1 %2 issues").arg(which, title));
            m_issueMilestonesTable->setItem(row, column, item);
        };
        addNumber(1, c.open, QStringLiteral("open"));
        addNumber(2, c.closed, QStringLiteral("closed"));

        auto *progressItem = new QTableWidgetItem(QStringLiteral("%1%").arg(pct));
        progressItem->setData(kTableSortRole, pct);
        m_issueMilestonesTable->setItem(row, 3, progressItem);
        auto *progress = new QProgressBar;
        progress->setRange(0, 100);
        progress->setValue(pct);
        progress->setTextVisible(true);
        progress->setFormat(QStringLiteral("%p%"));
        progress->setProperty("milestoneDone", pct >= 100);
        m_issueMilestonesTable->setCellWidget(row, 3, progress);

        m_issueMilestonesTable->setItem(
            row, 4,
            new QTableWidgetItem(
                ms.due > 0 ? QDateTime::fromMSecsSinceEpoch(ms.due).toString("yyyy-MM-dd")
                           : QString()));
        m_issueMilestonesTable->setItem(
            row, 5,
            new QTableWidgetItem(ms.status.trimmed().isEmpty() ? QStringLiteral("open")
                                                               : ms.status));
    }
    m_issueMilestonesTable->setSortingEnabled(true);
}

void MainWindow::refreshIssueLabels()
{
    if (!m_issueLabelsTable)
        return;

    struct Counts {
        int open = 0;
        int closed = 0;
    };
    QHash<QString, Counts> counts;
    QHash<QString, QString> colors;
    QStringList order;
    for (const IssueLabel &label : std::as_const(m_currentLabels)) {
        if (label.name.trimmed().isEmpty())
            continue;
        colors.insert(label.name, label.color);
        if (!order.contains(label.name))
            order << label.name;
    }
    for (const Issue &issue : std::as_const(m_currentIssues)) {
        if (issue.isDeleted())
            continue;
        for (const QString &name : issue.labels) {
            if (name.trimmed().isEmpty())
                continue;
            if (!order.contains(name))
                order << name;
            Counts &c = counts[name];
            if (issue.status == "closed")
                ++c.closed;
            else
                ++c.open;
        }
    }

    const bool writable = issueStoreForCurrentRepo().canWrite();
    TableRepaintGuard repaintGuard(m_issueLabelsTable);
    m_issueLabelsTable->setSortingEnabled(false);
    m_issueLabelsTable->setRowCount(0);
    for (const QString &name : std::as_const(order)) {
        const Counts c = counts.value(name);
        const QString color = colors.value(name, QStringLiteral("#94a3b8"));
        const int row = m_issueLabelsTable->rowCount();
        m_issueLabelsTable->insertRow(row);

        auto *labelItem = new QTableWidgetItem(name);
        labelItem->setData(Qt::UserRole, name);
        labelItem->setIcon(themedOcticon("tag", QColor(color), 14));
        m_issueLabelsTable->setItem(row, 0, labelItem);
        auto addNumber = [&](int column, int value) {
            auto *item = new QTableWidgetItem;
            item->setData(Qt::DisplayRole, value);
            item->setTextAlignment(Qt::AlignCenter);
            m_issueLabelsTable->setItem(row, column, item);
        };
        addNumber(1, c.open);
        addNumber(2, c.closed);
        addNumber(3, c.open + c.closed);

        auto *edit = new QPushButton("Edit");
        edit->setObjectName("ghostButton");
        edit->setProperty("buttonSize", "sm");
        edit->setEnabled(writable);
        edit->setCursor(Qt::PointingHandCursor);
        setOcticon(edit, "pencil", 14);
        connect(edit, &QPushButton::clicked, this, [this, name] {
            for (int r = 0; r < m_issueLabelsTable->rowCount(); ++r) {
                QTableWidgetItem *current = m_issueLabelsTable->item(r, 0);
                if (current && current->data(Qt::UserRole).toString() == name) {
                    editIssueLabelDefinition(r);
                    return;
                }
            }
        });
        m_issueLabelsTable->setCellWidget(row, 4, edit);
    }
    m_issueLabelsTable->setSortingEnabled(true);
}

void MainWindow::editIssueLabelDefinition(int row)
{
    if (!m_issueLabelsTable || row < 0 || row >= m_issueLabelsTable->rowCount())
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        setIssueInlineNotice("Labels are editable on the repository host.", true);
        return;
    }
    QTableWidgetItem *item = m_issueLabelsTable->item(row, 0);
    if (!item)
        return;
    const QString oldName = item->data(Qt::UserRole).toString();
    QString oldColor = QStringLiteral("#94a3b8");
    for (const IssueLabel &label : std::as_const(m_currentLabels))
        if (label.name == oldName && !label.color.trimmed().isEmpty())
            oldColor = label.color.trimmed();

    bool ok = false;
    const QString newName =
        QInputDialog::getText(this, "Edit label", "Label name:", QLineEdit::Normal,
                              oldName, &ok)
            .trimmed();
    if (!ok)
        return;
    if (newName.isEmpty()) {
        setIssueInlineNotice("A label name is required.", true);
        return;
    }
    const QString newColor =
        QInputDialog::getText(this, "Edit label", "Color (#RRGGBB):",
                              QLineEdit::Normal, oldColor, &ok)
            .trimmed();
    if (!ok)
        return;
    if (!QColor(newColor).isValid()) {
        setIssueInlineNotice("Use a valid label color, for example #3fb950.", true);
        return;
    }

    QList<IssueLabel> labels = m_currentLabels;
    bool updated = false;
    for (IssueLabel &label : labels) {
        if (label.name == oldName) {
            label.name = newName;
            label.color = newColor;
            updated = true;
        } else if (label.name == newName) {
            setIssueInlineNotice("That label already exists.", true);
            return;
        }
    }
    if (!updated)
        labels.append({newName, newColor});

    QString error;
    if (newName != oldName) {
        for (const Issue &issue : std::as_const(m_currentIssues)) {
            if (!issue.labels.contains(oldName))
                continue;
            QStringList issueLabels = issue.labels;
            issueLabels.replaceInStrings(QRegularExpression(
                                             QStringLiteral("^%1$")
                                                 .arg(QRegularExpression::escape(oldName))),
                                         newName);
            issueLabels.removeDuplicates();
            if (!store.setLabels(issue.number, issueLabels, &error)) {
                setIssueInlineNotice(error.isEmpty() ? "Could not rename label." : error,
                                     true);
                return;
            }
        }
    }
    if (!store.saveLabels(labels, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not save labels." : error, true);
        return;
    }
    setIssueInlineNotice("Label updated.");
    reloadIssues();
}

void MainWindow::showIssue(int number)
{
    for (const Issue &issue : m_currentIssues) {
        if (issue.number == number) {
            // Repository tabs are built lazily. A restored/deep-linked issue
            // can arrive before the Issues tab owns its detail widgets; retain
            // the selection for that later build instead of dereferencing the
            // not-yet-created thread layout.
            if (!m_issueThreadLayout || !m_issueTitle) {
                m_selectIssueOnReload = number;
                return;
            }
            removeIssueComposePage();
            m_issueDeleteConfirmPending = false;
            m_currentIssueNumber = number;
            if (m_issueDetail && !m_issueDetail->isVisible()) {
                m_issueDetail->show();
                m_issueDetail->raise();
                if (m_issueDetailToggle)
                    m_issueDetailToggle->setText("Hide detail");
            }
            renderIssueThread(issue);
            updateIssueActionState();
            scheduleNavRecord();
            return;
        }
    }
}

namespace {

// Empties a timeline card body before it is drawn or swapped for the editor.
void clearIssueCardBody(QVBoxLayout *bodyLayout)
{
    while (QLayoutItem *item = bodyLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete item;
    }
}

} // namespace

// What renderIssueThread() works out once and every card it draws reads.
struct MainWindow::IssueThreadCardContext {
    QHash<QString, IssueEvent> edits; // target event id -> its latest edit
    QString imageBase;
    bool haveLocalFiles = false;
    bool writable = false;
    QString myKey;
    QString myName;
};

void MainWindow::renderIssueThread(const Issue &issue)
{
    if (!m_issueThreadLayout || !m_issueTitle)
        return;
    if (m_issueAiTypingTimer)
        m_issueAiTypingTimer->stop();
    m_issueAiTypingTimer = nullptr;
    m_issueAiTypingRow = nullptr;

    while (QLayoutItem *item = m_issueThreadLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            if (w == m_issueComposerRow)
                w->hide();
            else
                w->deleteLater();
        }
        delete item;
    }

    auto clearLabelPills = [this] {
        if (!m_issueLabelsPillsLayout)
            return;
        while (m_issueLabelsPillsLayout->count() > 1) {
            QLayoutItem *item = m_issueLabelsPillsLayout->takeAt(1);
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        if (m_issueLabelsValue)
            m_issueLabelsValue->show();
    };
    clearLabelPills();

    if (issue.number == 0) {
        renderNoIssueSelected();
        return;
    }

    m_issueTitle->setText(
        QStringLiteral("%1 <span style='color:#656d76;font-weight:400'>#%2</span>")
            .arg(issue.title.toHtmlEscaped())
            .arg(issue.number));
    if (m_issueTitleEditor)
        m_issueTitleEditor->setText(issue.title);

    renderIssueParentRefs(issue);
    renderIssueSidebarValues(issue);

    IssueThreadCardContext context;
    QHash<QString, IssueEvent> &edits = context.edits;
    QSet<QString> deleted;
    // Taking a reaction back is a delete event naming it. Those are not
    // "deleted a comment" and don't belong in the activity feed at all.
    QSet<QString> reactionIds;
    QString openId;
    for (const IssueEvent &ev : issue.events) {
        if (ev.type == "open")
            openId = ev.id;
        else if (ev.type == "reaction")
            reactionIds.insert(ev.id);
        else if (ev.type == "edit" && !ev.target.isEmpty())
            edits.insert(ev.target, ev); // later edits overwrite
        else if (ev.type == "delete" && !ev.target.isEmpty() && ev.target != "self")
            deleted.insert(ev.target);
    }

    const int idx = issuesRepoIndex();
    const QString &imageBase = context.imageBase =
        idx >= 0 ? IssueStore::issueDirPath(m_repositories.at(idx).localPath,
                                           issue.number) +
                       "/"
                 : QString();
    context.haveLocalFiles = !imageBase.isEmpty() &&
                             QFileInfo::exists(
                                 imageBase + QStringLiteral("issue-%1.json")
                                                 .arg(issue.number));
    const bool writable = context.writable =
        issueStoreForCurrentRepo().canWrite();
    // Whose reaction chips light up as already-mine. Both halves matter: this
    // node also signs for the model bots, which react under their own names.
    context.myKey = m_profileIdentity.publicKey();
    context.myName = chatDisplayName();

    auto addCard = [&](const IssueEvent &ev, bool isOpen) {
        addIssueThreadCard(issue, ev, isOpen, context);
    };
    // `html` is already escaped; activity links name another issue.
    auto addActivityHtml = [&](const IssueEvent &ev, const QString &html) {
        addIssueActivityRow(ev, html);
    };
    auto addActivity = [&](const IssueEvent &ev, const QString &text) {
        addActivityHtml(ev, text.toHtmlEscaped());
    };

    auto addSubIssues = [&]() { addIssueSubIssuesSection(issue, writable); };

    // Other issues referencing this one join the history at the moment they
    // were written, so the log reads the way it happened.
    const QList<IssueMention> mentions =
        IssueStore::mentionsOf(m_currentIssues, issue.number);
    int nextMention = 0;
    auto addMentionsBefore = [&](qint64 ts) {
        while (nextMention < mentions.size() &&
               mentions.at(nextMention).ts < ts) {
            const IssueMention &mention = mentions.at(nextMention++);
            IssueEvent synthetic;
            synthetic.author = mention.author;
            synthetic.authorName = mention.authorName;
            synthetic.ts = mention.ts;
            QString title = mention.sourceTitle.trimmed();
            if (title.size() > 120)
                title = title.left(119) + QChar(0x2026);
            addActivityHtml(
                synthetic,
                QStringLiteral("mentioned this in <a href='forkmesh-issue:%1' "
                               "style='color:%4;text-decoration:none'>"
                               "#%1 %2</a>%3")
                    .arg(QString::number(mention.sourceNumber),
                         title.toHtmlEscaped(),
                         mention.sourceStatus == QLatin1String("closed")
                             ? QStringLiteral(" <span style='color:#8b949e'>(Closed)</span>")
                             : QString(),
                         Theme::nextOrHex(Theme::Role::AccentFg,
                                          QStringLiteral("#58a6ff"))));
        }
    };

    bool openedShown = false;
    for (const IssueEvent &ev : issue.events) {
        if (openedShown)
            addMentionsBefore(ev.ts);
        if (ev.type == "reaction")
            continue; // already drawn as a chip on the event it reacted to
        if (ev.type == "delete" && reactionIds.contains(ev.target))
            continue; // an un-react, not a deleted comment
        if (ev.type == "open") {
            // A signed "reporter" event moves the credit without touching the
            // opening event, so the card reads from the folded name.
            IssueEvent opened = ev;
            if (!issue.authorName.trimmed().isEmpty())
                opened.authorName = issue.authorName;
            addCard(opened, true);
            if (!openedShown)
                addSubIssues();
            openedShown = true;
        } else if (ev.type == "comment") {
            if (!deleted.contains(ev.id))
                addCard(ev, false);
        } else if (ev.type == "status")
            addActivity(ev, ev.status == "closed" ? "closed this" : "reopened this");
        else if (ev.type == "labels")
            addActivity(ev, "set labels: " + ev.labels.join(", "));
        else if (ev.type == "milestone")
            addActivity(ev, ev.milestone.isEmpty() ? "cleared the milestone"
                                                   : "set milestone: " + ev.milestone);
        else if (ev.type == "priority")
            addActivity(ev, ev.priority > 0
                                ? QStringLiteral("set priority: %1").arg(ev.priority)
                                : QStringLiteral("cleared the priority"));
        else if (ev.type == "assignees")
            addActivity(ev, "set assignees: " + ev.assignees.join(", "));
        else if (ev.type == "claim")
            addActivity(ev, ev.claimState == QLatin1String("released")
                                ? QStringLiteral("unassigned themselves")
                                : QStringLiteral("assigned themselves"));
        else if (ev.type == "reporter")
            addActivity(ev, ev.reporter.isEmpty()
                                ? QStringLiteral("cleared the reporter")
                                : QStringLiteral("credited this report to %1")
                                      .arg(ev.reporter));
        else if (ev.type == "agent") {
            QString text;
            if (ev.agentSessionId <= 0 || ev.agentStatus == AgentStatus::Cleared) {
                text = QStringLiteral("cleared the agent assignment");
            } else {
                text = QStringLiteral("assigned %1 session #%2")
                           .arg(agentProviderName(ev.agentProvider))
                           .arg(ev.agentSessionId);
                if (ev.agentCreatePr)
                    text += QStringLiteral(" with PR creation requested");
                if (!ev.agentStatus.isEmpty())
                    text += QStringLiteral(" (%1)").arg(agentStatusText(ev.agentStatus));
            }
            addActivity(ev, text);
        } else if (ev.type == "title")
            addActivity(ev, ev.title.isEmpty()
                                ? QStringLiteral("cleared the title")
                                : QStringLiteral("changed the title to \"%1\"").arg(ev.title));
        else if (ev.type == "progress")
            addActivity(ev, QStringLiteral("set progress to %1%").arg(ev.progress));
        else if (ev.type == "dates")
            addActivity(ev, QStringLiteral("updated the schedule dates"));
        else if (ev.type == "bounty")
            addActivity(ev, ev.bountyUsd > 0
                                ? QStringLiteral("set a $%1 bounty%2")
                                      .arg(QString::number(ev.bountyUsd),
                                           ev.bountyStatus.isEmpty()
                                               ? QString()
                                               : QStringLiteral(" (%1)").arg(ev.bountyStatus))
                                : QStringLiteral("cleared the bounty"));
        else if (ev.type == "edit")
            addActivity(ev, ev.target == openId ? QStringLiteral("edited the description")
                                                : QStringLiteral("edited a comment"));
        else if (ev.type == "delete")
            addActivity(ev, ev.target == "self" ? QStringLiteral("deleted this issue")
                                                : QStringLiteral("deleted a comment"));
        else if (ev.type == "vote")
            addActivity(ev, QStringLiteral("voted on this issue"));
        else if (!ev.type.isEmpty())
            addActivity(ev, QStringLiteral("recorded a %1 action").arg(ev.type));
    }
    if (!openedShown)
        addSubIssues();
    addMentionsBefore(std::numeric_limits<qint64>::max());
    addIssueRemoteThread(issue);
    if (m_issueComposerRow) {
        m_issueThreadLayout->addWidget(m_issueComposerRow);
        m_issueComposerRow->show();
    }
    m_issueThreadLayout->addStretch();
}

// The detail column with no issue selected: every sidebar value back to its
// placeholder and an empty thread.
void MainWindow::renderNoIssueSelected()
{
    m_issueTitle->setText("Select an issue");
    if (m_issueParentRef)
        m_issueParentRef->hide();
    cancelIssueTitleEdit();
    m_issueMeta->clear();
    m_issueMeta->hide();
    renderIssueAssigneesValue({});
    renderIssueReporterValue(QString());
    if (m_issueLabelsValue)
        m_issueLabelsValue->setText("No labels");
    if (m_issueMilestoneValue)
        m_issueMilestoneValue->setText("No milestone");
    if (m_issueDatesValue)
        m_issueDatesValue->setText("No dates");
    if (m_issuePriorityValue)
        m_issuePriorityValue->setText("No priority");
    if (m_issueEstimateValue)
        m_issueEstimateValue->setText(QString::fromUtf8("\xE2\x80\x94"));
    if (m_issueBountyValue)
        m_issueBountyValue->setText("No bounty");
    if (m_issueCommentsValue)
        m_issueCommentsValue->setText("No comments");
    updateIssueAgentUi(Issue());
    refreshIssueFilesPanel(Issue());
    cancelIssueSidebarEditors();
    m_issueThreadLayout->addStretch();
}

void MainWindow::renderIssueParentRefs(const Issue &issue)
{
    // An issue whose description names a parent ("Parent issue: #N" or
    // "Depends on: #N") is that parent's sub-issue; say so under the status.
    // Status comes from the current snapshot, so a parent closed elsewhere
    // reads Closed after the next sync.
    auto findIssue = [this](int number) -> const Issue * {
        for (const Issue &candidate : m_currentIssues) {
            if (candidate.number == number)
                return &candidate;
        }
        return nullptr;
    };
    if (m_issueParentRef) {
        QLayout *parentLayout = m_issueParentRef->layout();
        while (parentLayout->count() > 1) {
            QLayoutItem *item = parentLayout->takeAt(1);
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        for (int parentNumber : IssueStore::dependencyNumbers(
                 IssueStore::effectiveBody(issue))) {
            if (parentNumber == issue.number)
                continue;
            const Issue *parent = findIssue(parentNumber);
            const QString fullTitle = parent ? parent->title.trimmed() : QString();
            QString title = fullTitle;
            if (title.size() > 80)
                title = title.left(79) + QChar(0x2026);
            const bool parentClosed =
                parent && parent->status == QLatin1String("closed");
            const QString status = !parent ? QStringLiteral("unknown")
                : parentClosed ? QStringLiteral("closed")
                               : QStringLiteral("open");
            auto *pill = new QPushButton(
                title.isEmpty()
                    ? QStringLiteral("#%1").arg(parentNumber)
                    : QStringLiteral("#%1 %2").arg(parentNumber).arg(title),
                m_issueParentRef);
            pill->setObjectName(QStringLiteral("issueParentPill"));
            pill->setProperty("status", status);
            pill->setCursor(Qt::PointingHandCursor);
            pill->setIcon(themedOcticon(
                parentClosed ? QStringLiteral("check-circle")
                             : QStringLiteral("issue-opened"),
                parentClosed
                    ? Theme::nextOr(Theme::Role::AccentFg,
                                    Theme::remapColor(QColor("#8250df")))
                    : Theme::remapColor(QColor(!parent ? "#8b949e" : "#1a7f37")),
                14));
            pill->setIconSize(QSize(14, 14));
            pill->setToolTip(
                !parent ? QStringLiteral("Issue #%1 has not synced to this mirror yet")
                              .arg(parentNumber)
                        : QStringLiteral("#%1 %2 (%3)")
                              .arg(parentNumber)
                              .arg(fullTitle,
                                   parentClosed ? QStringLiteral("Closed")
                                                : QStringLiteral("Open")));
            connect(pill, &QPushButton::clicked, this,
                    [this, parentNumber]() { showIssue(parentNumber); });
            parentLayout->addWidget(pill);
        }
        m_issueParentRef->setVisible(parentLayout->count() > 1);
    }
}

// code-health: allow(stylesheet-in-loop) one colour sheet per label pill on the selected issue (a handful), set on new pills before they are first polished
void MainWindow::renderIssueSidebarValues(const Issue &issue)
{
    const bool issueClosed = issue.status == "closed";
    m_issueMeta->show();
    m_issueMeta->setText(issueClosed ? "Closed" : "Open");
    m_issueMeta->setProperty("status", issueClosed ? "closed" : "open");
    m_issueMeta->style()->unpolish(m_issueMeta);
    m_issueMeta->style()->polish(m_issueMeta);

    auto colorFor = [this](const QString &name) -> QString {
        for (const IssueLabel &l : m_currentLabels)
            if (l.name == name && !l.color.isEmpty())
                return l.color;
        return QStringLiteral("#94a3b8");
    };
    renderIssueAssigneesValue(issue.assignees);
    renderIssueReporterValue(issue.authorName);
    if (issue.labels.isEmpty()) {
        m_issueLabelsValue->setText("No labels");
    } else {
        m_issueLabelsValue->hide();
        for (const QString &name : issue.labels) {
            auto *pill = new QLabel(name, m_issueLabelsView);
            pill->setObjectName(QStringLiteral("issueLabelPill"));
            pill->setProperty("labelColor", colorFor(name));
            pill->setAlignment(Qt::AlignCenter);
            pill->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            pill->setStyleSheet(Theme::remapSheet(
                QStringLiteral("QLabel { color: %1; border-color: %1; }")
                    .arg(colorFor(name))));
            m_issueLabelsPillsLayout->addWidget(pill);
        }
    }
    m_issueMilestoneValue->setText(
        issue.milestone.isEmpty()
            ? QStringLiteral("No milestone")
            : QStringLiteral("<b>%1</b>").arg(issue.milestone.toHtmlEscaped()));
    if (m_issueDatesValue) {
        auto dateText = [](qint64 ms) {
            return ms > 0 ? QDateTime::fromMSecsSinceEpoch(ms)
                                .toString(QStringLiteral("yyyy-MM-dd"))
                          : QString::fromUtf8("\xE2\x80\x94");
        };
        m_issueDatesValue->setText(
            issue.startDate <= 0 && issue.endDate <= 0
                ? QStringLiteral("No dates")
                : QString::fromUtf8("%1 \xE2\x86\x92 %2")
                      .arg(dateText(issue.startDate), dateText(issue.endDate)));
    }
    m_issuePriorityValue->setText(
        issue.priority > 0
            ? QStringLiteral("<b style='color:%1'>%2 · %3</b> "
                             "<span style='color:#8b949e'>(1 highest, 8999 lowest)</span>")
                  .arg(issuePriorityColor(issue.priority).name())
                  .arg(issue.priority)
                  .arg(issuePriorityMeaning(issue.priority))
            : QStringLiteral("No priority"));
    if (m_issueProgressSlider)
        static_cast<ProgressSlider *>(m_issueProgressSlider)
            ->setValue(qBound(0, issue.progress, 100));
    if (m_issueEstimateValue) {
        m_issueEstimateValue->setText(
            QStringLiteral("~$%1 <span style='color:#8b949e'>(OpenAI to implement)</span>")
                .arg(QString::number(openAiEstimateUsd(issue), 'f', 2)));
    }
    if (m_issueBountyValue) {
        if (issue.bountyUsd > 0) {
            const QString status = issue.bountyStatus.isEmpty()
                                       ? QStringLiteral("open")
                                       : issue.bountyStatus;
            m_issueBountyValue->setText(
                QStringLiteral("<b>$%1</b> <span style='color:#8b949e'>"
                               "(legacy, migration-only: %2)</span>")
                    .arg(QString::number(issue.bountyUsd, 'f', 2), status.toHtmlEscaped()));
        } else {
            m_issueBountyValue->setText(QStringLiteral("No bounty"));
        }
    }
    if (m_issueCommentsValue) {
        QSet<QString> deleted;
        for (const IssueEvent &ev : issue.events)
            if (ev.type == QLatin1String("delete") && !ev.target.isEmpty() &&
                ev.target != QLatin1String("self"))
                deleted.insert(ev.target);
        int comments = 0;
        QString latest;
        qint64 latestTs = -1;
        for (const IssueEvent &ev : issue.events) {
            if (ev.type != QLatin1String("comment") || deleted.contains(ev.id))
                continue;
            ++comments;
            if (ev.ts >= latestTs) {
                latestTs = ev.ts;
                latest = ev.authorName.trimmed().isEmpty() ? ev.author.left(8)
                                                            : ev.authorName.trimmed();
            }
        }
        m_issueCommentsValue->setText(
            comments == 0
                ? QStringLiteral("No comments")
                : QStringLiteral("<b>%1</b>%2")
                      .arg(comments)
                      .arg(latest.isEmpty()
                               ? QString()
                               : QStringLiteral(" <span style='color:#8b949e'>latest by %1</span>")
                                     .arg(latest.toHtmlEscaped())));
    }
    updateIssueAgentUi(issue);
    refreshIssueFilesPanel(issue);
    if (m_issueDevelopmentValue) {
        const QList<int> pulls = pullsLinkedToIssue(issue.number);
        if (pulls.isEmpty()) {
            m_issueDevelopmentValue->setText("No linked pull requests.");
        } else {
            QStringList links;
            for (const int n : pulls) {
                QString badge;
                for (const PullRequest &pr : m_currentPulls) {
                    if (pr.number != n)
                        continue;
                    const QString label =
                        pr.status == QLatin1String("merged")  ? QStringLiteral("Merged")
                        : pr.status == QLatin1String("closed") ? QStringLiteral("Closed")
                                                               : QStringLiteral("Open");
                    const QString color =
                        pr.status == QLatin1String("merged")
                            ? Theme::nextOrHex(Theme::Role::AccentFg,
                                               QStringLiteral("#a371f7"))
                        : pr.status == QLatin1String("closed") ? QStringLiteral("#f85149")
                                                               : QStringLiteral("#3fb950");
                    badge = QStringLiteral(
                                " <span style='color:%1'>%2</span>").arg(color, label);
                    break;
                }
                links << QStringLiteral(
                             "<a href='pull:%1' style='color:%3;"
                             "text-decoration:none'>pull request #%1</a>%2")
                             .arg(QString::number(n), badge,
                                  Theme::nextOrHex(Theme::Role::AccentFg,
                                                   QStringLiteral("#58a6ff")));
            }
            m_issueDevelopmentValue->setText(links.join("<br>"));
        }
    }
    if (m_issueAssigneesEdit)
        m_issueAssigneesEdit->setText(issue.assignees.join(", "));
    if (m_issueReporterEdit)
        m_issueReporterEdit->setText(issue.authorName);
    if (m_issueLabelsEdit)
        m_issueLabelsEdit->setText(issue.labels.join(", "));
    if (m_issueMilestoneEdit) {
        QSignalBlocker blocker(m_issueMilestoneEdit);
        m_issueMilestoneEdit->clear();
        m_issueMilestoneEdit->addItem("No milestone", QString());
        for (const IssueMilestone &ms : m_currentMilestones)
            m_issueMilestoneEdit->addItem(ms.title, ms.title);
        const int selected = m_issueMilestoneEdit->findData(issue.milestone);
        if (selected >= 0)
            m_issueMilestoneEdit->setCurrentIndex(selected);
    }
    if (m_issueStartDateEdit && m_issueStartDateEnable) {
        m_issueStartDateEnable->setChecked(issue.startDate > 0);
        m_issueStartDateEdit->setDate(
            issue.startDate > 0
                ? QDateTime::fromMSecsSinceEpoch(issue.startDate).date()
                : QDate::currentDate());
    }
    if (m_issueEndDateEdit && m_issueEndDateEnable) {
        m_issueEndDateEnable->setChecked(issue.endDate > 0);
        m_issueEndDateEdit->setDate(
            issue.endDate > 0
                ? QDateTime::fromMSecsSinceEpoch(issue.endDate).date()
                : QDate::currentDate());
    }
    if (m_issuePriorityEdit)
        m_issuePriorityEdit->setValue(issue.priority);
    cancelIssueSidebarEditors();
}

// One comment or description card in the issue timeline: avatar, header with
// its actions, the body, and its reactions.
void MainWindow::addIssueThreadCard(const Issue &issue, const IssueEvent &ev,
                                    bool isOpen,
                                    const IssueThreadCardContext &context)
{
    const QHash<QString, IssueEvent> &edits = context.edits;
    const bool writable = context.writable;
    IssueEvent shown = ev;
    if (edits.contains(ev.id)) {
        shown.body = edits.value(ev.id).body;
        shown.attachments = edits.value(ev.id).attachments;
    }
    const int num = issue.number;
    const QString eid = ev.id;
    const QString eventBody = shown.body;
    const QStringList eventAttachments = shown.attachments;
    const QString who = ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
    const QString when = formatIssueRelativeTime(ev.ts);

    auto *row = new QWidget;
    row->setObjectName("issueTimelineRow");
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(14);
    auto *avatar = new QLabel(who.left(2).toUpper());
    avatar->setObjectName("issueAvatar");
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(36, 36);
    avatar->setScaledContents(true);
    avatar->setProperty(kIssueAvatarAuthorIdProperty, ev.author);
    avatar->setProperty(kIssueAvatarAuthorNameProperty, ev.authorName);
    avatar->setProperty(kIssueAvatarSideProperty, 36);
    const QPixmap authorAvatar =
        issueTimelineAvatar(ev.author, ev.authorName, 36);
    if (!authorAvatar.isNull()) {
        avatar->setText(QString());
        avatar->setPixmap(authorAvatar);
    }
    avatar->setToolTip(who);
    rowLayout->addWidget(avatar, 0, Qt::AlignTop);

    auto *card = new QWidget;
    card->setObjectName("issueTimelineCard");
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    auto *headerBox = new QWidget(card);
    headerBox->setObjectName("issueTimelineHeader");
    auto *headerRow = new QHBoxLayout(headerBox);
    headerRow->setContentsMargins(16, 8, 10, 8);
    headerRow->setSpacing(8);
    auto *header = new QLabel(
        QStringLiteral("<b>%1</b> <span>%2 %3</span>")
            .arg(userProfileLinkHtml(ev.authorName, who),
                 isOpen ? "opened" : "commented", when));
    header->setTextFormat(Qt::RichText);
    header->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    header->setOpenExternalLinks(false);
    connect(header, &QLabel::linkActivated, this,
            [this](const QString &href) { openBodyReference(href); });
    header->setToolTip(issueFullDateTime(ev.ts));
    headerRow->addWidget(header);
    headerRow->addStretch();

    auto *bodyContainer = new ClickableIssueBody(card);
    auto *bodyLayout = new QVBoxLayout(bodyContainer);
    bodyLayout->setContentsMargins(16, 16, 16, 16);
    bodyLayout->setSpacing(10);

    auto clearBody = [bodyLayout]() { clearIssueCardBody(bodyLayout); };
    auto showBodyEditor = [=]() {
        clearBody();
        bodyContainer->onClicked = nullptr;
        bodyContainer->unsetCursor();
        bodyContainer->setMinimumHeight(0);
        auto *editor = new MarkdownEditor(bodyContainer);
        editor->setMarkdown(eventBody);
        editor->setMentionCandidates(issueMentionCandidateNames());
        editor->setMinimumHeight(250);
        editor->setPlaceholderText(isOpen ? "Type your description here..."
                                          : "Type your comment here...");
        const int repoIdx = issuesRepoIndex();
        if (repoIdx >= 0)
            editor->setPreviewBasePath(IssueStore::issueDirPath(
                m_repositories.at(repoIdx).localPath, num));
        bodyLayout->addWidget(editor);
        auto *attach = new QPushButton("Paste, drop, or click to add files");
        attach->setObjectName("ghostButton");
        attach->setCursor(Qt::PointingHandCursor);
        setOcticon(attach, "paperclip", 16);
        connect(attach, &QPushButton::clicked, this, [editor]() {
            const QStringList files = QFileDialog::getOpenFileNames(
                editor, "Attach images", QString(),
                "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg);;All files (*)");
            for (const QString &file : files)
                editor->addImageFile(file);
        });
        bodyLayout->addWidget(attach, 0, Qt::AlignLeft);
        auto *buttonRow = new QHBoxLayout;
        buttonRow->setContentsMargins(0, 0, 0, 0);
        auto *cancel = new QPushButton("Cancel", bodyContainer);
        cancel->setObjectName("ghostButton");
        cancel->setCursor(Qt::PointingHandCursor);
        auto *save = new QPushButton("Save", bodyContainer);
        save->setObjectName("primaryButton");
        save->setCursor(Qt::PointingHandCursor);
        buttonRow->addStretch();
        buttonRow->addWidget(cancel);
        buttonRow->addWidget(save);
        bodyLayout->addLayout(buttonRow);
        QPointer<QPushButton> saveGuard(save);
        QTimer::singleShot(0, this, [saveGuard]() {
            if (!saveGuard)
                return;
            saveGuard->style()->unpolish(saveGuard);
            saveGuard->style()->polish(saveGuard);
            saveGuard->update();
        });
        connect(cancel, &QPushButton::clicked, this, [this, num]() { showIssue(num); });
        connect(save, &QPushButton::clicked, this,
                [this, num, eid, eventAttachments, editor]() {
                    IssueStore store = issueStoreForCurrentRepo();
                    QString error;
                    if (!store.editEvent(num, eid, editor->markdown(),
                                         eventAttachments,
                                         editor->pendingAttachments(),
                                         editor->pendingAttachmentPlaceholders(),
                                         &error)) {
                        setIssueInlineNotice(
                            error.isEmpty() ? "Could not update the issue body."
                                            : error,
                            true);
                        return;
                    }
                    setIssueInlineNotice("Issue body updated.");
                    reloadIssues();
                });
        editor->focusEditor();
    };
    if (writable && isOpen && eventBody.trimmed().isEmpty())
        bodyContainer->onClicked = [=]() { showBodyEditor(); };

    QMenu *menu = buildIssueCardMenu(card, num, eid, eventBody, writable, isOpen);
    addIssueCardSubIssuesButton(headerBox, headerRow, num, eventBody, writable,
                                isOpen);
    if (writable) {
        auto *editButton = new QPushButton(headerBox);
        editButton->setObjectName("issueActionButton");
        editButton->setFixedSize(30, 30);
        editButton->setCursor(Qt::PointingHandCursor);
        editButton->setToolTip(isOpen ? "Edit description" : "Edit comment");
        setOcticon(editButton, "pencil", 15);
        connect(editButton, &QPushButton::clicked, this, showBodyEditor);
        headerRow->addWidget(editButton);
    }
    auto *actionsButton = new QToolButton(headerBox);
    actionsButton->setObjectName("issueActionButton");
    actionsButton->setText("...");
    actionsButton->setCursor(Qt::PointingHandCursor);
    actionsButton->setPopupMode(QToolButton::InstantPopup);
    actionsButton->setMenu(menu);
    headerRow->addWidget(actionsButton);

    cardLayout->addWidget(headerBox);
    renderIssueCardBody(bodyContainer, bodyLayout, num, eid, eventBody,
                        eventAttachments, isOpen, writable, context.imageBase,
                        context.haveLocalFiles);
    cardLayout->addWidget(bodyContainer);

    addIssueCardReactions(card, cardLayout, issue, num, eid, writable,
                          context.myKey, context.myName);
    rowLayout->addWidget(card, 1);
    m_issueThreadLayout->addWidget(row);
}

// Draws a timeline card body: the Markdown (or its checklist rows) followed by
// the attached images.
void MainWindow::renderIssueCardBody(QWidget *bodyContainer,
                                     QVBoxLayout *bodyLayout, int num,
                                     const QString &eid,
                                     const QString &eventBody,
                                     const QStringList &eventAttachments,
                                     bool isOpen, bool writable,
                                     const QString &imageBase,
                                     bool haveLocalFiles)
{
    clearIssueCardBody(bodyLayout);
    const bool emptyEditableDescription =
        writable && isOpen && eventBody.trimmed().isEmpty();
    if (emptyEditableDescription) {
        auto *body = new QLabel;
        body->setText("Click to add a description.");
        body->setObjectName("statusLine");
        body->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        bodyContainer->setCursor(Qt::PointingHandCursor);
        bodyContainer->setMinimumHeight(52);
        bodyLayout->addWidget(body);
    } else {
        bodyContainer->unsetCursor();
        bodyContainer->setMinimumHeight(0);
        const QList<IssueChecklistTask> tasks =
            isOpen ? IssueStore::checklistTasks(eventBody)
                   : QList<IssueChecklistTask>();
        // Relationship lines stay in the signed Markdown (and the
        // editor) but render only once, in the list below.
        const QString displayBody = isOpen
            ? IssueStore::bodyWithoutDependencyLines(eventBody)
            : eventBody;
        if (tasks.isEmpty() && displayBody.trimmed().isEmpty()) {
            // A description that is only a relationship line.
        } else if (tasks.isEmpty()) {
            auto *body = new QLabel;
            body->setTextFormat(Qt::MarkdownText);
            body->setText(autolinkReferencesWithStatus(displayBody));
            body->setWordWrap(true);
            body->setTextInteractionFlags(Qt::TextBrowserInteraction);
            body->setOpenExternalLinks(false);
            connect(body, &QLabel::linkActivated, this,
                    [this](const QString &href) {
                        openBodyReference(href);
                    });
            bodyLayout->addWidget(body);
        } else {
            // Render source-backed rows between ordinary Markdown
            // chunks. Keeping the source line on every row means each
            // interaction becomes one signed description edit.
            bodyLayout->addWidget(buildIssueChecklist(
                bodyContainer, num, eid, eventBody, eventAttachments, displayBody,
                tasks, writable));
        }
    }
    for (const QString &rel : eventAttachments) {
        if (haveLocalFiles) {
            const QString imagePath = imageBase + rel;
            QPixmap pix(imagePath);
            if (!pix.isNull()) {
                auto *img = new ClickableIssueImage;
                img->setObjectName("issueAttachmentPreview");
                img->setPixmap(pix.width() > 640
                                   ? pix.scaledToWidth(640, Qt::SmoothTransformation)
                                   : pix);
                img->setCursor(Qt::PointingHandCursor);
                img->setToolTip(QStringLiteral("Click to view full size"));
                img->onClicked = [this, imagePath]() {
                    showQuickAddImageDetail(imagePath);
                };
                bodyLayout->addWidget(img);
                continue;
            }
        }
        auto *placeholder =
            new QLabel(QStringLiteral("Image: %1").arg(rel));
        placeholder->setObjectName("statusLine");
        bodyLayout->addWidget(placeholder);
    }
}

QWidget *MainWindow::buildIssueChecklist(QWidget *bodyContainer, int num,
                                         const QString &eid,
                                         const QString &eventBody,
                                         const QStringList &eventAttachments,
                                         const QString &displayBody,
                                         const QList<IssueChecklistTask> &tasks,
                                         bool writable)
{
    auto *checklist = new QWidget(bodyContainer);
    checklist->setObjectName(QStringLiteral("issueChecklist"));
    auto *checklistLayout = new QVBoxLayout(checklist);
    checklistLayout->setContentsMargins(0, 0, 0, 0);
    checklistLayout->setSpacing(1);
    const QStringList sourceLines =
        displayBody.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    auto addMarkdownChunk = [&](int begin, int end) {
        if (begin >= end)
            return;
        QStringList chunkLines;
        for (int line = begin; line < end; ++line)
            chunkLines.append(sourceLines.value(line));
        const QString markdown = chunkLines.join(QLatin1Char('\n'));
        if (markdown.trimmed().isEmpty())
            return;
        auto *chunk = new QLabel(checklist);
        chunk->setObjectName(QStringLiteral("issueChecklistMarkdown"));
        chunk->setTextFormat(Qt::MarkdownText);
        chunk->setText(autolinkReferencesWithStatus(markdown));
        chunk->setWordWrap(true);
        chunk->setTextInteractionFlags(Qt::TextBrowserInteraction);
        chunk->setOpenExternalLinks(false);
        connect(chunk, &QLabel::linkActivated, this,
                [this](const QString &href) {
                    openBodyReference(href);
                });
        checklistLayout->addWidget(chunk);
    };

    QPointer<QWidget> checklistGuard(checklist);
    auto persistChecklistBody =
        [=](const QString &nextBody, const QString &notice,
            int reloadDelayMs) {
            if (!checklistGuard)
                return;
            checklistGuard->setEnabled(false);
            QTimer::singleShot(90, this,
                [=]() {
                    IssueStore store = issueStoreForCurrentRepo();
                    QString error;
                    if (!store.editEvent(num, eid, nextBody,
                                         eventAttachments, {},
                                         &error)) {
                        setIssueInlineNotice(
                            error.isEmpty()
                                ? QStringLiteral(
                                      "Could not update the checklist.")
                                : error,
                            true);
                        m_selectIssueOnReload = num;
                        reloadIssues();
                        return;
                    }
                    propagateRepoUpdate(issuesRepoIndex());
                    setIssueInlineNotice(notice);
                    m_selectIssueOnReload = num;
                    QTimer::singleShot(reloadDelayMs, this,
                        [this]() { reloadIssues(); });
                });
        };
    auto reorderTask = [=](int fromLine, int toLine,
                           bool placeAfter) {
        bool changed = false;
        const QString reordered = IssueStore::reorderChecklistTask(
            eventBody, fromLine, toLine, placeAfter, &changed);
        if (!changed)
            return;
        persistChecklistBody(
            reordered, QStringLiteral("Checklist reordered."), 40);
    };

    int cursor = 0;
    for (int taskIndex = 0; taskIndex < tasks.size(); ++taskIndex) {
        const IssueChecklistTask task = tasks.at(taskIndex);
        addMarkdownChunk(cursor, task.line);
        auto *taskRow =
            new IssueChecklistRow(
                task, writable,
                autolinkReferencesWithStatus(task.label),
                checklist);
        connect(taskRow->label(), &QLabel::linkActivated, this,
                [this](const QString &href) {
                    openBodyReference(href);
                });
        if (writable) {
            connect(taskRow->checkButton(),
                    &QAbstractButton::clicked, this,
                    [=](bool checked) {
                        taskRow->setCompleted(checked);
                        bool changed = false;
                        const QString nextBody =
                            IssueStore::setChecklistTaskChecked(
                                eventBody, task.line, checked,
                                &changed);
                        if (!changed)
                            return;
                        persistChecklistBody(
                            nextBody,
                            checked
                                ? QStringLiteral(
                                      "Task checked — constellation complete!")
                                : QStringLiteral(
                                      "Task reopened — back into orbit."),
                            checked ? 690 : 530);
                    });
            taskRow->onDropped = reorderTask;
            if (IssueTaskDragButton *drag = taskRow->dragButton()) {
                drag->onKeyboardMove =
                    [=](int direction) {
                        int targetIndex = taskIndex + direction;
                        while (targetIndex >= 0 &&
                               targetIndex < tasks.size() &&
                               tasks.at(targetIndex).indent !=
                                   task.indent) {
                            targetIndex += direction;
                        }
                        if (targetIndex < 0 ||
                            targetIndex >= tasks.size())
                            return;
                        reorderTask(
                            task.line,
                            tasks.at(targetIndex).line,
                            direction > 0);
                    };
            }
            if (QPushButton *convert =
                    taskRow->convertButton()) {
                connect(convert, &QPushButton::clicked, this,
                    [=]() {
                        if (QMessageBox::question(
                                this,
                                QStringLiteral(
                                    "Create attached sub-issue"),
                                QStringLiteral(
                                    "Turn this task into an attached "
                                    "sub-issue? The child will link "
                                    "back here and this row will keep "
                                    "its issue number."),
                                QMessageBox::Yes |
                                    QMessageBox::No,
                                QMessageBox::Yes) !=
                            QMessageBox::Yes) {
                            return;
                        }
                        if (checklistGuard)
                            checklistGuard->setEnabled(false);
                        QTimer::singleShot(0, this, [=]() {
                            IssueStore store =
                                issueStoreForCurrentRepo();
                            int child = 0;
                            QString error;
                            if (!store.createSubIssue(
                                    num, task.line, &child,
                                    &error)) {
                                if (checklistGuard)
                                    checklistGuard->setEnabled(true);
                                setIssueInlineNotice(
                                    error.isEmpty()
                                        ? QStringLiteral(
                                              "Could not create the "
                                              "sub-issue.")
                                        : error,
                                    true);
                                return;
                            }
                            propagateRepoUpdate(
                                issuesRepoIndex());
                            triggerTeammatesForEvent(
                                QStringLiteral("new-issues"),
                                QStringLiteral("Issue #%1 · %2")
                                    .arg(child)
                                    .arg(task.label));
                            setIssueInlineNotice(
                                QStringLiteral(
                                    "Created and attached sub-issue "
                                    "#%1.")
                                    .arg(child));
                            m_selectIssueOnReload = num;
                            reloadIssues();
                        });
                    });
            }
        }
        if (QPushButton *attached =
                taskRow->attachedButton()) {
            const int child =
                attached->property("issueNumber").toInt();
            connect(attached, &QPushButton::clicked, this,
                    [this, child]() { showIssue(child); });
        }
        checklistLayout->addWidget(taskRow);
        cursor = task.line + 1;
    }
    addMarkdownChunk(cursor, sourceLines.size());
    return checklist;
}

// The "..." menu on a timeline card: copy link, copy Markdown, quote reply, and
// delete for a comment when the repository is writable.
QMenu *MainWindow::buildIssueCardMenu(QWidget *card, int num, const QString &eid,
                                      const QString &eventBody, bool writable,
                                      bool isOpen)
{
    QMenu *menu = new QMenu(card);
    menu->setAttribute(Qt::WA_TranslucentBackground, false);
    menu->setAutoFillBackground(true);
    menu->setWindowOpacity(1.0);
    const bool darkMenu = currentThemeIsDark();
    menu->setStyleSheet(Theme::remapSheet(
        QStringLiteral(
            "QMenu { background-color:%1; color:%2; border:1px solid %3; "
            "border-radius:8px; padding:6px; }"
            "QMenu::item { background-color:%1; padding:7px 26px 7px 22px; "
            "border-radius:6px; }"
            "QMenu::item:selected { background-color:%4; }"
            "QMenu::separator { height:1px; background:%3; margin:6px 0; }")
            .arg(darkMenu ? "#161b22" : "#ffffff",
                 darkMenu ? "#e6edf3" : "#1f2328",
                 darkMenu ? "#30363d" : "#d0d7de",
                 darkMenu ? "#21262d" : "#f6f8fa")));
    QAction *copyLink = menu->addAction("Copy link");
    QAction *copyMarkdown = menu->addAction("Copy Markdown");
    QAction *quoteReply = menu->addAction("Quote reply");
    connect(copyLink, &QAction::triggered, this, [this, num, eid]() {
        QString owner = "repo";
        QString repo = "issue";
        const int repoIdx = issuesRepoIndex();
        if (repoIdx >= 0) {
            owner = m_repositories.at(repoIdx).owner;
            repo = m_repositories.at(repoIdx).name;
        }
        QApplication::clipboard()->setText(
            QStringLiteral("forkmesh://issue/%1/%2/%3#%4")
                .arg(owner, repo)
                .arg(num)
                .arg(eid));
        setIssueInlineNotice("Issue link copied.");
    });
    connect(copyMarkdown, &QAction::triggered, this, [this, eventBody]() {
        QApplication::clipboard()->setText(eventBody);
        setIssueInlineNotice("Markdown copied.");
    });
    connect(quoteReply, &QAction::triggered, this, [this, eventBody]() {
        if (!m_issueComposer)
            return;
        QStringList quoted;
        for (const QString &line : eventBody.split('\n'))
            quoted << QStringLiteral("> %1").arg(line);
        QString text = m_issueComposer->markdown();
        if (!text.isEmpty() && !text.endsWith('\n'))
            text += '\n';
        text += quoted.join('\n') + "\n\n";
        m_issueComposer->setMarkdown(text);
        m_issueComposer->focusEditor();
        setIssueInlineNotice("Quoted into the comment box.");
    });
    // Comments can be deleted (the description/open event is removed by
    // deleting the whole issue, handled elsewhere). Deletion appends a
    // signed delete event that folds the comment out everywhere and syncs
    // to peers; the text stays in git history.
    if (writable && !isOpen) {
        menu->addSeparator();
        QAction *deleteComment = menu->addAction("Delete comment");
        connect(deleteComment, &QAction::triggered, this, [this, num, eid]() {
            if (QMessageBox::question(
                    this, QStringLiteral("Delete comment"),
                    QStringLiteral(
                        "Delete this comment? It will be hidden everywhere "
                        "and the deletion syncs to peers; the comment stays "
                        "in git history."),
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes)
                return;
            IssueStore store = issueStoreForCurrentRepo();
            QString error;
            if (!store.deleteEvent(num, eid, &error)) {
                setIssueInlineNotice(
                    error.isEmpty() ? "Could not delete the comment." : error,
                    true);
                return;
            }
            setIssueInlineNotice("Comment deleted.");
            reloadIssues();
        });
    }
    return menu;
}

void MainWindow::addIssueCardSubIssuesButton(QWidget *headerBox,
                                             QHBoxLayout *headerRow, int num,
                                             const QString &eventBody,
                                             bool writable, bool isOpen)
{
    const QStringList subIssueTitles =
        isOpen ? IssueStore::subIssueTitles(eventBody) : QStringList();
    if (writable && isOpen && !subIssueTitles.isEmpty()) {
        auto *subIssuesButton = new QPushButton(
            subIssueTitles.size() == 1
                ? QStringLiteral("Create sub-issue")
                : QStringLiteral("Create %1 sub-issues")
                      .arg(subIssueTitles.size()),
            headerBox);
        subIssuesButton->setObjectName("issueSubIssuesButton");
        subIssuesButton->setProperty("buttonSize", "sm");
        subIssuesButton->setCursor(Qt::PointingHandCursor);
        subIssuesButton->setToolTip(
            "Create an issue for each unchecked task and link both directions");
        setOcticon(subIssuesButton, "list-unordered", 15);
        connect(subIssuesButton, &QPushButton::clicked, this,
                [this, num, subIssueTitles, subIssuesButton]() {
                    const int count = subIssueTitles.size();
                    const QString prompt =
                        count == 1
                            ? QStringLiteral(
                                  "Create one sub-issue from this unchecked "
                                  "task? The child will link back here and "
                                  "the task will gain its issue number.")
                            : QStringLiteral(
                                  "Create %1 sub-issues from the unchecked "
                                  "tasks? Each child will link back here and "
                                  "each task will gain its issue number.")
                                  .arg(count);
                    if (QMessageBox::question(
                            this, QStringLiteral("Create sub-issues"), prompt,
                            QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::Yes) != QMessageBox::Yes) {
                        return;
                    }

                    subIssuesButton->setEnabled(false);
                    IssueStore store = issueStoreForCurrentRepo();
                    QList<int> created;
                    QString error;
                    if (!store.createSubIssues(num, &created, &error)) {
                        subIssuesButton->setEnabled(true);
                        setIssueInlineNotice(
                            error.isEmpty()
                                ? QStringLiteral("Could not create sub-issues.")
                                : error,
                            true);
                        return;
                    }
                    if (created.isEmpty()) {
                        subIssuesButton->setEnabled(true);
                        setIssueInlineNotice(
                            "No unlinked unchecked tasks remain in the description.");
                        return;
                    }

                    const int repoIndex = issuesRepoIndex();
                    propagateRepoUpdate(repoIndex);
                    for (int i = 0; i < created.size(); ++i) {
                        triggerTeammatesForEvent(
                            QStringLiteral("new-issues"),
                            QStringLiteral("Issue #%1 · %2")
                                .arg(created.at(i))
                                .arg(subIssueTitles.value(i)));
                    }
                    QStringList references;
                    for (int child : std::as_const(created))
                        references.append(QStringLiteral("#%1").arg(child));
                    setIssueInlineNotice(
                        QStringLiteral("Created %1 sub-issue%2: %3")
                            .arg(created.size())
                            .arg(created.size() == 1 ? QString()
                                                     : QStringLiteral("s"))
                            .arg(references.join(QStringLiteral(", "))));
                    m_selectIssueOnReload = num;
                    reloadIssues();
                });
        headerRow->addWidget(subIssuesButton);
    }
}

void MainWindow::addIssueCardReactions(QWidget *card, QVBoxLayout *cardLayout,
                                       const Issue &issue, int num,
                                       const QString &eid, bool writable,
                                       const QString &myKey,
                                       const QString &myName)
{
    // Reactions on this event. A bot answering an @model mention leaves one
    // here the moment it picks the mention up, which is what makes the wait
    // for its reply read as work in progress rather than silence.
    const QList<IssueReaction> reacted =
        IssueStore::reactionsForEvent(issue, eid, myKey, myName);
    if (writable || !reacted.isEmpty()) {
        auto *reactionsRow = new QWidget(card);
        reactionsRow->setObjectName(QStringLiteral("issueReactionsRow"));
        auto *reactionsLayout = new QHBoxLayout(reactionsRow);
        reactionsLayout->setContentsMargins(16, 0, 16, 12);
        reactionsLayout->setSpacing(4);
        for (const IssueReaction &tally : reacted) {
            auto *chip = new QPushButton(
                QString::number(tally.names.size()), reactionsRow);
            chip->setObjectName(QStringLiteral("reactionChip"));
            const QPixmap glyph = reactions::emojiPixmap(
                tally.value, 16, devicePixelRatio());
            if (glyph.isNull()) {
                chip->setText(reactions::displayName(tally.value) +
                              QLatin1Char(' ') +
                              QString::number(tally.names.size()));
            } else {
                chip->setIcon(QIcon(glyph));
                chip->setIconSize(QSize(16, 16));
            }
            chip->setToolTip(reactions::displayName(tally.value) +
                             QString::fromUtf8(" \xC2\xB7 ") +
                             tally.names.join(QStringLiteral(", ")));
            if (writable) {
                chip->setCursor(Qt::PointingHandCursor);
                const QString value = tally.value;
                connect(chip, &QPushButton::clicked, this,
                        [this, num, eid, value] {
                            toggleIssueReaction(num, eid, value);
                        });
            } else {
                chip->setEnabled(false);
            }
            reactionsLayout->addWidget(chip);
        }
        if (writable) {
            auto *add = new QToolButton(reactionsRow);
            add->setObjectName(QStringLiteral("reactionAdd"));
            add->setCursor(Qt::PointingHandCursor);
            add->setToolTip(QStringLiteral("Add reaction"));
            add->setIcon(QIcon(reactions::addGlyph(
                16, devicePixelRatio(),
                currentThemeIsDark() ? Theme::remapColor(QColor("#8b949e"))
                                     : Theme::remapColor(QColor("#57606a")))));
            add->setIconSize(QSize(16, 16));
            connect(add, &QToolButton::clicked, this, [this, num, eid, add] {
                showIssueReactionPicker(num, eid, add);
            });
            reactionsLayout->addWidget(add);
        }
        reactionsLayout->addStretch();
        cardLayout->addWidget(reactionsRow);
    }
}

// One activity line in the issue timeline. `html` is already escaped;
// activity links name another issue.
void MainWindow::addIssueActivityRow(const IssueEvent &ev, const QString &html)
{
    const QString who =
        ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("issueTimelineRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(14);

    auto *avatar = new QLabel(issueInitials(who), row);
    avatar->setObjectName(QStringLiteral("issueActivityAvatar"));
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(24, 24);
    avatar->setScaledContents(true);
    avatar->setProperty(kIssueAvatarAuthorIdProperty, ev.author);
    avatar->setProperty(kIssueAvatarAuthorNameProperty, ev.authorName);
    avatar->setProperty(kIssueAvatarSideProperty, 24);
    const QPixmap pixmap = issueTimelineAvatar(ev.author, ev.authorName, 24);
    if (!pixmap.isNull()) {
        avatar->clear();
        avatar->setPixmap(pixmap);
    }
    avatar->setToolTip(who);
    auto *avatarColumn = new QWidget(row);
    avatarColumn->setFixedWidth(36);
    auto *avatarLayout = new QHBoxLayout(avatarColumn);
    avatarLayout->setContentsMargins(6, 0, 6, 0);
    avatarLayout->addWidget(avatar, 0, Qt::AlignTop | Qt::AlignHCenter);
    rowLayout->addWidget(avatarColumn, 0, Qt::AlignTop);

    const QString when = formatIssueRelativeTime(ev.ts);
    auto *line = new QLabel(
        QStringLiteral("<b>%1</b> %2 <span style='color:#8b949e'>%3</span>")
            .arg(userProfileLinkHtml(ev.authorName, who), html, when.toHtmlEscaped()),
        row);
    line->setObjectName(QStringLiteral("issueActivityCard"));
    line->setTextFormat(Qt::RichText);
    line->setWordWrap(true);
    line->setToolTip(issueFullDateTime(ev.ts));
    line->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    connect(line, &QLabel::linkActivated, this, [this](const QString &href) {
        if (href.startsWith(QStringLiteral("forkmesh-issue:")))
            showIssue(href.mid(15).toInt());
        else if (href.startsWith(kUserLinkScheme))
            openBodyReference(href);
    });
    rowLayout->addWidget(line, 1);
    m_issueThreadLayout->addWidget(row);
}

// Sub-issues list in the parent, right under its description: every
// child's status, progress, priority, and assignees in one place.
void MainWindow::addIssueSubIssuesSection(const Issue &issue, bool writable)
{
    auto findIssue = [this](int number) -> const Issue * {
        for (const Issue &candidate : m_currentIssues) {
            if (candidate.number == number)
                return &candidate;
        }
        return nullptr;
    };
    const QList<int> children =
        IssueStore::subIssueNumbers(m_currentIssues, issue.number);
    if (children.isEmpty() && !writable)
        return;
    int closedCount = 0;
    for (int childNumber : children) {
        const Issue *child = findIssue(childNumber);
        if (child && child->status == QLatin1String("closed"))
            ++closedCount;
    }
    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("issueTimelineRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(50, 0, 0, 0);
    rowLayout->setSpacing(0);
    auto *section = new QWidget(row);
    section->setObjectName(QStringLiteral("issueSubIssues"));
    auto *sectionLayout = new QVBoxLayout(section);
    sectionLayout->setContentsMargins(12, 10, 12, 10);
    sectionLayout->setSpacing(6);
    auto *headingRow = new QHBoxLayout;
    headingRow->setSpacing(8);
    auto *heading = new QLabel(
        children.isEmpty()
            ? QStringLiteral("Sub-issues")
            : QStringLiteral("Sub-issues <span style='color:#8b949e;"
                             "font-weight:400'>%1 of %2 closed</span>")
                  .arg(closedCount)
                  .arg(children.size()),
        section);
    heading->setObjectName(QStringLiteral("issueDependenciesHeading"));
    heading->setTextFormat(Qt::RichText);
    headingRow->addWidget(heading, 1);
    if (writable) {
        auto *add = new QPushButton(QStringLiteral("Add sub-issue"), section);
        add->setObjectName(QStringLiteral("ghostButton"));
        add->setProperty("buttonSize", "sm");
        add->setCursor(Qt::PointingHandCursor);
        add->setToolTip(QStringLiteral(
            "File a new issue whose description links back to #%1")
                            .arg(issue.number));
        const int parentNumber = issue.number;
        connect(add, &QPushButton::clicked, this, [this, parentNumber]() {
            composeNewIssue(QString(),
                            QStringLiteral("Parent issue: #%1\n\n").arg(parentNumber));
        });
        headingRow->addWidget(add, 0, Qt::AlignRight);
    }
    sectionLayout->addLayout(headingRow);
    if (children.isEmpty()) {
        auto *empty = new QLabel(
            QStringLiteral("No sub-issues yet."), section);
        empty->setObjectName(QStringLiteral("statusLine"));
        sectionLayout->addWidget(empty);
    }
    for (int childNumber : children) {
        const Issue *child = findIssue(childNumber);
        const QString status = !child ? QStringLiteral("unknown")
            : child->status == QLatin1String("closed")
                ? QStringLiteral("closed")
                : QStringLiteral("open");
        const QString statusText = status == QLatin1String("closed")
            ? QStringLiteral("Closed")
            : status == QLatin1String("open") ? QStringLiteral("Open")
                                              : QStringLiteral("Not synced");
        QString title = child && !child->title.trimmed().isEmpty()
            ? child->title.trimmed()
            : QStringLiteral("Issue #%1").arg(childNumber);
        const QString fullTitle = title;
        if (title.size() > 120)
            title = title.left(119) + QChar(0x2026);
        QStringList details;
        if (child) {
            details << QStringLiteral("%1%").arg(qBound(0, child->progress, 100));
            if (child->priority > 0)
                details << QStringLiteral("P%1").arg(child->priority);
            details << (child->assignees.isEmpty()
                            ? QStringLiteral("Unassigned")
                            : child->assignees.join(QStringLiteral(", ")));
        }

        auto *childRow = new QPushButton(section);
        childRow->setObjectName(QStringLiteral("issueDependencyRow"));
        childRow->setProperty("status", status);
        childRow->setCursor(Qt::PointingHandCursor);
        childRow->setMinimumHeight(36);
        childRow->setToolTip(child
            ? QStringLiteral("#%1 %2 (%3)").arg(childNumber).arg(fullTitle, statusText)
            : QStringLiteral("Issue #%1 has not synced to this mirror yet")
                  .arg(childNumber));
        auto *childLayout = new QHBoxLayout(childRow);
        childLayout->setContentsMargins(10, 6, 10, 6);
        childLayout->setSpacing(8);
        auto *number = new QLabel(QStringLiteral("#%1").arg(childNumber), childRow);
        number->setObjectName(QStringLiteral("issueDependencyNumber"));
        auto *titleLabel = new QLabel(title, childRow);
        titleLabel->setObjectName(QStringLiteral("issueDependencyTitle"));
        titleLabel->setTextFormat(Qt::PlainText);
        titleLabel->setProperty("status", status);
        titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        auto *detailLabel = new QLabel(details.join(QStringLiteral(" · ")),
                                       childRow);
        detailLabel->setObjectName(QStringLiteral("issueDependencyNumber"));
        detailLabel->setTextFormat(Qt::PlainText);
        auto *pill = new QLabel(statusText, childRow);
        pill->setObjectName(QStringLiteral("issueDependencyStatus"));
        pill->setProperty("status", status);
        for (QLabel *label : {number, titleLabel, detailLabel, pill})
            label->setAttribute(Qt::WA_TransparentForMouseEvents);
        childLayout->addWidget(number);
        childLayout->addWidget(titleLabel, 1);
        if (child)
            childLayout->addWidget(detailLabel);
        else
            detailLabel->hide();
        childLayout->addWidget(pill);
        connect(childRow, &QPushButton::clicked, this,
                [this, childNumber]() { showIssue(childNumber); });
        sectionLayout->addWidget(childRow);
    }
    rowLayout->addWidget(section, 1);
    m_issueThreadLayout->addWidget(row);
}

void MainWindow::addIssueRemoteThread(const Issue &issue)
{
    // Private repositories have no ActivityPub actor, so there is no remote
    // thread to show and the lookup would only leak the repo name in a URL.
    if (m_repoDetailIndex >= 0 &&
        m_repoDetailIndex < m_repositories.size() && m_networkAccess &&
        !m_repositories.at(m_repoDetailIndex).isPrivate) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        auto *remoteThread =
            new FederatedThreadView(m_networkAccess, m_issueThreadContainer);
        const QUrl server(canonicalServerUrl(
            m_activeServer >= 0 && m_activeServer < m_servers.size()
                ? m_servers.at(m_activeServer).url
                : QString()));
        remoteThread->load(server, repo.owner, repo.name,
                           QStringLiteral("issue"), issue.number);
        // Line the section up with the comment column: same avatar gutter, so
        // the remote replies read as part of the conversation rather than as a
        // stray note pinned to the pane's left edge.
        auto *remoteRow = new QWidget;
        remoteRow->setObjectName("issueTimelineRow");
        auto *remoteRowLayout = new QHBoxLayout(remoteRow);
        remoteRowLayout->setContentsMargins(0, 0, 0, 0);
        remoteRowLayout->setSpacing(14);
        auto *remoteAvatar = new QLabel(remoteRow);
        remoteAvatar->setObjectName("issueAvatar");
        remoteAvatar->setAlignment(Qt::AlignCenter);
        remoteAvatar->setFixedSize(36, 36);
        remoteAvatar->setPixmap(tintedOcticonPixmap(
            QStringLiteral("mastodon"), Theme::remapColor(QColor("#8b949e")), 20));
        remoteAvatar->setToolTip(QStringLiteral("Remote ActivityPub replies"));
        remoteRowLayout->addWidget(remoteAvatar, 0, Qt::AlignTop);
        remoteRowLayout->addWidget(remoteThread, 1);
        m_issueThreadLayout->addWidget(remoteRow);
    }
}

void MainWindow::showIssueBurnupChart()
{
    if (!m_issueDetailStack)
        return;

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);

    auto *title = new QLabel("Issue burn-up");
    title->setObjectName("issuePageTitle");
    auto *back = new QPushButton("Back to issue");
    back->setObjectName("ghostButton");
    back->setCursor(Qt::PointingHandCursor);
    setOcticon(back, "arrow-left", 16);
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->addWidget(title);
    titleRow->addStretch();
    titleRow->addWidget(back);
    layout->addLayout(titleRow);

    auto *description = new QLabel(
        "Open and closed totals reconstructed from issue creation and status "
        "events. List filters do not change the chart.");
    description->setObjectName("statusLine");
    description->setWordWrap(true);
    layout->addWidget(description);

    auto *rangeGroup = new QButtonGroup(page);
    rangeGroup->setExclusive(true);
    auto *rangeRow = new QHBoxLayout;
    rangeRow->setContentsMargins(0, 0, 0, 0);
    rangeRow->setSpacing(4);
    const QStringList rangeLabels{
        QStringLiteral("Day"), QStringLiteral("Week"),
        QStringLiteral("2 weeks"), QStringLiteral("Month"),
        QStringLiteral("All time")};
    for (int i = 0; i < rangeLabels.size(); ++i) {
        auto *button = new QPushButton(rangeLabels.at(i));
        button->setObjectName("repoTab");
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        rangeGroup->addButton(button, i);
        rangeRow->addWidget(button);
    }
    rangeRow->addStretch();
    layout->addLayout(rangeRow);

    auto *summary = new QLabel;
    summary->setTextFormat(Qt::RichText);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *legend = new QLabel(
        "<span style='color:#58a6ff;font-weight:700'>\xE2\x97\x8F Open</span>"
        "&nbsp;&nbsp;&nbsp;"
        "<span style='color:#3fb950;font-weight:700'>\xE2\x97\x8F Closed</span>");
    legend->setTextFormat(Qt::RichText);
    layout->addWidget(legend);

    auto *chart = new IssueBurnupChart(page);
    layout->addWidget(chart, 1);

    auto refreshChart = [this, chart, summary](int range) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        qint64 start = now - 24LL * 60 * 60 * 1000;
        int intervals = 24;
        QString rangeName = QStringLiteral("past day");
        if (range == 1) {
            start = now - 7LL * 24 * 60 * 60 * 1000;
            intervals = 28;
            rangeName = QStringLiteral("past week");
        } else if (range == 2) {
            start = now - 14LL * 24 * 60 * 60 * 1000;
            intervals = 28;
            rangeName = QStringLiteral("past 2 weeks");
        } else if (range == 3) {
            start = now - 30LL * 24 * 60 * 60 * 1000;
            intervals = 30;
            rangeName = QStringLiteral("past month");
        } else if (range == 4) {
            start = firstIssueHistoryTimestamp(
                m_currentIssues, now - 24LL * 60 * 60 * 1000);
            if (start >= now)
                start = now - 24LL * 60 * 60 * 1000;
            intervals = 60;
            rangeName = QStringLiteral("all time");
        }

        if (m_currentIssues.isEmpty()) {
            chart->setSeries({});
            summary->setText(
                QStringLiteral("No issues are available for the %1 range.")
                    .arg(rangeName));
            return;
        }

        const QList<IssueBurnupPoint> series =
            buildIssueBurnupSeries(m_currentIssues, start, now, intervals);
        chart->setSeries(series);
        const IssueBurnupPoint &first = series.first();
        const IssueBurnupPoint &last = series.last();
        const int firstTotal = first.openCount + first.closedCount;
        const int lastTotal = last.openCount + last.closedCount;
        auto signedNumber = [](int value) {
            return value > 0 ? QStringLiteral("+%1").arg(value)
                             : QString::number(value);
        };
        summary->setText(
            QStringLiteral(
                "<span style='font-size:22px;font-weight:800'>%1</span> open"
                "&nbsp;&nbsp;&nbsp;"
                "<span style='font-size:22px;font-weight:800'>%2</span> closed"
                "&nbsp;&nbsp;&nbsp;"
                "<span style='color:#8b949e'>%3 total &middot; %4 total and %5 "
                "net closed over the %6</span>")
                .arg(last.openCount)
                .arg(last.closedCount)
                .arg(lastTotal)
                .arg(signedNumber(lastTotal - firstTotal))
                .arg(signedNumber(last.closedCount - first.closedCount),
                     rangeName));
    };

    connect(rangeGroup, &QButtonGroup::idClicked, this, refreshChart);
    connect(back, &QPushButton::clicked, this,
            &MainWindow::removeIssueComposePage);
    rangeGroup->button(1)->setChecked(true);
    refreshChart(1);
    showIssueComposePage(page);
}

void MainWindow::showIssueComposePage(QWidget *page)
{
    if (!m_issueDetailStack || !page)
        return;
    removeIssueComposePage();
    auto *scroll = new QScrollArea;
    scroll->setObjectName("issueComposeScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    m_issueComposePage = scroll; // removeIssueComposePage deletes this (and page)
    m_issueDetailStack->addWidget(scroll);
    m_issueDetailStack->setCurrentWidget(scroll);
    if (m_issueDetail) {
        m_issueDetail->setVisible(true);
        m_issueDetail->raise();
    }
    if (m_issueDetailToggle)
        m_issueDetailToggle->setText("Hide detail");
}

void MainWindow::removeIssueComposePage()
{
    if (!m_issueDetailStack)
        return;
    if (m_issueComposePage) {
        QWidget *old = m_issueComposePage;
        m_issueComposePage = nullptr;
        m_issueDetailStack->setCurrentIndex(0);
        m_issueDetailStack->removeWidget(old);
        old->deleteLater();
    } else {
        m_issueDetailStack->setCurrentIndex(0);
    }
}

void MainWindow::setIssueInlineNotice(const QString &message, bool error,
                                      const QString &kind)
{
    showLocalNotice(m_issueInlineNotice, message, error, kind);
}

void MainWindow::promptEditIssueTitle()
{
    if (m_currentIssueNumber < 0)
        return;
    QString currentTitle;
    for (const Issue &issue : std::as_const(m_currentIssues)) {
        if (issue.number == m_currentIssueNumber) {
            currentTitle = issue.title;
            break;
        }
    }
    if (currentTitle.isEmpty() || !m_issueTitleEditor)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    m_issueTitleEditor->setText(currentTitle);
    m_issueTitle->hide();
    m_issueTitleEditButton->hide();
    m_issueTitleEditor->show();
    m_issueTitleSaveButton->show();
    m_issueTitleCancelButton->show();
    m_issueTitleEditor->setFocus();
    m_issueTitleEditor->selectAll();
}

void MainWindow::saveIssueTitleEdit()
{
    if (m_currentIssueNumber < 0 || !m_issueTitleEditor)
        return;
    const QString trimmed = m_issueTitleEditor->text().trimmed();
    if (trimmed.isEmpty()) {
        setIssueInlineNotice("A title is required.", true);
        return;
    }
    QString currentTitle;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.number == m_currentIssueNumber)
            currentTitle = issue.title;
    if (trimmed == currentTitle) {
        cancelIssueTitleEdit();
        return;
    }

    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.setTitle(m_currentIssueNumber, trimmed, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update the title." : error,
                             true);
        return;
    }
    cancelIssueTitleEdit();
    setIssueInlineNotice("Title updated.");
    reloadIssues();
}

void MainWindow::cancelIssueTitleEdit()
{
    if (m_issueTitle)
        m_issueTitle->show();
    if (m_issueTitleEditButton)
        m_issueTitleEditButton->show();
    if (m_issueTitleEditor)
        m_issueTitleEditor->hide();
    if (m_issueTitleSaveButton)
        m_issueTitleSaveButton->hide();
    if (m_issueTitleCancelButton)
        m_issueTitleCancelButton->hide();
}

void MainWindow::updateIssueActionState()
{
    const IssueStore store = issueStoreForCurrentRepo();
    const bool writable = store.canWrite();
    const bool haveIssue = m_currentIssueNumber >= 0;

    if (m_issueNewButton)
        m_issueNewButton->setEnabled(writable || issuesRepoIndex() >= 0);
    if (m_issueListNewButton)
        m_issueListNewButton->setEnabled(writable || issuesRepoIndex() >= 0);
    if (m_issueSyncButton)
        m_issueSyncButton->setEnabled(writable);
    refreshPendingInboxBadges();
    if (m_issueTitleEditButton)
        m_issueTitleEditButton->setEnabled(writable && haveIssue);
    if (m_issueTitleEditor)
        m_issueTitleEditor->setEnabled(writable && haveIssue);
    if (m_issueTitleSaveButton)
        m_issueTitleSaveButton->setEnabled(writable && haveIssue);
    if (m_issueTitleCancelButton)
        m_issueTitleCancelButton->setEnabled(haveIssue);
    if (!haveIssue || !writable)
        m_issueDeleteConfirmPending = false;
    for (QPushButton *b : {m_issueCloseButton, m_issueCloseCommentButton,
                           m_issueLabelsButton,
                           m_issueMilestoneButton, m_issuePriorityButton,
                           m_issuePriorityRaiseButton, m_issuePriorityLowerButton,
                           m_issueAssigneesButton,
                           m_issueAssigneesSelfButton, m_issueAssigneesSaveButton,
                           m_issueReporterButton, m_issueReporterSelfButton,
                           m_issueReporterSaveButton,
                           m_issueDeleteButton, m_issueAttachButton,
                           m_issueAssignAgentButton}) {
        if (b)
            b->setEnabled(writable && haveIssue);
    }
    if (m_issueMoveToTaskButton)
        m_issueMoveToTaskButton->setEnabled(writable && haveIssue &&
                                            !m_issueTaskMoveInFlight);
    // Assignment is host-only, so the whole control - gear, editor and the
    // sidebar's "Assign yourself" link alike - stays shut on a mirror, and says
    // why, rather than taking a selection Save cannot keep (issue #638).
    if (m_issueAssigneesEdit)
        m_issueAssigneesEdit->setEnabled(writable && haveIssue);
    if (m_issueAssigneesButton)
        m_issueAssigneesButton->setToolTip(
            writable ? QString() : kIssueAssigneesHostOnlyNotice);
    renderIssueAssigneesValue(m_issueAssigneesShown);
    // Re-crediting a report writes the same log, so the reporter control
    // follows the same host-only rule.
    if (m_issueReporterEdit)
        m_issueReporterEdit->setEnabled(writable && haveIssue);
    if (m_issueReporterButton)
        m_issueReporterButton->setToolTip(
            writable ? QString() : kIssueReporterHostOnlyNotice);
    renderIssueReporterValue(m_issueReporterShown);
    if (m_issueAgentProvider)
        m_issueAgentProvider->setEnabled(writable && haveIssue);
    if (m_issueAgentModel)
        m_issueAgentModel->setEnabled(writable && haveIssue);
    if (m_issueAgentSelector)
        m_issueAgentSelector->setEnabled(writable && haveIssue);
    if (m_issueAgentCreatePrCheck)
        m_issueAgentCreatePrCheck->setEnabled(writable && haveIssue);
    if (m_issueAgentViewButton)
        m_issueAgentViewButton->setEnabled(haveIssue &&
                                           latestAgentSessionForIssue(m_currentIssueNumber));
    // Comments work for everyone with an issue selected: owners write locally,
    // others submit a signed comment to the relay inbox.
    if (m_issueCommentButton)
        m_issueCommentButton->setEnabled(haveIssue);
    if (m_issueAskAiButton)
        m_issueAskAiButton->setEnabled(haveIssue);
    if (m_issueCopyButton)
        m_issueCopyButton->setEnabled(haveIssue);
    if (m_issueCopyAllButton)
        m_issueCopyAllButton->setEnabled(haveIssue);
    if (m_issueMcpPromptButton)
        m_issueMcpPromptButton->setEnabled(haveIssue);
    if (m_issueComposer) {
        m_issueComposer->setEnabled(haveIssue);
        m_issueComposer->setMentionCandidates(issueMentionCandidateNames());
    }
    updateVoteUi();

    if (haveIssue) {
        for (const Issue &issue : m_currentIssues) {
            if (issue.number == m_currentIssueNumber) {
                const bool open = issue.status != QLatin1String("closed");
                if (m_issueCloseButton)
                    m_issueCloseButton->setText(open ? "Close issue" : "Reopen");
                if (m_issueCloseCommentButton) {
                    m_issueCloseCommentButton->setEnabled(writable && open);
                    m_issueCloseCommentButton->setVisible(open);
                }
                break;
            }
        }
    }
    if (m_issueReadonlyNote) {
        m_issueReadonlyNote->setVisible(!writable && issuesRepoIndex() >= 0);
        m_issueReadonlyNote->setText(
            "You don't host this repository \xE2\x80\x94 new issues and comments are "
            "sent to the maintainer's inbox (text only) and sync back once they "
            "merge them. Edits stay owner-only.");
    }
    if (m_issueCommentButton)
        m_issueCommentButton->setText(writable ? "Comment" : "Send to maintainer");
}

void MainWindow::promptNewIssue()
{
    composeNewIssue(QString(), QString());
}

void MainWindow::promptIssueFromChatMessage(const QString &text,
                                            const QString &senderName,
                                            qint64 timestampMs)
{
    const QString message = text.trimmed();
    if (message.isEmpty())
        return;
    if (m_repositories.isEmpty()) {
        flashMessage("Open a repository before filing an issue from chat.");
        return;
    }

    int repoIndex = m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
                        ? m_repoDetailIndex
                        : 0;
    if (m_repositories.size() > 1) {
        QStringList labels;
        labels.reserve(m_repositories.size());
        for (const RepositoryRecord &repo : std::as_const(m_repositories))
            labels << repo.owner + "/" + repo.name;
        bool ok = false;
        const QString pick = QInputDialog::getItem(
            this, QStringLiteral("Create issue from message"),
            QStringLiteral("File this message as an issue in:"), labels,
            repoIndex, false, &ok);
        if (!ok || pick.isEmpty())
            return;
        repoIndex = labels.indexOf(pick);
        if (repoIndex < 0)
            return;
    }

    QString title = message.section('\n', 0, 0).simplified();
    if (title.size() > kIssueTitleLimit)
        title = title.left(kIssueTitleLimit - 1) +
                QString::fromUtf8("\xE2\x80\xA6");
    const QString when =
        QDateTime::fromMSecsSinceEpoch(timestampMs > 0
                                           ? timestampMs
                                           : QDateTime::currentMSecsSinceEpoch())
            .toUTC()
            .toString(Qt::ISODate);
    const QString channel = m_currentConversation.trimmed();
    const QString body =
        message + "\n\n---\nFiled from a " +
        (channel.isEmpty() ? QString() : channel + " ") + "chat message by " +
        (senderName.trimmed().isEmpty() ? QStringLiteral("someone")
                                        : senderName.trimmed()) +
        " at " + when + ".";

    if (repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    showSection(0);
    if (m_repoDetailTabs && m_repoDetailTabs->button(2))
        m_repoDetailTabs->button(2)->click();
    composeNewIssue(title, body);
}

bool MainWindow::createIssueFromChatCommand(const QString &argument)
{
    QString issueText = argument.trimmed();
    if (issueText.isEmpty()) {
        logSystem(QStringLiteral("Usage: /issue [owner/repo] <title>"));
        return false;
    }
    if (m_repositories.isEmpty()) {
        logSystem(QStringLiteral(
            "Chat: open a repository before using /issue."));
        return false;
    }

    int repoIndex =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
            ? m_repoDetailIndex
            : -1;
    const int separator = issueText.indexOf(QRegularExpression(QStringLiteral("\\s")));
    const QString firstToken =
        separator < 0 ? issueText : issueText.left(separator);
    static const QRegularExpression repoPattern(
        QStringLiteral("\\A[a-z][a-z0-9-]{0,62}/[A-Za-z0-9._-]{1,100}\\z"));
    const bool explicitRepository = repoPattern.match(firstToken).hasMatch();
    if (explicitRepository) {
        repoIndex = -1;
        for (int index = 0; index < m_repositories.size(); ++index) {
            const RepositoryRecord &candidate = m_repositories.at(index);
            if ((candidate.owner + QLatin1Char('/') + candidate.name)
                    .compare(firstToken, Qt::CaseInsensitive) == 0) {
                repoIndex = index;
                break;
            }
        }
        if (repoIndex < 0) {
            logSystem(QStringLiteral("Chat: repository %1 is not available.")
                          .arg(firstToken));
            return false;
        }
        issueText = issueText.mid(firstToken.size()).trimmed();
    }
    if (repoIndex < 0) {
        for (int index = 0; index < m_repositories.size(); ++index) {
            const RepositoryRecord &candidate = m_repositories.at(index);
            if (candidate.owner.compare(QStringLiteral("forkmesh"),
                                        Qt::CaseInsensitive) == 0 &&
                candidate.name.compare(QStringLiteral("forkmesh"),
                                       Qt::CaseInsensitive) == 0) {
                repoIndex = index;
                break;
            }
        }
    }
    if (repoIndex < 0)
        repoIndex = 0;
    if (issueText.isEmpty()) {
        logSystem(QStringLiteral("Usage: /issue [owner/repo] <title>"));
        return false;
    }

    QString title = issueText.section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.size() > 200)
        title = title.left(199) + QString::fromUtf8("\xE2\x80\xA6");
    const QString when = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    const QString channel = m_currentConversation.trimmed();
    const QString body =
        issueText + QStringLiteral("\n\n---\nFiled from a ") +
        (channel.isEmpty() ? QString() : channel + QLatin1Char(' ')) +
        QStringLiteral("chat message by ") +
        (chatDisplayName().trimmed().isEmpty()
             ? QStringLiteral("someone")
             : chatDisplayName().trimmed()) +
        QStringLiteral(" at ") + when + QLatin1Char('.');

    if (repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    const QString repositoryLabel = repo.owner + QLatin1Char('/') + repo.name;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        const bool started = submitNewIssueToInbox(
            title, body, {}, QString(), 0, {}, {}, {},
            [this, title, repositoryLabel](bool ok, const QString &error) {
                logSystem(ok
                              ? QStringLiteral("Chat: issue \"%1\" was signed and "
                                               "sent to %2.")
                                    .arg(title, repositoryLabel)
                              : QStringLiteral("Chat: could not file issue \"%1\" "
                                               "in %2 (%3).")
                                    .arg(title, repositoryLabel,
                                         error.isEmpty()
                                             ? QStringLiteral("request failed")
                                             : error));
            });
        if (!started)
            logSystem(QStringLiteral("Chat: could not reach %1 to file the issue.")
                          .arg(repositoryLabel));
        return started;
    }

    QString error;
    Issue created;
    const int number = store.createIssue(title, body, {}, QString(), 0, {}, {},
                                         &error, &created);
    if (number < 0) {
        logSystem(QStringLiteral("Chat: could not file the issue in %1 (%2).")
                      .arg(repositoryLabel,
                           error.isEmpty() ? QStringLiteral("write failed") : error));
        return false;
    }
    m_currentIssueNumber = number;
    appendCreatedIssue(store, created);
    propagateRepoUpdate(repoIndex);
    triggerTeammatesForEvent(
        QStringLiteral("new-issues"),
        QStringLiteral("Issue #%1 · %2").arg(number).arg(title));
    logSystem(QStringLiteral("Chat: created issue #%1 in %2: %3")
                  .arg(number)
                  .arg(repositoryLabel, title));
    return true;
}

void MainWindow::composeNewIssue(const QString &prefillTitle,
                                 const QString &prefillBody)
{
    if (issuesRepoIndex() < 0)
        return;

    auto *page = new QWidget;

    auto *titleLabel = new QLabel("Add a title <span style='color:#cf222e'>*</span>",
                                  page);
    titleLabel->setTextFormat(Qt::RichText);
    titleLabel->setObjectName("sectionLabel");
    auto *titleEdit = new QLineEdit(page);
    titleEdit->setPlaceholderText("Title");
    titleEdit->setText(prefillTitle);
    auto *bodyEdit = new MarkdownEditor(page);
    bodyEdit->setMentionCandidates(issueMentionCandidateNames());
    bodyEdit->setMinimumHeight(200);
    bodyEdit->setPlaceholderText("Type your description here...");
    if (!prefillBody.isEmpty())
        bodyEdit->setMarkdown(prefillBody);

    auto *left = new QWidget(page);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);
    leftLayout->addWidget(makeComposerIdentity(nullptr, QStringLiteral("Filing")));
    leftLayout->addWidget(titleLabel);
    leftLayout->addWidget(titleEdit);
    auto *descriptionLabel = new QLabel("Add a description", page);
    descriptionLabel->setObjectName("sectionLabel");
    leftLayout->addSpacing(8);
    leftLayout->addWidget(descriptionLabel);
    leftLayout->addWidget(bodyEdit, 1);
    auto *attachHint = new QLabel("Paste, drop, or click Image to add files", page);
    attachHint->setObjectName("statusLine");
    leftLayout->addWidget(attachHint);

    auto *sidebar = new QWidget(page);
    sidebar->setObjectName("issueComposeSidebar");
    sidebar->setMinimumWidth(190);
    sidebar->setMaximumWidth(285);
    sidebar->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(18, 2, 0, 0);
    sideLayout->setSpacing(8);

    auto addDivider = [&]() {
        auto *line = new QWidget(sidebar);
        line->setFixedHeight(1);
        line->setStyleSheet(Theme::remapSheet(QStringLiteral("background:%1;")
                                .arg(currentThemeIsDark() ? "#30363d" : "#d0d7de")));
        sideLayout->addWidget(line);
    };
    auto addSection = [&](const QString &label, QWidget *field) {
        auto *header = new QHBoxLayout;
        header->setContentsMargins(0, 0, 0, 0);
        auto *title = new QLabel(label, sidebar);
        title->setObjectName("sectionLabel");
        auto *gear = new QPushButton(sidebar);
        gear->setObjectName("ghostButton");
        gear->setProperty("buttonSize", "sm");
        gear->setFixedSize(28, 28);
        gear->setEnabled(false);
        setOcticon(gear, "gear", 14);
        header->addWidget(title);
        header->addStretch();
        header->addWidget(gear);
        sideLayout->addLayout(header);
        sideLayout->addWidget(field);
        sideLayout->addSpacing(6);
        addDivider();
        sideLayout->addSpacing(6);
    };

    auto *assigneeBox = new QWidget(sidebar);
    auto *assigneeLayout = new QVBoxLayout(assigneeBox);
    assigneeLayout->setContentsMargins(0, 0, 0, 0);
    assigneeLayout->setSpacing(6);
    auto *assigneesEdit = new QLineEdit(sidebar);
    assigneesEdit->setPlaceholderText("No one");
    auto *assignSelf = new QPushButton("Assign yourself", sidebar);
    assignSelf->setObjectName("ghostButton");
    assignSelf->setProperty("buttonSize", "sm");
    assignSelf->setCursor(Qt::PointingHandCursor);
    connect(assignSelf, &QPushButton::clicked, this, [this, assigneesEdit] {
        const QString who = m_userName.trimmed();
        if (who.isEmpty())
            return;
        QStringList assignees = splitIssueFieldList(assigneesEdit->text());
        if (!assignees.contains(who))
            assignees << who;
        assigneesEdit->setText(assignees.join(", "));
    });
    assigneeLayout->addWidget(assigneesEdit);
    assigneeLayout->addWidget(assignSelf, 0, Qt::AlignLeft);
    addSection("Assignees", assigneeBox);

    auto *labelsEdit = new QLineEdit(sidebar);
    labelsEdit->setPlaceholderText("No labels");
    addSection("Labels", labelsEdit);

    auto *typeValue = new QLabel("No type", sidebar);
    typeValue->setObjectName("statusLine");
    addSection("Type", typeValue);

    auto *priorityCombo = new QSpinBox(sidebar);
    priorityCombo->setRange(0, 8999);
    priorityCombo->setSpecialValueText("No priority");
    priorityCombo->setToolTip("1 is highest priority; 8999 is lowest");
    addSection("Priority", priorityCombo);

    auto *projectsValue = new QLabel("No projects", sidebar);
    projectsValue->setObjectName("statusLine");
    addSection("Projects", projectsValue);

    auto *milestoneCombo = new QComboBox(sidebar);
    milestoneCombo->addItem("No milestone", QString());
    for (const IssueMilestone &ms : m_currentMilestones)
        milestoneCombo->addItem(ms.title, ms.title);
    addSection("Milestone", milestoneCombo);
    sideLayout->addStretch();

    auto *content = new QHBoxLayout;
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(22);
    content->addWidget(left, 1);
    content->addWidget(sidebar);

    auto *createMore = new QCheckBox("Create more", page);
    auto *cancelButton = new QPushButton("Cancel", page);
    cancelButton->setObjectName("ghostButton");
    cancelButton->setCursor(Qt::PointingHandCursor);
    auto *createButton = new QPushButton("Create", page);
    createButton->setObjectName("primaryButton");
    createButton->setCursor(Qt::PointingHandCursor);
    createButton->setToolTip("Create the issue (Ctrl+Enter)");
    setOcticon(createButton, "issue-opened", 16);
    auto *pageNotice = new QLabel(page);
    pageNotice->setObjectName("issueInlineNotice");
    pageNotice->setWordWrap(true);
    pageNotice->hide();
    auto setPageNotice = [pageNotice](const QString &message, bool error = false) {
        if (message.trimmed().isEmpty()) {
            pageNotice->clear();
            pageNotice->hide();
            return;
        }
        const bool dark = currentThemeIsDark();
        const QString bg = error ? (dark ? "#3d1f21" : "#ffebe9")
                                 : (dark ? "#11251a" : "#dafbe1");
        const QString border = error ? (dark ? "#f85149" : "#cf222e")
                                     : (dark ? "#2ea043" : "#1f883d");
        const QString fg = dark ? "#e6edf3" : "#1f2328";
        pageNotice->setStyleSheet(Theme::remapSheet(
            QStringLiteral("QLabel#issueInlineNotice { background-color:%1; color:%2; "
                           "border:1px solid %3; border-radius:6px; padding:8px 10px; }")
                .arg(bg, fg, border)));
        pageNotice->setText(message.toHtmlEscaped());
        pageNotice->show();
    };
    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addStretch();
    buttonRow->addWidget(createMore);
    buttonRow->addSpacing(18);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(createButton);

    auto *pageLayout = new QVBoxLayout(page);
    pageLayout->setContentsMargins(22, 18, 16, 14);
    pageLayout->setSpacing(14);
    pageLayout->addLayout(content, 1);
    pageLayout->addWidget(pageNotice);
    pageLayout->addLayout(buttonRow);
    connect(cancelButton, &QPushButton::clicked, this,
            &MainWindow::removeIssueComposePage);
    connect(createButton, &QPushButton::clicked, this, [=] {
        if (titleEdit->text().trimmed().isEmpty()) {
            setPageNotice("A title is required.", true);
            return;
        }

        const QString title = titleEdit->text().trimmed();
        const QStringList labels = splitIssueFieldList(labelsEdit->text());
        const QStringList assignees = splitIssueFieldList(assigneesEdit->text());
        const QString milestone = milestoneCombo->currentData().toString();
        const int priority = priorityCombo->value();

        IssueStore store = issueStoreForCurrentRepo();
        // On a mirror (no work tree) we can't write the issue locally, so send a
        // signed "open" event to the source of truth's inbox; the owner merges it
        // into.forkmesh/issues/ preserving us as the author, and it syncs back to mirrors.
        if (!store.canWrite()) {
            const QPointer<QWidget> pageGuard(page);
            createButton->setEnabled(false);
            setPageNotice("Sending your issue to the maintainer's inbox...");
            const bool started = submitNewIssueToInbox(
                title, bodyEdit->markdown(), labels, milestone, priority,
                assignees, bodyEdit->pendingAttachments(),
                bodyEdit->pendingAttachmentPlaceholders(),
                [this, pageGuard, createButton, setPageNotice](bool ok,
                                                                const QString &error) {
                    if (!pageGuard) {
                        if (ok)
                            setIssueInlineNotice(
                                "Your signed issue was sent to the maintainer's "
                                "inbox. It appears once they sync it.");
                        return;
                    }
                    if (ok) {
                        removeIssueComposePage();
                        setIssueInlineNotice("Your signed issue was sent to the "
                                             "maintainer's inbox. It appears once "
                                             "they sync it.");
                    } else {
                        createButton->setEnabled(true);
                        setPageNotice(error.isEmpty()
                                          ? "Could not send the issue."
                                          : "Could not send the issue: " + error,
                                      true);
                    }
                });
            if (!started) {
                createButton->setEnabled(true);
                setPageNotice("Open a repository you can reach to file an issue.",
                              true);
            }
            return;
        }

        QString error;
        Issue created;
        const int number = store.createIssue(title, bodyEdit->markdown(), labels,
                                             milestone, priority, assignees,
                                             bodyEdit->pendingAttachments(),
                                             bodyEdit->pendingAttachmentPlaceholders(),
                                             &error, &created);
        if (number < 0) {
            setPageNotice(error.isEmpty() ? "Could not create the issue." : error,
                          true);
            return;
        }
        m_currentIssueNumber = number;
        const bool more = createMore->isChecked();
        removeIssueComposePage();
        appendCreatedIssue(store, created);
        const int targetRepoIndex = issuesRepoIndex();
        propagateRepoUpdate(targetRepoIndex);
        if (targetRepoIndex == m_repoDetailIndex)
            triggerTeammatesForEvent(
                QStringLiteral("new-issues"),
                QStringLiteral("Issue #%1 · %2").arg(number).arg(title));
        setIssueInlineNotice("Issue created.");
        // An "@fable fix this" in the body of a brand new issue is answered
        // the same way one in a comment is.
        scanCurrentRepoModelMentions();
        if (more)
            promptNewIssue();
    });

    auto *submitShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), page);
    submitShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(submitShortcut, &QShortcut::activated, createButton, &QPushButton::click);

    showIssueComposePage(page);
    titleEdit->setFocus();
}

// The footer prompt's ActivityPub action uses the same signed endpoint as
// release announcements, but authenticates the visible user actor rather than
// assuming they own the open repository. It deliberately sends `manual: true`:
// otherwise the relay treats this as an automatic repository event and holds
// it for the daily digest instead of publishing the explicit post immediately.
void MainWindow::publishQuickAddToActivityPub(const QString &prompt)
{
    if (prompt.isEmpty()) {
        setIssueInlineNotice(
            QStringLiteral("Add prompt text before posting to ActivityPub."), true);
        return;
    }
    if (!m_quickAddImages.isEmpty()) {
        // The repository ActivityPub endpoint accepts inline data images, but
        // the footer queue holds local paths. Refuse rather than silently
        // losing an attachment or leaking a local path into a public post.
        setIssueInlineNotice(
            QStringLiteral("ActivityPub prompt posts cannot include attachments "
                           "yet. Remove them before posting."), true);
        return;
    }

    const int repoIndex = issuesRepoIndex();
    if (repoIndex < 0 || repoIndex >= m_repositories.size()) {
        setIssueInlineNotice(
            QStringLiteral("Open a repository before posting to ActivityPub."), true);
        return;
    }
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    if (!repo.publishToNetwork) {
        setIssueInlineNotice(
            QStringLiteral("Publish this repository to the network before "
                           "posting to ActivityPub."), true);
        return;
    }

    const QString owner = repoSegment(repo.owner, QStringLiteral("owner"));
    const QString name = repoSegment(repo.name, QStringLiteral("repository"));
    const QString signer = accountOwner().trimmed().toLower();
    const QString author = topBarUserName().trimmed().toLower();
    // A manual prompt is a post by the signed-in person, not an assertion that
    // they own the repository currently open in the UI. The relay verifies the
    // signer and only includes the repository actor when that same proof also
    // carries repository-owner authority.
    if (!m_networkAccess || author.isEmpty() ||
        !hasOwnerSigningCapability(signer)) {
        setIssueInlineNotice(
            QStringLiteral("Sign in with this device before posting to "
                           "ActivityPub."), true);
        return;
    }

    // Match the issue composer convention: a first line acts as the compact
    // headline and the rest becomes the post's detail. ActivityPub's event
    // renderer caps those fields independently, so this keeps the most useful
    // beginning visible instead of duplicating the entire prompt in both.
    const int firstNewline = prompt.indexOf(QLatin1Char('\n'));
    QString title = (firstNewline < 0 ? prompt : prompt.left(firstNewline)).trimmed();
    QString body = firstNewline < 0 ? QString() : prompt.mid(firstNewline + 1).trimmed();
    if (title.isEmpty()) {
        title = QStringLiteral("Update");
        body = prompt;
    }

    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/ap-publish").arg(owner, name));
    url.setQuery(signedInboxQuery(signer));
    const QJsonObject payload{
        {QStringLiteral("kind"), QStringLiteral("discussion")},
        {QStringLiteral("eventType"), QStringLiteral("open")},
        {QStringLiteral("title"), title.left(240)},
        {QStringLiteral("body"), body.left(4000)},
        {QStringLiteral("author"), author},
        {QStringLiteral("manual"), true},
    };
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));

    QPointer<QPlainTextEdit> inputGuard(m_issueQuickAdd);
    QPointer<QPushButton> sendGuard(m_quickAddSendButton);
    if (inputGuard)
        inputGuard->setEnabled(false);
    if (sendGuard)
        sendGuard->setEnabled(false);
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (!reply) {
        if (inputGuard)
            inputGuard->setEnabled(true);
        if (sendGuard)
            sendGuard->setEnabled(true);
        setIssueInlineNotice(QStringLiteral("Could not start the ActivityPub post."),
                             true);
        return;
    }

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, inputGuard, sendGuard, owner, name, author] {
                const QByteArray response = reply->readAll();
                const bool posted = reply->error() == QNetworkReply::NoError;
                const QString error = posted ? QString()
                                             : apiErrorSummary(reply, response);
                reply->deleteLater();
                if (inputGuard)
                    inputGuard->setEnabled(true);
                if (sendGuard)
                    sendGuard->setEnabled(true);
                if (!posted) {
                    setIssueInlineNotice(
                        QStringLiteral("Could not post to ActivityPub%1")
                            .arg(error.isEmpty() ? QStringLiteral(".")
                                                 : QStringLiteral(": ") + error),
                        true);
                    return;
                }
                if (inputGuard)
                    inputGuard->clear();
                setIssueInlineNotice(
                    QStringLiteral("Published to ActivityPub as @%1 for %2/%3.")
                        .arg(author, owner, name));
            });
}

void MainWindow::quickAddIssue()
{
    if (!m_issueQuickAdd || submitQuickAddToPackage())
        return;
    const QString title = m_issueQuickAdd->toPlainText().trimmed();
    if (title.isEmpty() && m_quickAddImages.isEmpty())
        return;
    if (!title.isEmpty())
        recordQuickAddHistory(title);

    const QString quickAddProvider =
        m_quickAddAgentProvider
            ? m_quickAddAgentProvider->currentData().toString()
            : QStringLiteral("claude-code");
    if (quickAddProvider == kActivityPubPromptProvider) {
        publishQuickAddToActivityPub(title);
        return;
    }
    if (quickAddProvider == kNotePromptProvider) {
        takeQuickAddNote(m_issueQuickAdd->toPlainText());
        return;
    }
    if (quickAddProvider == kTaskPromptProvider) {
        createQuickAddOrganizationTask();
        return;
    }
    if (quickAddProvider != QLatin1String("manual")) {
        const QString provider = quickAddProvider;
        const QString model = (provider == QLatin1String("claude-code") ||
                               agentIsCodexProvider(provider) ||
                               agentIsCloudflareAiProvider(provider) ||
                               agentIsLocalLlamaProvider(provider))
                                  ? selectedModelComboValue(m_quickAddClaudeModel)
                                  : QString();
        const bool createPr = m_quickAddCreatePr && m_quickAddCreatePr->isChecked();
        const QStringList images = m_quickAddImages;
        QString prompt = title;
        for (const QString &attachment : m_quickAddImages) {
            if (!prompt.endsWith(QLatin1Char('\n')))
                prompt += QLatin1Char('\n');
            prompt += promptAttachmentLine(attachment);
        }
        if (m_promptCloudButton && m_promptCloudButton->isChecked()) {
            if (!images.isEmpty()) {
                flashMessage(QStringLiteral("Attached files stay on this "
                                            "desktop. Remove them, or untick "
                                            "the cloud to run here."),
                             true);
                return;
            }
            if (startQuickAddCloudAgent(issuesRepoIndex(), title, provider,
                                        model, createPr))
                m_issueQuickAdd->clear();
            return;
        }
        const int agentSessionId = startAdHocAgentForRepo(
            issuesRepoIndex(), prompt, provider, createPr, model, QString(),
            /*genie=*/false, /*switchToTab=*/false, QString(), QString(),
            quickAddAgentBaseBranchForGitView(), /*priority=*/50,
            /*directCheckout=*/m_promptWorktreeCheck &&
                !m_promptWorktreeCheck->isChecked(),
            QString(), m_promptVibeCheck && m_promptVibeCheck->isChecked());
        if (agentSessionId > 0) {
            m_issueQuickAdd->clear();
            clearQuickAddImages();
            // Working straight in the checkout is a one-prompt choice: re-tick
            // the box so the next prompt is isolated again unless unticked.
            if (m_promptWorktreeCheck)
                m_promptWorktreeCheck->setChecked(true);
            // No issue exists in this mode (that's the point of it), so saying
            // "no issue created" is just noise — show what actually happened
            // instead: which agent, model, and permission mode picked up the
            // prompt.
            QStringList details;
            const QString modelLabel = agentModelLabel(model);
            if (!modelLabel.isEmpty())
                details << modelLabel;
            if (m_quickAddModeSelector)
                details << m_quickAddModeSelector->currentText();
            const QString suffix =
                details.isEmpty()
                    ? QString()
                    : QStringLiteral(" (%1)").arg(details.join(QStringLiteral(", ")));
            const QString startedMessage =
                QStringLiteral("Started a %1 agent on your prompt%2.")
                    .arg(agentProviderName(provider), suffix);
            logSystem(startedMessage);
            flyPromptSparkleToAgents();
        }
        return;
    }

    IssueStore store = issueStoreForCurrentRepo();
    if (title.isEmpty()) {
        flashMessage(QStringLiteral("Add prompt text before creating an issue."), true);
        return;
    }
    if (!store.canWrite()) {
        if (issuesRepoIndex() < 0) {
            setIssueInlineNotice("Pick a repository to add issues.", true);
            return;
        }
        QPointer<QPlainTextEdit> quickAddGuard(m_issueQuickAdd);
        quickAddGuard->setEnabled(false);
        const bool started = submitNewIssueToInbox(
            title, QString(), {}, QString(), 0, {}, m_quickAddImages, {},
            [this, quickAddGuard, title](bool ok, const QString &) {
                if (!quickAddGuard)
                    return;
                quickAddGuard->setEnabled(true);
                if (ok) {
                    quickAddGuard->clear();
                    clearQuickAddImages();
                    setIssueInlineNotice("Your signed issue was sent to the "
                                         "maintainer's inbox. It appears once "
                                         "they sync it.");
                } else {
                    quickAddGuard->setPlainText(title);
                }
            });
        if (!started)
            quickAddGuard->setEnabled(true);
        return;
    }
    QString error;
    Issue created;
    const int number = store.createIssue(title, QString(), {}, QString(), 0, {},
                                         m_quickAddImages, &error, &created);
    if (number < 0) {
        setIssueInlineNotice(error.isEmpty() ? "Could not create the issue." : error,
                             true);
        return;
    }
    m_issueQuickAdd->clear();
    clearQuickAddImages();
    m_currentIssueNumber = number;
    appendCreatedIssue(store, created);
    const int targetRepoIndex = issuesRepoIndex();
    propagateRepoUpdate(targetRepoIndex);
    if (targetRepoIndex == m_repoDetailIndex)
        triggerTeammatesForEvent(
            QStringLiteral("new-issues"),
            QStringLiteral("Issue #%1 · %2").arg(number).arg(title));
    setIssueInlineNotice(QStringLiteral("Issue #%1 created: %2").arg(number).arg(title));
    showIssue(number);
}

QString MainWindow::quickAddAgentBaseBranchForGitView() const
{
    // The standard Git workspace always shows the default branch. Its branch
    // selection is applied asynchronously, so m_repoBranch may still name the
    // previously browsed branch while this page is visibly loading main.
    if (!m_railGitButton || !m_railGitButton->isChecked() ||
        sourceControlShowsRange() || m_repoDetailIndex < 0 ||
        issuesRepoIndex() != m_repoDetailIndex)
        return QString();
    return repoDefaultBranchFast().trimmed();
}

#ifdef FORKMESH_WINDOW_TESTS
int MainWindow::testQuickAddIssueNoAgent(const QString &title)
{
    if (!m_issueQuickAdd)
        return -1;
    if (m_quickAddAgentProvider) {
        const int idx = m_quickAddAgentProvider->findData(QStringLiteral("manual"));
        if (idx >= 0)
            m_quickAddAgentProvider->setCurrentIndex(idx);
    }
    m_issueQuickAdd->setPlainText(title);
    quickAddIssue();
    return m_currentIssueNumber;
}

int MainWindow::testQuickAddAgent(const QString &prompt)
{
    if (!m_issueQuickAdd)
        return -1;
    QSet<int> existingIds;
    for (const AgentSession &session : std::as_const(m_agentSessions))
        existingIds.insert(session.id);
    if (m_quickAddAgentProvider) {
        const int index =
            m_quickAddAgentProvider->findData(QStringLiteral("claude-code"));
        if (index >= 0)
            m_quickAddAgentProvider->setCurrentIndex(index);
    }
    m_issueQuickAdd->setPlainText(prompt);
    quickAddIssue();
    for (auto it = m_agentSessions.crbegin(); it != m_agentSessions.crend(); ++it)
        if (!existingIds.contains(it->id))
            return it->id;
    return -1;
}
#endif

void MainWindow::recordQuickAddHistory(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return;
    if (m_quickAddHistory.isEmpty() || m_quickAddHistory.last() != t)
        m_quickAddHistory.append(t);
    constexpr int kMaxQuickAddHistory = 50;
    while (m_quickAddHistory.size() > kMaxQuickAddHistory)
        m_quickAddHistory.removeFirst();
    m_quickAddHistoryIndex = -1;
    m_quickAddDraft.clear();
    QSettings().setValue(kQuickAddHistorySetting, m_quickAddHistory);
}

void MainWindow::saveQuickAddDraft()
{
    if (m_quickAddDraftSaveTimer)
        m_quickAddDraftSaveTimer->stop();
    if (!m_quickAddDraftDirty)
        return;
    m_quickAddDraftDirty = false;
    if (!PromptDraftStore::store(m_quickAddPendingDraft))
        logSystem(QStringLiteral("Could not save the prompt draft to %1")
                      .arg(QDir::toNativeSeparators(PromptDraftStore::path())));
}

bool MainWindow::navigateQuickAddHistory(int direction)
{
    if (!m_issueQuickAdd || m_quickAddHistory.isEmpty())
        return false;
    auto showText = [this](const QString &text) {
        m_quickAddHistoryNavigating = true;
        m_issueQuickAdd->setPlainText(text);
        m_issueQuickAdd->moveCursor(QTextCursor::End);
        m_quickAddHistoryNavigating = false;
    };
    const int count = m_quickAddHistory.size();
    if (direction < 0) { // Up: step toward older prompts
        if (m_quickAddHistoryIndex < 0) {
            m_quickAddDraft = m_issueQuickAdd->toPlainText();
            m_quickAddHistoryIndex = count - 1;
        } else if (m_quickAddHistoryIndex > 0) {
            --m_quickAddHistoryIndex;
        } else {
            return true; // already at the oldest entry; swallow the key
        }
        showText(m_quickAddHistory.at(m_quickAddHistoryIndex));
        return true;
    }
    if (m_quickAddHistoryIndex < 0)
        return false; // not navigating; let the field handle the key
    if (m_quickAddHistoryIndex < count - 1) {
        ++m_quickAddHistoryIndex;
        showText(m_quickAddHistory.at(m_quickAddHistoryIndex));
    } else {
        m_quickAddHistoryIndex = -1;
        showText(m_quickAddDraft);
    }
    return true;
}

void MainWindow::attachQuickAddMedia()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("Attach image or video"), QString(),
        promptAttachmentFileFilter());
    for (const QString &f : files)
        queueQuickAddImage(f);
    if (m_issueQuickAdd)
        m_issueQuickAdd->setFocus();
}

bool MainWindow::tryPasteImageIntoQuickAdd()
{
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    if (!mime || !mime->hasImage())
        return false;
    const QString path =
        saveNewAgentPromptImage(qvariant_cast<QImage>(mime->imageData()));
    if (path.isEmpty())
        return false;
    queueQuickAddImage(path);
    return true;
}

void MainWindow::queueQuickAddImage(const QString &path)
{
    if (path.isEmpty() || m_quickAddImages.contains(path))
        return;
    m_quickAddImages.append(path);
    updateQuickAddImageButton();

    auto *row = m_quickAddAttachStrip
                    ? qobject_cast<QHBoxLayout *>(m_quickAddAttachStrip->layout())
                    : nullptr;
    QWidget *chip = row && row->count() > 0
                        ? row->itemAt(row->count() - 1)->widget()
                        : nullptr;
    if (!chip)
        return;
    const int fullWidth = chip->sizeHint().width();
    const QColor accent = Theme::nextOr(Theme::Role::AccentFg, QColor("#2f81f7"));
    const QColor restFill = Theme::remapColor(QColor("#21262d"));
    const QColor restBorder = Theme::remapColor(QColor("#30363d"));
    const auto mix = [](const QColor &from, const QColor &to, qreal k) {
        return QColor::fromRgbF(from.redF() + (to.redF() - from.redF()) * k,
                                from.greenF() + (to.greenF() - from.greenF()) * k,
                                from.blueF() + (to.blueF() - from.blueF()) * k);
    };
    auto *fade = new QGraphicsOpacityEffect(chip);
    fade->setOpacity(0.0);
    chip->setGraphicsEffect(fade);
    chip->setMaximumWidth(0);

    auto *anim = new QVariantAnimation(chip);
    anim->setStartValue(0.0);
    anim->setEndValue(1.0);
    anim->setDuration(760);
    connect(anim, &QVariantAnimation::valueChanged, chip,
            [chip, fade, fullWidth, accent, restFill, restBorder, mix](const QVariant &value) {
                const qreal t = value.toReal();
                const qreal open = QEasingCurve(QEasingCurve::OutCubic)
                                       .valueForProgress(qMin<qreal>(1.0, t / 0.4));
                chip->setMaximumWidth(qRound(fullWidth * open));
                fade->setOpacity(open);
                const qreal glow =
                    t < 0.25 ? t / 0.25
                             : QEasingCurve(QEasingCurve::InOutQuad)
                                   .valueForProgress(1.0 - (t - 0.25) / 0.75);
                chip->setStyleSheet(
                    QStringLiteral("QWidget#quickAddAttachChip{background:%1;"
                                   "border:1px solid %2;border-radius:4px;}")
                        .arg(mix(restFill, accent, 0.28 * glow).name(),
                             mix(restBorder, accent, glow).name()));
            });
    connect(anim, &QVariantAnimation::finished, chip, [chip]() {
        chip->setMaximumWidth(QWIDGETSIZE_MAX);
        chip->setGraphicsEffect(nullptr);
        chip->setStyleSheet(QString());
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::removeQuickAddImage(const QString &path)
{
    if (!m_quickAddImages.removeOne(path))
        return;
    updateQuickAddImageButton();
    setIssueInlineNotice(
        m_quickAddImages.isEmpty()
            ? QStringLiteral("Attachment removed.")
            : QStringLiteral("Attachment removed (%1 left).")
                  .arg(m_quickAddImages.size()));
}

void MainWindow::clearQuickAddImages()
{
    if (m_quickAddImages.isEmpty())
        return;
    m_quickAddImages.clear();
    updateQuickAddImageButton();
}

void MainWindow::updateQuickAddImageButton()
{
    rebuildQuickAddAttachChips();
    if (!m_quickAddImageButton)
        return;
    const int n = m_quickAddImages.size();
    m_quickAddImageButton->setText(n > 0 ? QString::number(n) : QString());
    if (n == 0) {
        m_quickAddImageButton->setToolTip(
            QString::fromUtf8("Attach an image or video \xE2\x80\x94 pick a file or "
                           "paste an image with Ctrl+V. Sent to the agent in "
                           "\"No issue\" mode, or attached to the created issue."));
        return;
    }
    QStringList names;
    for (const QString &p : m_quickAddImages)
        names << QFileInfo(p).fileName();
    m_quickAddImageButton->setToolTip(
        QStringLiteral("%1 attachment%2:\n%3\n\nClick to add more.")
            .arg(n)
            .arg(n == 1 ? QString() : QStringLiteral("s"), names.join(QLatin1Char('\n'))));
}

void MainWindow::rebuildQuickAddAttachChips()
{
    if (!m_quickAddAttachStrip)
        return;
    auto *row = qobject_cast<QHBoxLayout *>(m_quickAddAttachStrip->layout());
    if (!row)
        return;
    while (QLayoutItem *item = row->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    // The chip rules live on the strip, applied once, instead of a stylesheet
    // on every chip, thumbnail and remove button at each rebuild. A parent's
    // sheet still outranks the application sheet, so the look is unchanged.
    static const QString kAttachStripStyle = QStringLiteral(
        "QWidget#quickAddAttachChip{background:#21262d;border:1px solid #30363d;"
        "border-radius:4px;}"
        "QToolButton#quickAddAttachThumb{border:none;background:transparent;padding:0;"
        "color:#c9d1d9;}"
        "QPushButton#quickAddAttachRemove{color:#8b949e;border:none;"
        "background:transparent;font-size:13px;font-weight:bold;padding:0;}"
        "QPushButton#quickAddAttachRemove:hover{color:#f85149;}");
    if (m_quickAddAttachStrip->styleSheet() != kAttachStripStyle)
        m_quickAddAttachStrip->setStyleSheet(Theme::remapSheet(kAttachStripStyle));

    for (const QString &path : std::as_const(m_quickAddImages)) {
        auto *chip = new QWidget(m_quickAddAttachStrip);
        chip->setObjectName("quickAddAttachChip");
        chip->setToolTip(QFileInfo(path).fileName());
        auto *chipRow = new QHBoxLayout(chip);
        chipRow->setContentsMargins(2, 1, 1, 1);
        chipRow->setSpacing(2);

        // A video has no frame to draw here, so it wears a play glyph and its
        // file name instead of an empty button. Both open on click.
        const bool video = isVideoAttachmentPath(path);
        const QPixmap pm = video ? QPixmap() : QPixmap(path);
        if (!pm.isNull() || video) {
            auto *thumb = new QToolButton(chip);
            thumb->setObjectName("quickAddAttachThumb");
            if (video) {
                thumb->setText(QString::fromUtf8("\xE2\x96\xB6  ") +
                               QFileInfo(path).fileName());
                thumb->setToolTip(QStringLiteral("Click to play"));
            } else {
                thumb->setIcon(QIcon(pm));
                thumb->setIconSize(QSize(18, 18));
                thumb->setToolTip(QStringLiteral("Click to view full size"));
            }
            thumb->setAutoRaise(true);
            thumb->setCursor(Qt::PointingHandCursor);
            connect(thumb, &QToolButton::clicked, this,
                    [this, path]() { showQuickAddImageDetail(path); });
            chipRow->addWidget(thumb);
        } else {
            auto *thumb = new QLabel(QFileInfo(path).fileName(), chip);
            chipRow->addWidget(thumb);
        }

        auto *remove = new QPushButton(QString::fromUtf8("\xC3\x97"), chip);
        remove->setObjectName("quickAddAttachRemove");
        remove->setCursor(Qt::PointingHandCursor);
        remove->setFixedSize(14, 14);
        remove->setToolTip(QStringLiteral("Remove this attachment"));
        connect(remove, &QPushButton::clicked, this,
                [this, path]() { removeQuickAddImage(path); });
        chipRow->addWidget(remove);

        row->addWidget(chip);
    }
    m_quickAddAttachStrip->setVisible(!m_quickAddImages.isEmpty());
}

void MainWindow::showQuickAddImageDetail(const QString &path)
{
    if (isVideoAttachmentPath(path)) {
        showAttachedVideoDetail(path);
        return;
    }
    QPixmap pixmap(path);
    if (pixmap.isNull())
        return;

    auto *dialog = new QDialog(this);
    dialog->setObjectName("imageDetailDialog");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QFileInfo(path).fileName());

    QSize maxSize(1200, 800);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize avail = screen->availableSize();
        maxSize = QSize(avail.width() * 9 / 10, avail.height() * 9 / 10);
    }
    QPixmap shown = pixmap;
    if (pixmap.width() > maxSize.width() || pixmap.height() > maxSize.height())
        shown = pixmap.scaled(maxSize, Qt::KeepAspectRatio,
                              Qt::SmoothTransformation);

    auto *imageLabel = new QLabel;
    imageLabel->setAlignment(Qt::AlignCenter);
    imageLabel->setPixmap(shown);

    auto *scroll = new QScrollArea;
    scroll->setObjectName("messageView");
    scroll->setWidgetResizable(true);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setWidget(imageLabel);

    auto *closeButton = new QPushButton(QStringLiteral("Close"));
    closeButton->setObjectName("primaryButton");
    closeButton->setCursor(Qt::PointingHandCursor);
    connect(closeButton, &QPushButton::clicked, dialog, &QDialog::accept);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->addWidget(new QLabel(
        QString::fromUtf8("%1 \xC3\x97 %2").arg(pixmap.width()).arg(pixmap.height())));
    buttonRow->addStretch();
    buttonRow->addWidget(closeButton);

    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);
    layout->addWidget(scroll, 1);
    layout->addLayout(buttonRow);

    dialog->resize(qMin(shown.width() + 48, maxSize.width()),
                   qMin(shown.height() + 96, maxSize.height()));
    dialog->show();
}

void MainWindow::copyIssueToClipboard()
{
    if (m_currentIssueNumber < 0)
        return;
    const Issue *issue = nullptr;
    for (const Issue &candidate : m_currentIssues)
        if (candidate.number == m_currentIssueNumber)
            issue = &candidate;
    if (!issue)
        return;

    QApplication::clipboard()->setText(issue->title);
    setIssueInlineNotice("Issue title copied.");
}

void MainWindow::copyIssueThreadToClipboard()
{
    if (m_currentIssueNumber < 0)
        return;
    const Issue *issue = nullptr;
    for (const Issue &candidate : m_currentIssues)
        if (candidate.number == m_currentIssueNumber)
            issue = &candidate;
    if (!issue)
        return;

    QHash<QString, IssueEvent> edits;
    QSet<QString> deleted;
    for (const IssueEvent &ev : issue->events) {
        if (ev.type == QLatin1String("edit") && !ev.target.isEmpty())
            edits.insert(ev.target, ev);
        else if (ev.type == QLatin1String("delete") && !ev.target.isEmpty() &&
                 ev.target != QLatin1String("self"))
            deleted.insert(ev.target);
    }

    QStringList parts;
    parts << QStringLiteral("#%1 %2").arg(issue->number).arg(issue->title);
    parts << QStringLiteral("Status: %1").arg(issue->status);

    int commentCount = 0;
    for (const IssueEvent &ev : issue->events) {
        const bool isOpen = ev.type == QLatin1String("open");
        const bool isComment = ev.type == QLatin1String("comment");
        if (!isOpen && !isComment)
            continue;
        if (isComment && deleted.contains(ev.id))
            continue;
        QString body = edits.contains(ev.id) ? edits.value(ev.id).body : ev.body;
        const QString who =
            ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
        const QString when = QDateTime::fromMSecsSinceEpoch(
                                 issueDisplayTimestampMs(ev.ts))
                                 .toString(
            QStringLiteral("yyyy-MM-dd HH:mm"));
        parts << QString()
              << QStringLiteral("--- %1 %2 (%3) ---")
                     .arg(who,
                          isOpen ? QStringLiteral("opened")
                                 : QStringLiteral("commented"),
                          when)
              << body.trimmed();
        if (isComment)
            ++commentCount;
    }

    QApplication::clipboard()->setText(parts.join(QChar('\n')));
    setIssueInlineNotice(QStringLiteral("Issue and %1 comment%2 copied.")
                             .arg(commentCount)
                             .arg(commentCount == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::showIssueAiTyping()
{
    hideIssueAiTyping();
    if (!m_issueThreadLayout)
        return;

    auto *row = new QWidget;
    row->setObjectName("issueTimelineRow");
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(14);
    auto *avatar = new QLabel(QStringLiteral("AI"));
    avatar->setObjectName("issueAvatar");
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setFixedSize(36, 36);
    rowLayout->addWidget(avatar, 0, Qt::AlignTop);

    auto *card = new QWidget;
    card->setObjectName("issueTimelineCard");
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(16, 12, 16, 12);
    auto *label = new QLabel(QString::fromUtf8("\xF0\x9F\xA4\x96  AI is answering"));
    label->setObjectName("statusLine");
    cardLayout->addWidget(label);
    rowLayout->addWidget(card, 1);

    const int composerIndex = m_issueComposerRow
                                  ? m_issueThreadLayout->indexOf(m_issueComposerRow)
                                  : -1;
    const int insertAt = composerIndex >= 0
                             ? composerIndex
                             : qMax(0, m_issueThreadLayout->count() - 1);
    m_issueThreadLayout->insertWidget(insertAt, row);
    m_issueAiTypingRow = row;

    m_issueAiTypingTimer = new QTimer(row);
    connect(m_issueAiTypingTimer, &QTimer::timeout, label, [label, n = 0]() mutable {
        n = (n + 1) % 4;
        label->setText(QString::fromUtf8("%1\xF0\x9F\xA4\x96  AI is answering%2")
                           .arg(QString(n, QChar(' ')), QString(n, QChar('.'))));
    });
    m_issueAiTypingTimer->start(400);

    if (m_issueThreadScroll) {
        QTimer::singleShot(0, this, [this] {
            if (m_issueThreadScroll && m_issueThreadScroll->verticalScrollBar())
                m_issueThreadScroll->verticalScrollBar()->setValue(
                    m_issueThreadScroll->verticalScrollBar()->maximum());
        });
    }
}

void MainWindow::hideIssueAiTyping()
{
    if (m_issueAiTypingTimer) {
        m_issueAiTypingTimer->stop();
        m_issueAiTypingTimer = nullptr; // parented to the row; freed with it
    }
    if (m_issueAiTypingRow) {
        if (m_issueThreadLayout)
            m_issueThreadLayout->removeWidget(m_issueAiTypingRow);
        m_issueAiTypingRow->deleteLater();
        m_issueAiTypingRow = nullptr;
    }
}

void MainWindow::askAiForCurrentIssue()
{
    if (m_currentIssueNumber < 0 || !m_networkAccess)
        return;
    const QString question =
        m_issueComposer ? m_issueComposer->markdown().trimmed() : QString();
    if (question.isEmpty()) {
        setIssueInlineNotice("Type a question in the comment box first.", true);
        return;
    }
    const QString apiKey =
        QSettings().value(kCodexApiKeySetting).toString().trimmed();
    if (apiKey.isEmpty()) {
        setIssueInlineNotice("Add an OpenAI API key in Settings first.", true);
        return;
    }

    const int issueNumber = m_currentIssueNumber;
    const Issue *selected = nullptr;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.number == issueNumber)
            selected = &issue;
    if (!selected)
        return;
    const int repoIdx = issuesRepoIndex();
    if (repoIdx < 0)
        return;
    const RepositoryRecord selectedRepo = m_repositories.at(repoIdx);
    const RepositoryRecord writableRepo = writableRecordFor(selectedRepo);

    const QString issueContext =
        issueThreadContext(*selected, kIssueAiContextChars);

    const QString prompt =
        QStringLiteral("Use this issue thread as context.\n\n%1\n\nQuestion:\n%2")
            .arg(issueContext, question);

    {
        IssueStore questionStore = issueStoreForCurrentRepo();
        if (questionStore.canWrite()) {
            QString qError;
            questionStore.addComment(issueNumber, question, {}, &qError);
        } else {
            submitIssueCommentToInbox(question);
        }
        if (m_issueComposer)
            m_issueComposer->setMarkdown(QString());
        reloadIssues();
        showIssueAiTyping();
    }

    QJsonObject payload;
    payload.insert(QStringLiteral("model"), kIssueAskAiModel);
    payload.insert(QStringLiteral("instructions"),
                   QStringLiteral("Answer the user's issue question concisely. "
                                  "If the repository context is insufficient, "
                                  "say what is missing. Do not claim to have "
                                  "changed code or inspected files beyond the "
                                  "provided issue thread."));
    payload.insert(QStringLiteral("input"), prompt);
    payload.insert(QStringLiteral("max_output_tokens"), 900);

    QNetworkRequest request(
        QUrl(QStringLiteral("https://api.openai.com/v1/responses")));
    request = openAiRequest(request.url(), apiKey);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    if (m_issueAskAiButton) {
        m_issueAskAiButton->setEnabled(false);
        m_issueAskAiButton->setText("Asking...");
    }
    setIssueInlineNotice("Asking OpenAI...");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, issueNumber, question, selectedRepo, writableRepo] {
                const QByteArray body = reply->readAll();
                reply->deleteLater();
                hideIssueAiTyping();
                if (m_issueAskAiButton) {
                    m_issueAskAiButton->setText("Ask AI");
                    m_issueAskAiButton->setEnabled(m_currentIssueNumber >= 0);
                }
                if (reply->error() != QNetworkReply::NoError) {
                    setIssueInlineNotice("OpenAI request failed: " +
                                             apiErrorSummary(reply, body),
                                         true);
                    return;
                }

                const QJsonObject obj = QJsonDocument::fromJson(body).object();
                const QString answer = openAiResponseText(obj);
                if (answer.isEmpty()) {
                    setIssueInlineNotice("OpenAI returned an empty answer.", true);
                    return;
                }

                qint64 inTok = 0, outTok = 0;
                const double costUsd = openAiAskCostUsd(obj, &inTok, &outTok);
                const QString costLine =
                    QString::fromUtf8("\n\n*\xF0\x9F\xA4\x96 %1 \xC2\xB7 cost "
                                      "$%2 (%3 in / %4 out tokens)*")
                        .arg(kIssueAskAiModel,
                             QString::number(costUsd, 'f', 4))
                        .arg(inTok)
                        .arg(outTok);
                const QString comment = answer.trimmed() + costLine;

                IssueStore store(writableRepo.localPath, writableRepo.mirrorPath,
                                 &m_profileIdentity, m_userName);
                if (store.canWrite()) {
                    QString error;
                    if (!store.addComment(issueNumber, comment, {}, &error)) {
                        setIssueInlineNotice(error.isEmpty()
                                                 ? "Could not add the AI answer."
                                                 : error,
                                             true);
                        return;
                    }
                    if (m_issueComposer)
                        m_issueComposer->setMarkdown(QString());
                    reloadIssues();
                    setIssueInlineNotice("AI answer added.");
                    return;
                }

                IssueStore signingStore(selectedRepo.localPath,
                                        selectedRepo.mirrorPath,
                                        &m_profileIdentity, m_userName);
                signingStore.setSigningRepository(selectedRepo.owner,
                                                  selectedRepo.name);
                IssueEvent ev;
                ev.type = "comment";
                ev.body = comment;
                ev = signingStore.makeSignedEvent(issueNumber, ev);
                QJsonObject eventJson = ev.toJson();
                eventJson.insert("body", ev.body);
                const QJsonObject payload{{"owner", selectedRepo.owner},
                                          {"repo", selectedRepo.name},
                                          {"number", issueNumber},
                                          {"event", eventJson}};

                QNetworkRequest request(issuesApiUrl(selectedRepo));
                request.setHeader(QNetworkRequest::ContentTypeHeader,
                                  "application/json");
                QNetworkReply *postReply = m_networkAccess->post(
                    request,
                    QJsonDocument(payload).toJson(QJsonDocument::Compact));
                connect(postReply, &QNetworkReply::finished, this,
                        [this, postReply] {
                            postReply->deleteLater();
                            if (postReply->error() == QNetworkReply::NoError) {
                                if (m_issueComposer)
                                    m_issueComposer->setMarkdown(QString());
                                setIssueInlineNotice(
                                    "AI answer sent to the maintainer's inbox.");
                            } else {
                                setIssueInlineNotice(
                                    "Could not send the AI answer: " +
                                        postReply->errorString(),
                                    true);
                            }
                        });
            });
}

namespace {

// Per-million-token pricing for the models the source-control AI picker knows,
// plus the ones only a mention can reach: the picker lists what is worth
// drafting a commit message with, while "@fable" or a superseded id spelled out
// in full ("@claude-opus-4-8") still deserves a dollar figure in its footer.
// A model in neither table reports its token counts without inventing one.
bool issueModelPricePerMillion(const QString &model, double *inPerMillion,
                               double *outPerMillion)
{
    for (int i = 0; i < kScmAiModelCount; ++i) {
        if (model != QLatin1String(kScmAiModels[i].id))
            continue;
        *inPerMillion = kScmAiModels[i].inPerM;
        *outPerMillion = kScmAiModels[i].outPerM;
        return true;
    }
    struct MentionPrice {
        const char *id;
        double inPerM;
        double outPerM;
    };
    static const MentionPrice kMentionOnly[] = {
        {"claude-fable-5", 10.0, 50.0},
        {"claude-opus-4-8", 5.0, 25.0},
        {"claude-opus-4-7", 5.0, 25.0},
        {"claude-opus-4-6", 5.0, 25.0},
        {"claude-sonnet-4-6", 3.0, 15.0},
    };
    for (const MentionPrice &price : kMentionOnly) {
        if (model != QLatin1String(price.id))
            continue;
        *inPerMillion = price.inPerM;
        *outPerMillion = price.outPerM;
        return true;
    }
    return false;
}

// The Codex line-up names models of a ChatGPT subscription the CLI drives, not
// models /v1/responses will answer as. Only the ids the desktop already asks
// over the OpenAI API can take a question here.
bool openAiApiAnswersAs(const QString &model)
{
    for (int i = 0; i < kScmAiModelCount; ++i)
        if (QLatin1String(kScmAiModels[i].provider) == QLatin1String("openai") &&
            model == QLatin1String(kScmAiModels[i].id))
            return true;
    return false;
}

// The model an alias resolves to right now. The app's own "auto" ladder wins
// over the parser's offline table, so bumping a rung in claudeAutoLadder()
// moves "@opus" with it.
QString liveMentionModel(const forkmesh::issuemention::Mention &mention)
{
    for (const ClaudeAutoRung &rung : claudeAutoLadder())
        if (mention.alias == rung.alias)
            return rung.id;
    return mention.model;
}

} // namespace

QString MainWindow::issueThreadContext(const Issue &issue, int maxChars) const
{
    QHash<QString, QString> edits;
    QSet<QString> deleted;
    for (const IssueEvent &ev : issue.events) {
        if (ev.type == "edit" && !ev.target.isEmpty())
            edits.insert(ev.target, ev.body);
        else if (ev.type == "delete" && !ev.target.isEmpty() && ev.target != "self")
            deleted.insert(ev.target);
    }

    QStringList context;
    context << QStringLiteral("Issue #%1: %2").arg(issue.number).arg(issue.title)
            << QStringLiteral("Status: %1").arg(issue.status);
    if (!issue.labels.isEmpty())
        context << QStringLiteral("Labels: %1").arg(issue.labels.join(", "));
    if (!issue.milestone.isEmpty())
        context << QStringLiteral("Milestone: %1").arg(issue.milestone);
    for (const IssueEvent &ev : issue.events) {
        if (ev.type != "open" && ev.type != "comment")
            continue;
        if (deleted.contains(ev.id))
            continue;
        const QString who = ev.authorName.isEmpty() ? ev.author.left(10) : ev.authorName;
        const QString body = edits.contains(ev.id) ? edits.value(ev.id) : ev.body;
        context << QStringLiteral("\n%1:\n%2").arg(who, body);
    }

    QString text = context.join('\n');
    if (maxChars > 0 && text.size() > maxChars)
        text = text.left(maxChars) + QStringLiteral("\n\n[Issue context truncated]");
    return text;
}

// What the repository knows about an issue, for a model answering a mention:
// the commits that have landed, the ones naming this issue, the pull requests
// that reference it, and the files the thread points at. Without this a bot
// asked "was this fixed?" can only read the conversation back — the answer
// lives in the history, not the thread.
QString MainWindow::issueRepoContext(const RepositoryRecord &repo,
                                     const Issue &issue,
                                     const QList<PullRequest> &pulls,
                                     const QString &threadText,
                                     int maxChars) const
{
    const RepositoryRecord writable = writableRecordFor(repo);
    QString gitDir = writable.localPath.trimmed();
    if (gitDir.isEmpty() || !QDir(gitDir).exists())
        gitDir = writable.mirrorPath.trimmed();
    const bool haveGit = !gitDir.isEmpty() && QDir(gitDir).exists();

    const QString hash = QStringLiteral("#%1").arg(issue.number);
    const QString branchTag = QStringLiteral("issue-%1").arg(issue.number);
    auto namesThisIssue = [&hash, &branchTag](const QString &text) {
        if (text.contains(branchTag, Qt::CaseInsensitive))
            return true;
        // "#63" must not match inside "#631", and the first hit being a longer
        // number does not mean a later one is too.
        for (qsizetype at = text.indexOf(hash); at >= 0;
             at = text.indexOf(hash, at + 1))
            if (at + hash.size() >= text.size() ||
                !text.at(at + hash.size()).isDigit())
                return true;
        return false;
    };

    QStringList out;
    out << QStringLiteral("Repository: %1/%2").arg(repo.owner, repo.name);

    if (haveGit) {
        const QStringList format = {QStringLiteral("--date=short"),
                                    QStringLiteral("--pretty=%h %ad %an: %s")};
        QByteArray head;
        if (runGitCapture(gitDir,
                          QStringList{QStringLiteral("log"),
                                      QStringLiteral("-n"),
                                      QStringLiteral("30")} +
                              format,
                          &head, nullptr) &&
            !head.trimmed().isEmpty())
            out << QStringLiteral("\nRecent commits on the default branch "
                                  "(newest first):\n%1")
                       .arg(QString::fromUtf8(head).trimmed());

        // -n bounds the matches, not the walk: with no hit at all --grep reads
        // every commit on every ref. A fix cannot predate the report, so the
        // day before it was filed is both the honest floor and the cheap one.
        const QString since =
            QDateTime::fromMSecsSinceEpoch(
                issue.createdAt > 0
                    ? issueDisplayTimestampMs(issue.createdAt) - 24 * 60 * 60 * 1000
                    : 0)
                .toUTC()
                .toString(Qt::ISODate);
        QByteArray linked;
        if (runGitCapture(gitDir,
                          QStringList{QStringLiteral("log"),
                                      QStringLiteral("--all"),
                                      QStringLiteral("-n"),
                                      QStringLiteral("20"),
                                      QStringLiteral("--since=") + since,
                                      QStringLiteral("--regexp-ignore-case"),
                                      QStringLiteral("--grep=") + hash,
                                      QStringLiteral("--grep=") + branchTag} +
                              format,
                          &linked, nullptr) &&
            !linked.trimmed().isEmpty())
            out << QStringLiteral("\nCommits naming this issue (any branch):\n%1")
                       .arg(QString::fromUtf8(linked).trimmed());

        QByteArray branches;
        if (runGitCapture(gitDir,
                          {QStringLiteral("branch"), QStringLiteral("--all"),
                           QStringLiteral("--list"),
                           QLatin1Char('*') + branchTag + QLatin1Char('*')},
                          &branches, nullptr) &&
            !branches.trimmed().isEmpty())
            out << QStringLiteral("\nBranches for this issue:\n%1")
                       .arg(QString::fromUtf8(branches).trimmed());
    }

    // Newest first, and say so honestly: loadAll() returns whatever order the
    // ledger sits in on disk, which is not the order a reader assumes.
    QList<const PullRequest *> ordered;
    ordered.reserve(pulls.size());
    for (const PullRequest &pull : pulls)
        if (pull.number > 0)
            ordered.append(&pull);
    std::sort(ordered.begin(), ordered.end(),
              [](const PullRequest *a, const PullRequest *b) {
                  return a->number > b->number;
              });

    QStringList linkedPulls;
    QStringList otherPulls;
    for (const PullRequest *entry : std::as_const(ordered)) {
        const PullRequest &pull = *entry;
        // Built by concatenation, not arg(): a pull title is user text and a
        // stray "%3" in one would eat the next placeholder.
        const QString line =
            QStringLiteral("PR #%1 [%2] ").arg(pull.number).arg(pull.status) +
            pull.title.simplified() + QStringLiteral(" (") + pull.head +
            QStringLiteral(" -> ") + pull.base + QStringLiteral(", by ") +
            (pull.authorName.isEmpty() ? pull.author.left(10)
                                       : pull.authorName) +
            QStringLiteral(", %1 files +%2/-%3)")
                .arg(pull.filesChanged)
                .arg(pull.additions)
                .arg(pull.deletions);
        QString mentionText = pull.title + QLatin1Char('\n') + pull.description +
                              QLatin1Char('\n') + pull.head;
        for (const PullEvent &ev : pull.events)
            mentionText += QLatin1Char('\n') + ev.body;
        if (!namesThisIssue(mentionText)) {
            otherPulls << line;
            continue;
        }
        QString body = pull.description.simplified();
        if (body.size() > 600)
            body = body.left(597) + QString::fromUtf8("\xE2\x80\xA6");
        linkedPulls << (body.isEmpty() ? line
                                       : line + QStringLiteral("\n  ") + body);
    }
    if (!linkedPulls.isEmpty())
        out << QStringLiteral("\nPull requests referencing this issue:\n%1")
                   .arg(linkedPulls.join(QLatin1Char('\n')));
    if (!otherPulls.isEmpty()) {
        const int shown = qMin(otherPulls.size(), 12);
        out << QStringLiteral("\nOther pull requests (%1 of %2, newest "
                              "first):\n")
                       .arg(shown)
                       .arg(otherPulls.size()) +
                   otherPulls.mid(0, shown).join(QLatin1Char('\n'));
    }
    if (linkedPulls.isEmpty() && otherPulls.isEmpty())
        out << QStringLiteral("\nThis repository has no pull requests on "
                              "record.");

    const QList<forkmesh::issuemention::FileCitation> cited =
        forkmesh::issuemention::citedFilePaths(threadText);
    if (haveGit && !cited.isEmpty()) {
        QByteArray listing;
        QStringList tracked;
        if (runGitCapture(gitDir, {QStringLiteral("ls-files")}, &listing,
                          nullptr))
            tracked = QString::fromUtf8(listing).split(QLatin1Char('\n'),
                                                       Qt::SkipEmptyParts);
        int shown = 0;
        for (const forkmesh::issuemention::FileCitation &entry : cited) {
            if (shown >= 3)
                break;
            const QString path =
                forkmesh::issuemention::trackedPathFor(tracked, entry.path);
            if (path.isEmpty())
                continue;
            QByteArray body;
            if (!runGitCapture(gitDir,
                               {QStringLiteral("show"),
                                QStringLiteral("HEAD:") + path},
                               &body, nullptr) ||
                body.contains('\0'))
                continue;
            ++shown;
            out << QLatin1Char('\n') + path + QStringLiteral(" at HEAD") +
                       (entry.line > 0 ? QStringLiteral(", around line %1")
                                             .arg(entry.line)
                                       : QString()) +
                       QStringLiteral(":\n") +
                       forkmesh::issuemention::numberedFileWindow(
                           QString::fromUtf8(body), entry.line, 60, 4000);
        }
    }

    QString text = out.join(QLatin1Char('\n'));
    if (maxChars > 0 && text.size() > maxChars)
        text = text.left(maxChars) +
               QStringLiteral("\n\n[Repository context truncated]");
    return text;
}

// code-health: allow(deep-nested-loop) issues x events is one pass over events; the mention loop runs only for events past the repo's floor stamp whose text contains '@', a few mentions each
void MainWindow::answerIssueModelMentions(const RepositoryRecord &repo,
                                          const QList<Issue> &issues,
                                          const QList<PullRequest> &pulls)
{
    using namespace forkmesh::issuemention;
    if (!m_networkAccess || repo.owner.isEmpty() || repo.name.isEmpty())
        return;
    if (!QSettings().value(kIssueModelMentionsSetting, true).toBool())
        return;
    // Only the node holding the writable issue log answers. Every mirror sees
    // the same mention, and one mention deserves exactly one reply.
    const RepositoryRecord writable = writableRecordFor(repo);
    IssueStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                     m_userName);
    if (!store.canWrite())
        return;

    const QString repoKey = repo.owner + QLatin1Char('/') + repo.name;
    QSettings settings;
    QStringList handledList =
        settings.value(kIssueModelMentionsHandledSetting).toStringList();
    QSet<QString> handled(handledList.cbegin(), handledList.cend());
    QStringList floorList =
        settings.value(kIssueModelMentionsFloorSetting).toStringList();
    QHash<QString, qint64> floors;
    for (const QString &entry : std::as_const(floorList)) {
        const qsizetype space = entry.lastIndexOf(QLatin1Char(' '));
        if (space > 0)
            floors.insert(entry.left(space), entry.mid(space + 1).toLongLong());
    }
    // Everything older than this repository's stamp has been considered on an
    // earlier scan. On a first scan there is no stamp, so only the last few
    // minutes count: adding a repository with years of history must not fire a
    // burst of replies at conversations that ended long ago, while a mention
    // someone just typed is still answered.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 consideredBefore =
        floors.value(repoKey, now - kIssueModelMentionsSeedGraceMs);

    struct Pending {
        Issue issue;
        Mention mention;
        QString key;
        QString eventId; // what the bot reacts to, to say it saw the mention
        qint64 ts = 0;
    };
    QList<Pending> pending;
    for (const Issue &issue : issues) {
        if (issue.number <= 0 || issue.isDeleted())
            continue;
        QSet<QString> deleted;
        for (const IssueEvent &ev : issue.events)
            if (ev.type == QLatin1String("delete") && !ev.target.isEmpty() &&
                ev.target != QLatin1String("self"))
                deleted.insert(ev.target);
        for (const IssueEvent &ev : issue.events) {
            if (ev.type != QLatin1String("open") &&
                ev.type != QLatin1String("comment") &&
                ev.type != QLatin1String("edit"))
                continue;
            if (ev.ts < consideredBefore)
                continue;
            if (!ev.body.contains(QLatin1Char('@')) &&
                !ev.title.contains(QLatin1Char('@')))
                continue; // cheap skip: this pass runs over every thread
            if (deleted.contains(ev.id) || isBotAuthorName(ev.authorName))
                continue; // a model's own reply never re-triggers the models
            const QList<Mention> mentions =
                parse((ev.title + QLatin1Char('\n') + ev.body).trimmed());
            for (int i = 0; i < mentions.size(); ++i) {
                const QString key = QStringLiteral("%1#%2:%3:%4:%5")
                                        .arg(repoKey)
                                        .arg(issue.number)
                                        .arg(ev.id, mentions.at(i).alias)
                                        .arg(i);
                if (handled.contains(key))
                    continue;
                // A mention typed into an edit belongs to the comment that was
                // edited: the edit itself is an activity line, and a chip left
                // there is a chip nobody sees.
                const QString reactTo =
                    ev.type == QLatin1String("edit") && !ev.target.isEmpty()
                        ? ev.target
                        : ev.id;
                pending.append(
                    Pending{issue, mentions.at(i), key, reactTo, ev.ts});
            }
        }
    }

    QList<Pending> run;
    for (const Pending &item : std::as_const(pending)) {
        // Over budget: leave the mention unhandled so the next scan picks it up
        // rather than dropping it on the floor.
        if (run.size() >= kIssueModelMentionsPerScan)
            break;
        handled.insert(item.key);
        handledList.append(item.key);
        run.append(item);
    }
    // The stamp may not pass a mention still owed a reply, or the next scan
    // would skip it before it ever ran.
    qint64 nextFloor = now - kIssueModelMentionsSeedGraceMs;
    for (const Pending &item : std::as_const(pending))
        if (!handled.contains(item.key))
            nextFloor = qMin(nextFloor, item.ts);
    // Only when it actually moves on: this scan runs after every sync, and
    // every setValue() rewrites the whole ini file. Between advances the stamp
    // lags by at most another window, which only means a few extra minutes of
    // already-answered mentions are re-parsed and skipped by their key.
    if (nextFloor >
        floors.value(repoKey, 0) + kIssueModelMentionsSeedGraceMs) {
        floors.insert(repoKey, nextFloor);
        floorList.clear();
        for (auto it = floors.cbegin(); it != floors.cend(); ++it)
            floorList.append(it.key() + QLatin1Char(' ') +
                             QString::number(it.value()));
        floorList.sort(); // stable on disk, so a no-op scan doesn't rewrite it
        settings.setValue(kIssueModelMentionsFloorSetting, floorList);
    }
    if (!run.isEmpty()) {
        // Oldest first out: every setValue() rewrites the whole ini file.
        if (handledList.size() > kIssueModelMentionsHandledMax)
            handledList = handledList.mid(handledList.size() -
                                          kIssueModelMentionsHandledMax);
        settings.setValue(kIssueModelMentionsHandledSetting, handledList);
    }
    for (const Pending &item : std::as_const(run))
        runIssueModelMention(repo, item.issue, item.mention, item.eventId, pulls);
}

void MainWindow::runIssueModelMention(
    const RepositoryRecord &repo, const Issue &issue,
    const forkmesh::issuemention::Mention &mention,
    const QString &mentionEventId, const QList<PullRequest> &pulls)
{
    using namespace forkmesh::issuemention;
    const QString model = liveMentionModel(mention);
    const QString label = agentModelLabel(model);
    const QString botAuthor = botAuthorName(label);
    const QString provider = mention.provider.isEmpty()
                                 ? providerForModel(model)
                                 : mention.provider;
    const bool claude = agentIsClaudeProvider(provider);
    const int number = issue.number;

    // Acknowledge first. Asking a model takes a round trip, starting an agent
    // takes longer still, and until one of them lands the thread shows nothing
    // at all -- which reads exactly like a mention nobody picked up.
    postIssueModelReaction(repo, number, mentionEventId, botAuthor,
                           QStringLiteral("eyes"));

    if (mention.intent == Intent::Work) {
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            if (session.issueNumber != number || session.owner != repo.owner ||
                session.name != repo.name)
                continue;
            if (session.status != AgentStatus::Queued &&
                session.status != AgentStatus::Running &&
                session.status != AgentStatus::Waiting)
                continue;
            // A second agent on the same issue would branch from the same base
            // and race the first one's worktree.
            postIssueModelComment(
                repo, number, botAuthor,
                QStringLiteral(
                    "An agent is already working on this issue (session %1), "
                    "so I left it running instead of starting a second one.")
                    .arg(session.id));
            return;
        }
        const int sessionId = startAgentForIssue(issue, provider, /*createPr=*/false,
                                                 /*quiet=*/true, model, &repo);
        postIssueModelComment(
            repo, number, botAuthor,
            sessionId > 0
                ? QStringLiteral(
                      "Started a coding agent on this issue with %1.\n\n> %2\n\n"
                      "It works on `agent/issue-%3-s%4-...`; no pull request "
                      "will be opened unless one is explicitly requested.")
                      .arg(label, mention.request.simplified())
                      .arg(number)
                      .arg(sessionId)
                : QStringLiteral(
                      "I could not start an agent on this issue: this node has "
                      "no local checkout of %1/%2 to run one in.")
                      .arg(repo.owner, repo.name));
        return;
    }

    QString oauthToken;
    QString apiKey;
    if (claude) {
        // Claude Code authenticates with the claude.ai subscription token the
        // CLI keeps; only fall back to the metered API key when there is none.
        if (provider == QLatin1String("claude-code"))
            oauthToken = claudeCodeOAuthToken();
        if (oauthToken.isEmpty())
            apiKey = QSettings().value(kClaudeApiKeySetting).toString().trimmed();
    } else {
        apiKey = QSettings().value(kCodexApiKeySetting).toString().trimmed();
    }
    if (oauthToken.isEmpty() && apiKey.isEmpty()) {
        postIssueModelComment(
            repo, number, botAuthor,
            QStringLiteral("This node has no credentials for %1, so the "
                           "mention went unanswered. %2")
                .arg(label,
                     claude ? QStringLiteral("Sign in to Claude Code, or add a "
                                             "Claude API key in Settings.")
                            : QStringLiteral("Add an OpenAI API key in "
                                             "Settings.")));
        return;
    }

    QString note;
    QString askModel = model;
    if (!claude && !openAiApiAnswersAs(model)) {
        askModel = kIssueAskAiModel;
        note = QStringLiteral("asked as %1").arg(askModel);
    }

    const QString question =
        mention.request.trimmed().isEmpty()
            ? QStringLiteral("Read this issue and reply with what you make of it.")
            : mention.request.trimmed();
    const QString instructions =
        QStringLiteral(
            "You are %1, replying in a ForkMesh issue thread. Below the thread "
            "you are given what the repository itself says: its recent commit "
            "history, the commits and branches naming this issue, its pull "
            "requests, and the current contents of any file the thread points "
            "at. Use it -- when someone asks whether something was fixed, "
            "answer from the commits and pull requests rather than asking them "
            "to paste more. Be concrete and brief: a few short paragraphs or a "
            "short list, in Markdown, citing commit hashes and PR numbers when "
            "you rely on them. That snapshot is all you can see, so say "
            "plainly what is missing from it instead of guessing, and never "
            "claim to have run code or changed anything.")
            .arg(label);
    const QString thread = issueThreadContext(issue, kIssueAiContextChars);
    const QString prompt =
        QStringLiteral("----- ISSUE THREAD -----\n%1\n\n"
                       "----- REPOSITORY -----\n%2\n\n"
                       "----- ADDRESSED TO YOU -----\n%3")
            .arg(thread,
                 issueRepoContext(repo, issue, pulls,
                                  thread + QLatin1Char('\n') + question,
                                  kIssueRepoContextChars),
                 question);

    IssueMentionAsk ask;
    ask.repo = repo;
    ask.number = number;
    ask.botAuthor = botAuthor;
    ask.label = label;
    ask.model = askModel;
    ask.claude = claude;
    ask.oauthToken = oauthToken;
    ask.apiKey = apiKey;
    ask.instructions = instructions;
    ask.prompt = prompt;
    ask.note = note;

    logSystem(QStringLiteral("Answering an @%1 mention on %2/%3 issue #%4 with %5.")
                  .arg(mention.alias, repo.owner, repo.name)
                  .arg(number)
                  .arg(askModel));
    sendIssueMentionAsk(ask, kIssueModelMentionMaxTokens, /*retry=*/false);
}

void MainWindow::sendIssueMentionAsk(const IssueMentionAsk &ask, int maxTokens,
                                     bool retry)
{
    if (!m_networkAccess)
        return;
    QNetworkReply *reply = nullptr;
    if (ask.claude) {
        QJsonObject payload;
        payload.insert("model", ask.model);
        payload.insert("max_tokens", maxTokens);
        QJsonArray messages;
        QJsonObject userMessage;
        userMessage.insert("role", "user");
        // A subscription OAuth token is only accepted alongside Claude Code's
        // own system identity, so with one the instructions ride in the user
        // turn instead of the system slot.
        userMessage.insert("content", ask.oauthToken.isEmpty()
                                          ? ask.prompt
                                          : ask.instructions +
                                                QStringLiteral("\n\n") + ask.prompt);
        messages.append(userMessage);
        payload.insert("messages", messages);
        payload.insert("system", ask.oauthToken.isEmpty() ? ask.instructions
                                                          : kClaudeCodeOAuthSystem);
        QNetworkRequest request(
            QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
        if (!ask.oauthToken.isEmpty()) {
            request.setRawHeader("Authorization",
                                 "Bearer " + ask.oauthToken.toUtf8());
            request.setRawHeader("anthropic-beta", "oauth-2025-04-20");
        } else {
            request.setRawHeader("x-api-key", ask.apiKey.toUtf8());
        }
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    } else {
        QJsonObject payload;
        payload.insert("model", ask.model);
        payload.insert("instructions", ask.instructions);
        payload.insert("input", ask.prompt);
        payload.insert("max_output_tokens", maxTokens);
        QNetworkRequest request = openAiRequest(
            QUrl(QStringLiteral("https://api.openai.com/v1/responses")), ask.apiKey);
        request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    connect(reply, &QNetworkReply::finished, this,
            [this, reply, ask, maxTokens, retry] {
                const QByteArray body = reply->readAll();
                reply->deleteLater();
                if (reply->error() != QNetworkReply::NoError) {
                    postIssueModelComment(
                        ask.repo, ask.number, ask.botAuthor,
                        QStringLiteral("I could not reach %1: %2")
                            .arg(ask.label, apiErrorSummary(reply, body)));
                    return;
                }
                const QJsonObject obj = QJsonDocument::fromJson(body).object();
                QString answer;
                QString stopReason;
                qint64 inTokens = 0, outTokens = 0;
                if (ask.claude) {
                    for (const QJsonValue &value : obj.value("content").toArray()) {
                        const QJsonObject part = value.toObject();
                        if (part.value("type").toString() == QLatin1String("text"))
                            answer += part.value("text").toString();
                    }
                    const QJsonObject usage = obj.value("usage").toObject();
                    inTokens = usage.value("input_tokens").toInt();
                    outTokens = usage.value("output_tokens").toInt();
                    stopReason = obj.value("stop_reason").toString();
                } else {
                    answer = openAiResponseText(obj);
                    openAiAskCostUsd(obj, &inTokens, &outTokens);
                    if (obj.value("status").toString() == QLatin1String("incomplete"))
                        stopReason = obj.value("incomplete_details")
                                         .toObject()
                                         .value("reason")
                                         .toString();
                }
                answer = answer.trimmed();
                // Both APIs say the same thing in their own words: the answer
                // budget ran out. On a model that reasons first that happens
                // inside the thinking block, before a word of prose exists to
                // send back -- which is what an empty answer usually is.
                const bool outOfBudget =
                    stopReason == QLatin1String("max_tokens") ||
                    stopReason == QLatin1String("max_output_tokens");
                if (answer.isEmpty()) {
                    if (!retry && outOfBudget) {
                        sendIssueMentionAsk(ask, kIssueModelMentionRetryMaxTokens,
                                            /*retry=*/true);
                        return;
                    }
                    postIssueModelComment(
                        ask.repo, ask.number, ask.botAuthor,
                        outOfBudget
                            ? QStringLiteral(
                                  "%1 spent its whole %2-token answer budget "
                                  "without writing a reply. Ask something "
                                  "narrower, or mention a model that reasons "
                                  "less before it answers.")
                                  .arg(ask.label)
                                  .arg(maxTokens)
                            : QStringLiteral(
                                  "%1 answered with no text at all%2. Mention "
                                  "me again to retry.")
                                  .arg(ask.label,
                                       stopReason.isEmpty()
                                           ? QString()
                                           : QStringLiteral(" (stop reason: %1)")
                                                 .arg(stopReason)));
                    return;
                }
                const QString separator = QString::fromUtf8(" \xC2\xB7 ");
                QString footer = QString::fromUtf8("\n\n*\xF0\x9F\xA4\x96 ") +
                                 ask.model + separator +
                                 QStringLiteral("%1 in / %2 out tokens")
                                     .arg(inTokens)
                                     .arg(outTokens);
                double inPerMillion = 0.0, outPerMillion = 0.0;
                if (issueModelPricePerMillion(ask.model, &inPerMillion,
                                              &outPerMillion))
                    footer += separator +
                              QStringLiteral("cost $%1").arg(
                                  (inTokens * inPerMillion +
                                   outTokens * outPerMillion) /
                                      1000000.0,
                                  0, 'f', 4);
                if (!ask.note.isEmpty())
                    footer += separator + ask.note;
                if (retry)
                    footer += separator +
                              QStringLiteral("asked again with a larger answer budget");
                footer += QStringLiteral("*");
                postIssueModelComment(ask.repo, ask.number, ask.botAuthor,
                                      answer + footer);
            });
}

void MainWindow::postIssueModelComment(const RepositoryRecord &repo, int number,
                                       const QString &botAuthor,
                                       const QString &body)
{
    if (number <= 0 || body.trimmed().isEmpty())
        return;
    // authorName sits outside an event's signed canonical string, so the
    // comment stays signed by this node while reading as the model's, and the
    // [bot] suffix is what keeps a reply that quotes another @alias from
    // re-triggering the scan.
    const RepositoryRecord writable = writableRecordFor(repo);
    IssueStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                     botAuthor);
    QString error;
    if (!store.addComment(number, body, {}, &error)) {
        logSystem(QStringLiteral("Could not post the %1 reply on %2/%3 issue "
                                 "#%4: %5")
                      .arg(botAuthor, repo.owner, repo.name)
                      .arg(number)
                      .arg(error.isEmpty() ? QStringLiteral("write failed")
                                           : error));
        return;
    }
    const int index = repoIndexFor(repo.owner, repo.name);
    if (index >= 0)
        propagateRepoUpdate(index);
    if (index >= 0 && issuesRepoIndex() == index)
        reloadIssues();
}

void MainWindow::postIssueModelReaction(const RepositoryRecord &repo, int number,
                                        const QString &eventId,
                                        const QString &botAuthor,
                                        const QString &reaction)
{
    if (number <= 0 || eventId.isEmpty() || reaction.isEmpty())
        return;
    // Same trick as postIssueModelComment: this node signs, the display name
    // makes it the model's, and the [bot] suffix keeps the scan from reading
    // its own acknowledgement as a new mention.
    const RepositoryRecord writable = writableRecordFor(repo);
    IssueStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                     botAuthor);
    QString error;
    if (!store.setReaction(number, eventId, reaction, true, &error)) {
        logSystem(QStringLiteral("Could not acknowledge the %1 mention on "
                                 "%2/%3 issue #%4: %5")
                      .arg(botAuthor, repo.owner, repo.name)
                      .arg(number)
                      .arg(error.isEmpty() ? QStringLiteral("write failed")
                                           : error));
        return;
    }
    const int index = repoIndexFor(repo.owner, repo.name);
    if (index >= 0)
        propagateRepoUpdate(index);
    if (index >= 0 && issuesRepoIndex() == index)
        reloadIssues();
}

void MainWindow::toggleIssueReaction(int number, const QString &eventId,
                                     const QString &reaction)
{
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.toggleReaction(number, eventId, reaction, &error)) {
        setIssueInlineNotice(error.isEmpty()
                                 ? QStringLiteral("Could not save the reaction.")
                                 : error,
                             true);
        return;
    }
    propagateRepoUpdate(issuesRepoIndex());
    m_selectIssueOnReload = number;
    reloadIssues();
}

void MainWindow::showIssueReactionPicker(int number, const QString &eventId,
                                         QWidget *anchor)
{
    auto *popup = new QFrame(anchor ? anchor : this, Qt::Popup);
    popup->setObjectName(QStringLiteral("reactionPicker"));
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *grid = new QGridLayout(popup);
    grid->setContentsMargins(8, 8, 8, 8);
    grid->setSpacing(2);
    constexpr int kColumns = 6;
    int index = 0;
    for (const reactions::CatalogEntry &choice : reactions::catalog()) {
        auto *button = new QToolButton(popup);
        button->setObjectName(QStringLiteral("reactionPickerButton"));
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(choice.label);
        button->setIcon(QIcon(
            reactions::emojiPixmap(choice.value, 22, devicePixelRatio())));
        button->setIconSize(QSize(22, 22));
        button->setFixedSize(34, 34);
        const QString value = choice.value;
        connect(button, &QToolButton::clicked, this,
                [this, popup, number, eventId, value] {
                    popup->close();
                    toggleIssueReaction(number, eventId, value);
                });
        grid->addWidget(button, index / kColumns, index % kColumns);
        ++index;
    }
    popup->adjustSize();
    const QPoint cursor = QCursor::pos();
    QPoint pos = cursor - QPoint(10, popup->height() + 6);
    if (QScreen *screen = QGuiApplication::screenAt(cursor)) {
        const QRect avail = screen->availableGeometry();
        pos.setX(qBound(avail.left(), pos.x(), avail.right() - popup->width()));
        if (pos.y() < avail.top())
            pos.setY(cursor.y() + 6);
    }
    popup->move(pos);
    popup->show();
}

void MainWindow::scanCurrentRepoModelMentions()
{
    const int index = issuesRepoIndex();
    if (index < 0 || index >= m_repositories.size())
        return;
    scanRepoMentionsFor(writableRecordFor(m_repositories.at(index)));
}

void MainWindow::addIssueComment()
{
    if (m_currentIssueNumber < 0)
        return;
    const QString body = m_issueComposer ? m_issueComposer->markdown() : QString();
    const QStringList attachments =
        m_issueComposer ? m_issueComposer->pendingAttachments() : m_pendingIssueAttachments;
    const QStringList placeholders =
        m_issueComposer ? m_issueComposer->pendingAttachmentPlaceholders() : QStringList();
    if (body.trimmed().isEmpty() && attachments.isEmpty())
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        // Not the host: send a signed comment to the maintainer's relay inbox.
        submitIssueCommentToInbox(body, attachments, placeholders);
        return;
    }

    const int number = m_currentIssueNumber;

    if (attachments.isEmpty()) {
        for (Issue &issue : m_currentIssues) {
            if (issue.number != number)
                continue;
            IssueEvent ev;
            ev.type = "comment";
            ev.body = body;
            ev = store.makeSignedEvent(number, ev);
            issue.events.append(ev);
            renderIssueThread(issue);
            break;
        }
    }
    if (m_issueComposer) {
        m_issueComposer->setMarkdown(QString());
        m_issueComposer->clearPendingAttachments();
    }
    m_pendingIssueAttachments.clear();
    if (m_issueAttachButton)
        m_issueAttachButton->setText("Paste, drop, or click to add files");

    QTimer::singleShot(0, this, [this, number, body, attachments, placeholders]() {
        IssueStore store = issueStoreForCurrentRepo();
        QString error;
        if (!store.addComment(number, body, attachments, placeholders, &error)) {
            setIssueInlineNotice(error.isEmpty() ? "Could not add the comment." : error,
                                 true);
            reloadIssues(); // discard the optimistic card on failure
            return;
        }
        reloadIssues();
        propagateRepoUpdate(issuesRepoIndex());
        setIssueInlineNotice("Comment added.");
        // "@opus why does this hang?" typed here reaches the model the same
        // way one arriving from the relay inbox does.
        scanCurrentRepoModelMentions();
    });
}

void MainWindow::closeIssueWithComment()
{
    if (m_currentIssueNumber < 0)
        return;
    const int number = m_currentIssueNumber;
    const QString body = m_issueComposer ? m_issueComposer->markdown() : QString();
    const QStringList attachments =
        m_issueComposer ? m_issueComposer->pendingAttachments()
                        : m_pendingIssueAttachments;
    const QStringList placeholders =
        m_issueComposer ? m_issueComposer->pendingAttachmentPlaceholders() : QStringList();
    if (body.trimmed().isEmpty() && attachments.isEmpty()) {
        setIssueInlineNotice(
            "Write a comment to close with, or use \xE2\x80\x9C" "Close issue\xE2\x80\x9D.",
            true);
        if (m_issueComposer)
            m_issueComposer->setFocus();
        return;
    }
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        setIssueInlineNotice("This repo is read-only here; can't close the issue.",
                             true);
        return;
    }
    QString error;
    if (!store.addComment(number, body, attachments, placeholders, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not add the comment." : error,
                             true);
        return;
    }
    if (!store.setStatus(number, QStringLiteral("closed"), &error)) {
        setIssueInlineNotice(error.isEmpty()
                                 ? "Comment added, but could not close the issue."
                                 : error,
                             true);
        reloadIssues();
        return;
    }
    if (m_issueComposer) {
        m_issueComposer->setMarkdown(QString());
        m_issueComposer->clearPendingAttachments();
    }
    m_pendingIssueAttachments.clear();
    if (m_issueAttachButton)
        m_issueAttachButton->setText("Paste, drop, or click to add files");
    const int nextIssue = nextVisibleIssueAfter(number);
    if (nextIssue > 0)
        m_selectIssueOnReload = nextIssue;
    else
        m_keepCurrentOnReload = true;
    reloadIssues();
    propagateRepoUpdate(issuesRepoIndex());
    setIssueInlineNotice("Comment added and issue closed.");
}

void MainWindow::attachIssueImage()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Attach images", QString(),
        "Images (*.png *.jpg *.jpeg *.gif *.webp);;All files (*)");
    if (files.isEmpty())
        return;
    for (const QString &f : files)
        queueIssueAttachment(f);
}

void MainWindow::queueIssueAttachment(const QString &path)
{
    if (path.isEmpty() || m_pendingIssueAttachments.contains(path))
        return;
    m_pendingIssueAttachments += path;
    if (m_issueComposer)
        m_issueComposer->addImageFile(path);
    if (m_issueAttachButton)
        m_issueAttachButton->setText(
            QStringLiteral("Attached: %1").arg(m_pendingIssueAttachments.size()));
}

namespace {
QString promptDraftText(const QWidget *w)
{
    if (const auto *plain = qobject_cast<const QPlainTextEdit *>(w))
        return plain->toPlainText().trimmed();
    if (const auto *line = qobject_cast<const QLineEdit *>(w))
        return line->text().trimmed();
    if (const auto *rich = qobject_cast<const QTextEdit *>(w))
        return rich->toPlainText().trimmed();
    return QString();
}
} // namespace

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (handleFramelessResizeEvent(obj, event))
        return true;

    if (handlePageButtonEvent(obj, event))
        return true;

    if (obj == m_chatBody &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Show))
        syncChatMembersPanel();

    if (((m_pullDiff && obj == m_pullDiff->viewport()) ||
         (m_branchDiffView && obj == m_branchDiffView->viewport())) &&
        handlePullDiffLinePointerEvent(obj, event))
        return true;

    if (obj == m_gitMergedPage && event->type() == QEvent::Hide &&
        !event->spontaneous()) {
        QTimer::singleShot(0, this, [this] {
            if (m_gitMergedPage && !m_gitMergedPage->isVisible())
                clearGitMergedCover();
        });
    }

    if (event->type() == QEvent::Resize) {
        if (m_branchDiffView && obj == m_branchDiffView->viewport()) {
            updateBranchDiffSticky();
            updateBranchDiffActiveOutline();
            positionPullInlineComposer();
        } else if (m_scmDiff && obj == m_scmDiff->viewport()) {
            updateScmDiffScrollState();
        }
    }

    if (obj == m_scmTree && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const bool plainArrow =
            (key->key() == Qt::Key_Up || key->key() == Qt::Key_Down) &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier |
                                  Qt::MetaModifier | Qt::ShiftModifier));
        if (plainArrow) {
            stepScmTreeFile(key->key() == Qt::Key_Down ? 1 : -1);
            return true; // consumed either way: never land on a group header
        }
    }

    if (obj == m_agentStatusPill || obj == m_agentMetaPopup) {
        if (event->type() == QEvent::Enter && obj == m_agentStatusPill) {
            showAgentMetaPopup();
        } else if (event->type() == QEvent::Leave) {
            QTimer::singleShot(100, this,
                               &MainWindow::hideAgentMetaPopupIfPointerAway);
        }
    }

    if (event->type() == QEvent::Show) {
        if (auto *w = qobject_cast<QWidget *>(obj)) {
            if (auto *combo = qobject_cast<QComboBox *>(w->parent())) {
                if (combo->property("claudeModelCombo").toBool())
                    applyLiveClaudeModelsToCombos();
            }
        }
        if (auto *box = qobject_cast<QMessageBox *>(obj))
            recordMessageBoxNotification(box);
    }
    if (obj == m_userAvatarNavButton && event->type() == QEvent::Enter &&
        m_promptOverlayCollapsed) {
        setPromptOverlayCollapsed(false);
    }
    if (handlePromptPlacementEvent(obj, event))
        return true;
    if (event->type() == QEvent::Close) {
        for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
            if (page.window && obj == page.window) {
                event->ignore();
                const QString id = page.id;
                dockNativePackageWindow(id);
                return true;
            }
        }
    }
    if (obj == m_promptDetachWindow && m_promptDetachWindow) {
        if (event->type() == QEvent::Close) {
            event->ignore();
            setPromptOverlayDetached(false);
            return true;
        }
        if (event->type() == QEvent::Move || event->type() == QEvent::Resize) {
            m_promptDetachGeometry = m_promptDetachWindow->geometry();
        }
    }
    if (event->type() == QEvent::Wheel &&
        (static_cast<QWheelEvent *>(event)->modifiers() & Qt::ControlModifier)) {
        for (QTextEdit *view : m_diffViews) {
            if (view && obj == view->viewport()) {
                adjustDiffFont(
                    static_cast<QWheelEvent *>(event)->angleDelta().y() > 0 ? 1 : -1);
                return true;
            }
        }
    }
    if (event->type() == QEvent::Resize && m_pullDiff &&
        obj == m_pullDiff->viewport()) {
        layoutPullStickyHeader();
        positionPullInlineComposer();
    }
    if (event->type() == QEvent::Resize && m_scmDiff &&
        obj == m_scmDiff->viewport()) {
        layoutScmStickyHeader();
        updateScmNextFileBar();
    }
    if ((event->type() == QEvent::Resize || event->type() == QEvent::Show) &&
        obj == m_commitsStack)
        positionGitPromptOverlay();
    if ((event->type() == QEvent::Resize || event->type() == QEvent::Show) &&
        obj == m_globalOverlayHost)
        positionGlobalFooterOverlays();
    if ((event->type() == QEvent::Resize || event->type() == QEvent::Move ||
         event->type() == QEvent::Show || event->type() == QEvent::Hide) &&
        obj == m_agentDetail)
        positionGlobalFooterOverlays();
    if (event->type() == QEvent::Resize && obj == m_footerUpdateLog) {
        positionFloatingLogButton();
        positionFooterLogPauseButton();
    }
    if (event->type() == QEvent::ContextMenu) {
        if (maybeShowSendToPromptMenu(obj, static_cast<QContextMenuEvent *>(event)))
            return true;
    }
    if (obj == m_navSolanaBalance && event->type() == QEvent::Enter)
        refreshNavSolanaBalance();
    // Clicking the rail chip is the manual "check now": on its own the amount
    // is re-read only once an hour, because a public-RPC read per hover per
    // node is what made that endpoint the busiest thing we call. The empty
    // state paints an "Add SOL" link instead, and refreshNavSolanaBalance()
    // returns early with no address, so its click still just opens settings.
    if (obj == m_navSolanaBalance && event->type() == QEvent::MouseButtonRelease)
        refreshNavSolanaBalance(/*force=*/true);
    if (event->type() == QEvent::MouseButtonRelease) {
        if (auto *w = qobject_cast<QWidget *>(obj)) {
            if (w->property("slashKind").isValid()) {
                activateSlashActionRow(w);
                return true;
            }
        }
    }
    if (obj == m_messageInput && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->matches(QKeySequence::Paste) && trySendClipboardImage())
            return true;
    }
    if (m_mentionCompleterPopup && obj == m_mentionCompleterPopup &&
        event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Tab) {
            const QModelIndex idx = m_mentionCompleterPopup->currentIndex();
            QString name;
            if (idx.isValid())
                name = idx.data(Qt::DisplayRole).toString();
            else if (m_mentionCompleter->setCurrentRow(0))
                name = m_mentionCompleter->currentCompletion();
            if (!name.isEmpty()) {
                insertComposerCompletion(name);
                m_mentionCompleterPopup->hide();
                return true;
            }
        }
    }
    if (obj == m_issueQuickAdd && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->matches(QKeySequence::Paste) && tryPasteImageIntoQuickAdd())
            return true;
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
            !(ke->modifiers() & Qt::ShiftModifier)) {
            if (submitQuickAddToPackage())
                return true;
            if (quickAddShouldFollowUpAgent() && m_quickAddSendToAgentButton)
                m_quickAddSendToAgentButton->click();
            else
                quickAddIssue();
            return true;
        }
        if (ke->key() == Qt::Key_Up && navigateQuickAddHistory(-1))
            return true;
        if (ke->key() == Qt::Key_Down && navigateQuickAddHistory(1))
            return true;
    }
    if (event->type() == QEvent::FocusOut &&
        (obj == m_issueQuickAdd || obj == m_messageInput)) {
        const Qt::FocusReason reason = static_cast<QFocusEvent *>(event)->reason();
        if (reason == Qt::OtherFocusReason || reason == Qt::NoFocusReason) {
            QPointer<QWidget> prompt = qobject_cast<QWidget *>(obj);
            if (prompt && !promptDraftText(prompt).isEmpty()) {
                QTimer::singleShot(0, this, [this, prompt] {
                    if (prompt && prompt->isVisible() && prompt->isEnabled() &&
                        isActiveWindow() &&
                        QApplication::focusWidget() != prompt &&
                        !promptDraftText(prompt).isEmpty())
                        prompt->setFocus(Qt::OtherFocusReason);
                });
            }
        }
    }
    if (obj == m_slashActionsFilter && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        switch (ke->key()) {
        case Qt::Key_Down:
            moveSlashActionsSelection(1);
            return true;
        case Qt::Key_Up:
            moveSlashActionsSelection(-1);
            return true;
        case Qt::Key_Escape:
            if (m_slashActionsPopup)
                m_slashActionsPopup->hide();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_slashActionSelected >= 0 &&
                m_slashActionSelected < m_slashActionRows.size())
                activateSlashActionRow(m_slashActionRows.at(m_slashActionSelected));
            return true;
        default:
            break;
        }
    }
    if (m_issueTable && obj == m_issueTable->viewport() &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseMove ||
         event->type() == QEvent::MouseButtonRelease)) {
        if (handleIssueRowChipClick(static_cast<QMouseEvent *>(event)))
            return true;
        if (handleIssueProgressDrag(static_cast<QMouseEvent *>(event)))
            return true;
    }
    if (m_footerUpdateLog && obj == m_footerUpdateLog->viewport() &&
        (event->type() == QEvent::ToolTip ||
         event->type() == QEvent::MouseButtonRelease) &&
        handleFooterLogViewportEvent(m_footerUpdateLog, event))
        return true;
    if (m_debugLogTail && obj == m_debugLogTail->viewport() &&
        (event->type() == QEvent::ToolTip ||
         event->type() == QEvent::MouseButtonRelease) &&
        handleFooterLogViewportEvent(m_debugLogTail, event))
        return true;
    if (m_settingsLog && obj == m_settingsLog->viewport() &&
        (event->type() == QEvent::MouseButtonPress ||
         event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::ToolTip) &&
        handleSettingsLogViewportEvent(event))
        return true;
    if (obj == m_globalSearch) {
        if (event->type() == QEvent::KeyPress) {
            auto *ke = static_cast<QKeyEvent *>(event);
            const bool open = m_globalSearchPopup && m_globalSearchPopup->isVisible();
            switch (ke->key()) {
            case Qt::Key_Down:
                if (open) { moveGlobalSearchSelection(1); return true; }
                break;
            case Qt::Key_Up:
                if (open) { moveGlobalSearchSelection(-1); return true; }
                break;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                if (open) {
                    activateGlobalSearchItem(m_globalSearchPopup->currentItem());
                    return true;
                }
                break;
            case Qt::Key_Escape:
                if (open) { hideGlobalSearchPopup(); return true; }
                break;
            default:
                break;
            }
        } else if (event->type() == QEvent::FocusOut) {
            hideGlobalSearchPopup();
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

// A QMessageBox that only informs (OK/Close) is mirrored into Pings as it
// shows, and a failure one is also reported as a user-visible error.
void MainWindow::recordMessageBoxNotification(QMessageBox *box)
{
    const QMessageBox::Icon icon = box->icon();
    const bool localOperation =
        box->property(forkmesh::kLocalOperationDialogProperty).toBool();
    using DialogButton = forkmesh::ClientErrorReports::DialogButton;
    QList<DialogButton> buttons;
    for (QAbstractButton *button : box->buttons()) {
        const QMessageBox::StandardButton standard = box->standardButton(button);
        if (standard == QMessageBox::Ok || standard == QMessageBox::Close) {
            buttons.append(DialogButton::Acknowledge);
            continue;
        }
        if (standard != QMessageBox::NoButton) {
            buttons.append(DialogButton::Decision);
            continue;
        }
        switch (box->buttonRole(button)) {
        case QMessageBox::DestructiveRole:
        case QMessageBox::YesRole:
        case QMessageBox::NoRole:
            buttons.append(DialogButton::Decision);
            break;
        case QMessageBox::AcceptRole:
        case QMessageBox::RejectRole:
        case QMessageBox::ApplyRole:
        case QMessageBox::ResetRole:
            buttons.append(DialogButton::Choice);
            break;
        default:
            buttons.append(DialogButton::Aside);
            break;
        }
    }
    const bool informational =
        !forkmesh::ClientErrorReports::dialogAsksQuestion(buttons);
    if (informational) {
        QString text = box->text();
        if (!box->informativeText().isEmpty())
            text += QLatin1Char(' ') + box->informativeText();
        const bool failure = icon == QMessageBox::Warning
                             || icon == QMessageBox::Critical;
        AppNotification item;
        item.title = box->windowTitle().isEmpty()
                         ? QStringLiteral("Desktop alert")
                         : box->windowTitle();
        item.body = text.simplified();
        item.warning = failure;
        item.kind =
            localOperation ? forkmesh::kLocalOperationNoticeKind
                           : QStringLiteral("dialog");
        item.quiet = true;
        const qint64 pingId = recordNotification(item);
        if (failure && !localOperation)
            reportUserVisibleError(QStringLiteral("dialog"),
                                   box->windowTitle(), text, QString(),
                                   pingId);
    }
}

// Tooltips and clicks on the footer log or the debug tail: prompt anchors,
// source anchors, then the line itself. True when the event was consumed.
bool MainWindow::handleFooterLogViewportEvent(QTextEdit *view, QEvent *event)
{
    auto lineAt = [view](const QPoint &pos) -> QString {
        const QTextCursor cur = view->cursorForPosition(pos);
        if (auto *data =
                static_cast<FooterLogLineData *>(cur.block().userData()))
            return data->rawLine;
        return QString();
    };
    if (event->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(event);
        if (!logPromptAnchorLine(view->anchorAt(he->pos()))
                 .isEmpty()) {
            QToolTip::showText(he->globalPos(),
                               QStringLiteral("Add this log entry to the prompt"),
                               view->viewport());
            return true;
        }
        QString sourcePath;
        int sourceLine = 0;
        if (logSourceAnchorTarget(view->anchorAt(he->pos()),
                                  &sourcePath, &sourceLine)) {
            QToolTip::showText(
                he->globalPos(),
                QStringLiteral("Logged from %1:%2 — click to open it in Files")
                    .arg(sourcePath)
                    .arg(sourceLine),
                view->viewport());
            return true;
        }
        const QString line = lineAt(he->pos());
        if (!line.isEmpty()) {
            QToolTip::showText(he->globalPos(),
                               Tooltips::preview(line, 8, 800),
                               view->viewport());
            return true;
        }
    } else { // MouseButtonRelease
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QPoint pos = me->position().toPoint();
            const QString promptLine =
                logPromptAnchorLine(view->anchorAt(pos));
            if (!promptLine.isEmpty()) {
                appendTextToActivePrompt(promptLine);
                return true;
            }
            QString sourcePath;
            int sourceLine = 0;
            if (logSourceAnchorTarget(view->anchorAt(pos),
                                      &sourcePath, &sourceLine)) {
                revealLogSourceInExplorer(sourcePath, sourceLine);
                return true;
            }
            const QString line = lineAt(pos);
            if (!line.isEmpty()) {
                openFullLogAtFooterLine(line);
                return true;
            }
        }
    }
    return false;
}

// Tooltips and clicks on the Settings log: prompt and source anchors. The press
// is consumed too so the release is the one that acts. True when consumed.
bool MainWindow::handleSettingsLogViewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *he = static_cast<QHelpEvent *>(event);
        if (!logPromptAnchorLine(m_settingsLog->anchorAt(he->pos())).isEmpty()) {
            QToolTip::showText(he->globalPos(),
                               QStringLiteral("Add this log entry to the prompt"),
                               m_settingsLog->viewport());
            return true;
        }
        QString sourcePath;
        int sourceLine = 0;
        if (logSourceAnchorTarget(m_settingsLog->anchorAt(he->pos()),
                                  &sourcePath, &sourceLine)) {
            QToolTip::showText(
                he->globalPos(),
                QStringLiteral("Logged from %1:%2 — click to open it in Files")
                    .arg(sourcePath)
                    .arg(sourceLine),
                m_settingsLog->viewport());
            return true;
        }
    } else {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QString anchor =
                m_settingsLog->anchorAt(me->position().toPoint());
            const QString promptLine = logPromptAnchorLine(anchor);
            if (!promptLine.isEmpty()) {
                if (event->type() == QEvent::MouseButtonRelease)
                    appendTextToActivePrompt(promptLine);
                return true;
            }
            QString sourcePath;
            int sourceLine = 0;
            if (logSourceAnchorTarget(anchor, &sourcePath, &sourceLine)) {
                if (event->type() == QEvent::MouseButtonRelease)
                    revealLogSourceInExplorer(sourcePath, sourceLine);
                return true;
            }
        }
    }
    return false;
}

Qt::Edges MainWindow::resizeEdgesAtGlobalPos(const QPoint &globalPos) const
{
    if (!isVisible() || isMaximized() || isFullScreen())
        return {};

    const QPoint pos = mapFromGlobal(globalPos);
    const QRect r = rect();
    if (!r.adjusted(-1, -1, 1, 1).contains(pos))
        return {};

    constexpr int kResizeBorder = 8;
    Qt::Edges edges;
    if (pos.x() <= r.left() + kResizeBorder)
        edges |= Qt::LeftEdge;
    if (pos.x() >= r.right() - kResizeBorder)
        edges |= Qt::RightEdge;
    if (pos.y() <= r.top() + kResizeBorder)
        edges |= Qt::TopEdge;
    if (pos.y() >= r.bottom() - kResizeBorder)
        edges |= Qt::BottomEdge;
    return edges;
}

void MainWindow::updateFramelessResizeCursor(Qt::Edges edges)
{
    if (edges == (Qt::LeftEdge | Qt::TopEdge) ||
        edges == (Qt::RightEdge | Qt::BottomEdge)) {
        if (m_framelessResizeCursorActive)
            qApp->changeOverrideCursor(Qt::SizeFDiagCursor);
        else
            qApp->setOverrideCursor(Qt::SizeFDiagCursor);
        m_framelessResizeCursorActive = true;
        return;
    }
    if (edges == (Qt::RightEdge | Qt::TopEdge) ||
        edges == (Qt::LeftEdge | Qt::BottomEdge)) {
        if (m_framelessResizeCursorActive)
            qApp->changeOverrideCursor(Qt::SizeBDiagCursor);
        else
            qApp->setOverrideCursor(Qt::SizeBDiagCursor);
        m_framelessResizeCursorActive = true;
        return;
    }
    if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        if (m_framelessResizeCursorActive)
            qApp->changeOverrideCursor(Qt::SizeHorCursor);
        else
            qApp->setOverrideCursor(Qt::SizeHorCursor);
        m_framelessResizeCursorActive = true;
        return;
    }
    if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        if (m_framelessResizeCursorActive)
            qApp->changeOverrideCursor(Qt::SizeVerCursor);
        else
            qApp->setOverrideCursor(Qt::SizeVerCursor);
        m_framelessResizeCursorActive = true;
        return;
    }
    if (m_framelessResizeCursorActive) {
        qApp->restoreOverrideCursor();
        m_framelessResizeCursorActive = false;
    }
}

bool MainWindow::handleFramelessResizeEvent(QObject *obj, QEvent *event)
{
    if (!qApp || !testAttribute(Qt::WA_WState_Created))
        return false;

    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::HoverMove: {
        if (auto *mouse = dynamic_cast<QMouseEvent *>(event)) {
            updateFramelessResizeCursor(
                resizeEdgesAtGlobalPos(mouse->globalPosition().toPoint()));
        } else if (auto *hover = dynamic_cast<QHoverEvent *>(event)) {
            updateFramelessResizeCursor(
                resizeEdgesAtGlobalPos(mapToGlobal(hover->position().toPoint())));
        }
        return false;
    }
    case QEvent::Leave:
        if (!resizeEdgesAtGlobalPos(QCursor::pos()))
            updateFramelessResizeCursor({});
        return false;
    case QEvent::MouseButtonPress: {
        auto *mouse = dynamic_cast<QMouseEvent *>(event);
        if (!mouse || mouse->button() != Qt::LeftButton)
            return false;
        const Qt::Edges edges =
            resizeEdgesAtGlobalPos(mouse->globalPosition().toPoint());
        if (!edges)
            return false;
        if (QWindow *handle = windowHandle()) {
            mouse->accept();
            handle->startSystemResize(edges);
            return true;
        }
        return false;
    }
    case QEvent::MouseButtonRelease:
        updateFramelessResizeCursor(resizeEdgesAtGlobalPos(QCursor::pos()));
        return false;
    case QEvent::WindowStateChange:
        updateFramelessResizeCursor({});
        return false;
    default:
        return false;
    }
}

bool MainWindow::maybeShowSendToPromptMenu(QObject *obj, QContextMenuEvent *ce)
{
    QWidget *w = qobject_cast<QWidget *>(obj);
    if (!w || !ce)
        return false;

    QString selected;
    QMenu *menu = nullptr;

    if (auto *label = qobject_cast<QLabel *>(w)) {
        if (!(label->textInteractionFlags() & Qt::TextSelectableByMouse))
            return false;
        selected = label->selectedText();
        if (selected.isEmpty())
            return false;
        menu = new QMenu(this);
        QAction *copy = menu->addAction(tr("Copy"));
        connect(copy, &QAction::triggered, label,
                [label] { QApplication::clipboard()->setText(label->selectedText()); });
    } else {
        QWidget *host = w;
        while (host && !qobject_cast<QTextEdit *>(host) && !qobject_cast<QPlainTextEdit *>(host))
            host = host->parentWidget();
        if (!host || host == m_issueQuickAdd)
            return false;
        if (auto *te = qobject_cast<QTextEdit *>(host)) {
            selected = te->textCursor().selectedText();
            if (selected.isEmpty())
                return false;
            menu = te->createStandardContextMenu(ce->pos());
        } else if (auto *pte = qobject_cast<QPlainTextEdit *>(host)) {
            selected = pte->textCursor().selectedText();
            if (selected.isEmpty())
                return false;
            menu = pte->createStandardContextMenu(ce->pos());
        } else {
            return false;
        }
    }
    if (!menu)
        return false;

    const QString promptText = QString(selected).replace(QChar(0x2029), QLatin1Char('\n'));
    menu->addSeparator();
    QAction *sendToPrompt = menu->addAction(tr("Send to Prompt"));
    QAction *searchCodebase = menu->addAction(tr("Search Codebase"));
    QAction *chosen = menu->exec(ce->globalPos());
    if (chosen == sendToPrompt)
        appendTextToActivePrompt(promptText);
    else if (chosen == searchCodebase)
        openSearchResultsPage(promptText);
    delete menu;
    return true;
}

void MainWindow::appendTextToActivePrompt(const QString &text)
{
    QPlainTextEdit *target = m_issueQuickAdd;
    if (!target)
        return;
    QTextCursor cursor = target->textCursor();
    cursor.movePosition(QTextCursor::End);
    const QString existing = target->toPlainText();
    if (!existing.isEmpty() && !existing.endsWith(QLatin1Char('\n')))
        cursor.insertText(QStringLiteral("\n"));
    cursor.insertText(text);
    target->setTextCursor(cursor);
    target->setFocus();
}

void MainWindow::toggleIssueStatus()
{
    if (m_currentIssueNumber < 0)
        return;
    QString status = "open";
    for (const Issue &issue : m_currentIssues)
        if (issue.number == m_currentIssueNumber)
            status = issue.status;
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    const bool closing = status == "open";
    const int nextIssue = closing ? nextVisibleIssueAfter(m_currentIssueNumber) : -1;
    if (!store.setStatus(m_currentIssueNumber, closing ? "closed" : "open",
                         &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update issue status." : error,
                             true);
        return;
    }
    if (closing && nextIssue > 0) {
        m_selectIssueOnReload = nextIssue;
    } else {
        m_keepCurrentOnReload = true;
    }
    reloadIssues();
    setIssueInlineNotice(closing ? "Issue closed." : "Issue reopened.");
}

int MainWindow::nextVisibleIssueAfter(int number) const
{
    if (!m_issueTable)
        return -1;
    const int rows = m_issueTable->rowCount();
    int idx = -1;
    for (int r = 0; r < rows; ++r) {
        const QTableWidgetItem *item = m_issueTable->item(r, 0);
        if (item && item->data(Qt::UserRole).toInt() == number) {
            idx = r;
            break;
        }
    }
    if (idx < 0)
        return -1;
    if (idx + 1 < rows) {
        if (const QTableWidgetItem *item = m_issueTable->item(idx + 1, 0))
            return item->data(Qt::UserRole).toInt();
    }
    if (idx - 1 >= 0) {
        if (const QTableWidgetItem *item = m_issueTable->item(idx - 1, 0))
            return item->data(Qt::UserRole).toInt();
    }
    return -1;
}

void MainWindow::showIssueListContextMenu(const QPoint &viewportPos)
{
    if (!m_issueTable)
        return;
    const QPoint global = m_issueTable->viewport()->mapToGlobal(viewportPos);
    const QModelIndex index = m_issueTable->indexAt(viewportPos);
    const QTableWidgetItem *numItem =
        index.isValid() ? m_issueTable->item(index.row(), 0) : nullptr;
    if (!numItem) {
        QMenu menu(m_issueTable);
        QAction *newIssue = menu.addAction(QStringLiteral("New issue…"));
        newIssue->setEnabled(issuesRepoIndex() >= 0);
        if (menu.exec(global) == newIssue)
            promptNewIssue();
        return;
    }
    const int number = numItem->data(Qt::UserRole).toInt();
    // Selecting the row shows its detail, which is what the inline sidebar
    // editors (labels, milestone, assignees…) open into.
    m_issueTable->selectRow(index.row());
    if (m_currentIssueNumber != number)
        showIssue(number);
    QMenu *menu = buildIssueListContextMenu(number, m_issueTable);
    if (!menu)
        return;
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(global);
}

QMenu *MainWindow::buildIssueListContextMenu(int number, QWidget *parent)
{
    const Issue *issue = nullptr;
    for (const Issue &candidate : std::as_const(m_currentIssues))
        if (candidate.number == number)
            issue = &candidate;
    if (!issue)
        return nullptr;
    const bool writable = issueStoreForCurrentRepo().canWrite();
    const bool open = issue->status != QLatin1String("closed");

    auto *menu = new QMenu(parent);
    // Every action works on the current issue; a reload while the menu was up
    // may have moved the selection, so re-point it at this row first.
    auto add = [this, menu, number](const QString &text, bool enabled,
                                    std::function<void()> run,
                                    QMenu *into = nullptr) {
        QAction *action = (into ? into : menu)->addAction(text);
        action->setEnabled(enabled);
        connect(action, &QAction::triggered, this, [this, number, run]() {
            if (m_currentIssueNumber != number)
                showIssue(number);
            if (m_currentIssueNumber == number)
                run();
        });
        return action;
    };

    add(open ? QStringLiteral("Close issue") : QStringLiteral("Reopen issue"),
        writable, [this]() { toggleIssueStatus(); });
    add(QStringLiteral("Vote"), availableCredits() > 0,
        [this]() { voteOnCurrentIssue(); });
    menu->addSeparator();

    add(QStringLiteral("Edit title…"), writable,
        [this]() { promptEditIssueTitle(); });
    QMenu *edit = menu->addMenu(QStringLiteral("Edit"));
    edit->setEnabled(writable);
    add(QStringLiteral("Labels…"), writable, [this]() { editIssueLabels(); }, edit);
    add(QStringLiteral("Milestone…"), writable,
        [this]() { editIssueMilestone(); }, edit);
    add(QStringLiteral("Assignees…"), writable,
        [this]() { editIssueAssignees(); }, edit);
    add(QStringLiteral("Dates…"), writable, [this]() { editIssueDates(); }, edit);
    add(QStringLiteral("Progress…"), writable,
        [this]() { editIssueProgress(); }, edit);
    add(QStringLiteral("Bounty…"), writable, [this]() { editIssueBounty(); }, edit);
    QMenu *priority = menu->addMenu(QStringLiteral("Priority"));
    priority->setEnabled(writable);
    add(QStringLiteral("Raise priority"), writable,
        [this]() { nudgeIssuePriority(-1); }, priority);
    add(QStringLiteral("Lower priority"), writable,
        [this]() { nudgeIssuePriority(1); }, priority);
    add(QStringLiteral("Set priority…"), writable,
        [this]() { editIssuePriority(); }, priority);
    menu->addSeparator();

    add(QStringLiteral("Copy title"), true, [this]() { copyIssueToClipboard(); });
    add(QStringLiteral("Copy issue and comments"), true,
        [this]() { copyIssueThreadToClipboard(); });
    add(QStringLiteral("Copy link"), true, [this, number]() {
        const int repoIdx = issuesRepoIndex();
        if (repoIdx < 0)
            return;
        const RepositoryRecord &repo = m_repositories.at(repoIdx);
        QApplication::clipboard()->setText(
            QStringLiteral("forkmesh://issue/%1/%2/%3")
                .arg(repo.owner, repo.name)
                .arg(number));
        setIssueInlineNotice("Issue link copied.");
    });
    menu->addSeparator();

    add(QStringLiteral("Move to task…"),
        writable && !m_issueHistoryDeleteInProgress && !m_issueTaskMoveInFlight,
        [this]() { moveCurrentIssueToTask(); });
    add(QStringLiteral("Delete issue…"),
        writable && !m_issueHistoryDeleteInProgress,
        [this]() { deleteCurrentIssue(); });
    return menu;
}

#ifdef FORKMESH_WINDOW_TESTS
QStringList MainWindow::testIssueListContextMenuActions(int number)
{
    QStringList out;
    QMenu *menu = buildIssueListContextMenu(number, this);
    if (!menu)
        return out;
    const auto describe = [&out](const QList<QAction *> &actions,
                                 const QString &prefix,
                                 const auto &self) -> void {
        for (QAction *action : actions) {
            if (action->isSeparator()) {
                out << QStringLiteral("-");
                continue;
            }
            out << prefix + action->text() +
                       (action->isEnabled() ? QString()
                                            : QStringLiteral(" (disabled)"));
            if (QMenu *sub = action->menu())
                self(sub->actions(), prefix + action->text() + QLatin1Char('/'),
                     self);
        }
    };
    describe(menu->actions(), QString(), describe);
    delete menu;
    return out;
}
#endif

void MainWindow::deleteCurrentIssue()
{
    if (m_currentIssueNumber < 0)
        return;
    const int number = m_currentIssueNumber;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Delete issue"));
    box.setText(QStringLiteral("Delete issue #%1?").arg(number));
    box.setInformativeText(QStringLiteral(
        "Delete hides it everywhere and syncs the deletion to peers; the issue "
        "stays in git history.\n\n"
        "Delete with history also purges it from all of git history — thorough but "
        "slower and unrecoverable."));
    QPushButton *regularBtn =
        box.addButton(QStringLiteral("Delete"), QMessageBox::AcceptRole);
    QPushButton *historyBtn = box.addButton(QStringLiteral("Delete with history"),
                                            QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(regularBtn);
    box.exec();
    QAbstractButton *clicked = box.clickedButton();
    if (clicked == historyBtn) {
        deleteCurrentIssueWithHistory(number);
        return;
    }
    if (clicked != regularBtn)
        return;

    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.removeIssue(number, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not delete the issue." : error,
                             true);
        return;
    }
    removeIssueFromCurrentList(number);
    setIssueInlineNotice("Issue deleted.");
}

void MainWindow::moveCurrentIssueToTask()
{
    if (m_currentIssueNumber < 0 || m_issueTaskMoveInFlight)
        return;
    const int repoIndex = issuesRepoIndex();
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    const Issue *issue = nullptr;
    for (const Issue &candidate : std::as_const(m_currentIssues))
        if (candidate.number == m_currentIssueNumber)
            issue = &candidate;
    if (!issue)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        setIssueInlineNotice(
            "Only the repository's host can move this issue to a task.", true);
        return;
    }

    const int number = issue->number;
    QString openId;
    QString body;
    for (const IssueEvent &ev : issue->events) {
        if (ev.type == QLatin1String("open") && openId.isEmpty()) {
            openId = ev.id;
            body = ev.body;
        } else if (ev.type == QLatin1String("edit") && !openId.isEmpty() &&
                   ev.target == openId) {
            body = ev.body;
        }
    }
    const RepositoryRecord &repo = m_repositories.at(repoIndex);
    QString title = issue->title.simplified();
    if (title.isEmpty())
        title = QStringLiteral("Issue #%1").arg(number);
    if (title.size() > 160)
        title = title.left(159).trimmed() + QString::fromUtf8("\xE2\x80\xA6");
    const QString source =
        QStringLiteral("Moved from issue %1/%2#%3 by %4.")
            .arg(repo.owner, repo.name)
            .arg(number)
            .arg(chatDisplayName().isEmpty() ? QStringLiteral("a repository manager")
                                              : chatDisplayName());
    body = body.trimmed();
    const int room = 4000 - static_cast<int>(source.size()) - 2;
    if (body.size() > room)
        body = body.left(room - 1) + QString::fromUtf8("\xE2\x80\xA6");
    const QString details =
        body.isEmpty() ? source : body + QStringLiteral("\n\n") + source;

    if (QMessageBox::question(
            this, QStringLiteral("Move issue to task"),
            QStringLiteral("Move issue #%1 \"%2\" to an organization task?\n\n"
                           "The task is created from its title and description, "
                           "then the issue is deleted. If the issue cannot be "
                           "deleted, the new task is moved to trash.")
                .arg(number)
                .arg(title),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) !=
        QMessageBox::Yes)
        return;

    m_issueTaskMoveInFlight = true;
    updateIssueActionState();
    const QJsonObject request{
        {QStringLiteral("title"), title},
        {QStringLiteral("details"), details},
        {QStringLiteral("department"), QStringLiteral("general")},
        {QStringLiteral("destination"), QStringLiteral("department")},
        {QStringLiteral("assigneeKind"), QStringLiteral("unassigned")},
        {QStringLiteral("repository"), repo.owner + QLatin1Char('/') + repo.name},
        {QStringLiteral("priority"), 50},
    };
    requestOrganizationTasks(
        QByteArrayLiteral("POST"), QStringLiteral("/api/tasks"), request,
        [this, store, number, repoIndex](bool ok, const QJsonObject &payload,
                                         const QString &error) mutable {
            if (!ok) {
                m_issueTaskMoveInFlight = false;
                updateIssueActionState();
                setIssueInlineNotice(
                    QStringLiteral("The task could not be created, so the "
                                   "issue was kept: %1")
                        .arg(error),
                    true);
                return;
            }
            const QJsonObject task =
                payload.value(QStringLiteral("task")).toObject();
            const QString taskId = task.value(QStringLiteral("id")).toString();
            QString removeError;
            if (!store.removeIssue(number, &removeError)) {
                const QString why = removeError.isEmpty()
                                        ? QStringLiteral("Could not delete the issue.")
                                        : removeError;
                requestOrganizationTasks(
                    QByteArrayLiteral("DELETE"),
                    QStringLiteral("/api/tasks/") + taskId,
                    QJsonObject{{QStringLiteral("reason"),
                                 QStringLiteral("Issue #%1 could not be deleted.")
                                     .arg(number)}},
                    [this, why](bool undone, const QJsonObject &,
                                const QString &) {
                        m_issueTaskMoveInFlight = false;
                        updateIssueActionState();
                        setIssueInlineNotice(
                            why + (undone
                                       ? QStringLiteral(" The new task was moved to "
                                                        "trash, so the issue is "
                                                        "unchanged.")
                                       : QStringLiteral(" The new task could not be "
                                                        "removed; delete it from "
                                                        "Tasks.")),
                            true);
                    });
                return;
            }
            m_issueTaskMoveInFlight = false;
            if (issuesRepoIndex() == repoIndex)
                removeIssueFromCurrentList(number);
            updateIssueActionState();
            openCreatedOrganizationTask(task);
            flashMessage(QStringLiteral("Issue #%1 moved to a task.").arg(number));
        });
}

void MainWindow::deleteCurrentIssueWithHistory(int number)
{
    if (m_issueHistoryDeleteInProgress) {
        setIssueInlineNotice(
            QStringLiteral("Issue history deletion is already running."));
        return;
    }
    m_issueHistoryDeleteInProgress = true;
    setIssueInlineNotice(
        QStringLiteral("Deleting issue #%1 and rewriting history… this can take a "
                       "while.")
            .arg(number));
    if (m_issueDeleteButton)
        m_issueDeleteButton->setEnabled(false);
    QApplication::setOverrideCursor(Qt::BusyCursor);

    // deleteIssue only shells out to git (no event signing), so it's safe to run
    // on a worker thread with a copy of the store. Results travel back via shared
    // state read in the finished handler on the main thread.
    IssueStore store = issueStoreForCurrentRepo();
#ifdef FORKMESH_WINDOW_TESTS
    auto testRunner = m_testIssueHistoryDeleteRunner;
#endif
    auto ok = std::make_shared<bool>(false);
    auto error = std::make_shared<QString>();
    QThread *worker = QThread::create([store, number, ok, error
#ifdef FORKMESH_WINDOW_TESTS
                                       , testRunner
#endif
    ]() mutable {
        QString err;
#ifdef FORKMESH_WINDOW_TESTS
        if (testRunner) {
            *ok = testRunner(number, &err);
        } else
#endif
        {
            *ok = store.deleteIssue(number, &err);
        }
        *error = err;
    });
    connect(worker, &QThread::finished, this,
            [this, worker, ok, error, number]() {
                m_issueHistoryDeleteInProgress = false;
                QApplication::restoreOverrideCursor();
                if (m_issueDeleteButton)
                    m_issueDeleteButton->setEnabled(true);
                if (*ok) {
                    removeIssueFromCurrentList(number);
                    setIssueInlineNotice("Issue deleted with history.");
                } else {
                    setIssueInlineNotice(error->isEmpty()
                                             ? QStringLiteral("Could not delete the issue.")
                                             : *error,
                                         true);
                }
                worker->deleteLater();
            });
    worker->start();
}

void MainWindow::editIssueLabels()
{
    if (m_currentIssueNumber < 0)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (m_issueLabelsStack)
        m_issueLabelsStack->setCurrentIndex(1);
    if (m_issueLabelsEdit) {
        m_issueLabelsEdit->setFocus();
        m_issueLabelsEdit->selectAll();
    }
}

void MainWindow::saveIssueLabelsInline()
{
    if (m_currentIssueNumber < 0 || !m_issueLabelsEdit)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    const QStringList labels = splitIssueFieldList(m_issueLabelsEdit->text());
    if (!store.setLabels(m_currentIssueNumber, labels, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update labels." : error,
                             true);
        return;
    }
    setIssueInlineNotice("Labels updated.");
    reloadIssues();
}

void MainWindow::editIssueMilestone()
{
    if (m_currentIssueNumber < 0)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (m_issueMilestoneStack)
        m_issueMilestoneStack->setCurrentIndex(1);
    if (m_issueMilestoneEdit)
        m_issueMilestoneEdit->setFocus();
}

void MainWindow::saveIssueMilestoneInline()
{
    if (m_currentIssueNumber < 0 || !m_issueMilestoneEdit)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    const QString milestone = m_issueMilestoneEdit->currentData().toString();
    if (!store.setMilestone(m_currentIssueNumber, milestone, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update milestone." : error,
                             true);
        return;
    }
    setIssueInlineNotice("Milestone updated.");
    reloadIssues();
}

void MainWindow::editIssueDates()
{
    if (m_currentIssueNumber < 0)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (m_issueDatesStack)
        m_issueDatesStack->setCurrentIndex(1);
    if (m_issueStartDateEdit)
        m_issueStartDateEdit->setFocus();
}

void MainWindow::saveIssueDatesInline()
{
    if (m_currentIssueNumber < 0 || !m_issueStartDateEdit || !m_issueEndDateEdit)
        return;
    auto dateMs = [](QDateEdit *edit, QCheckBox *enable) -> qint64 {
        if (!enable || !enable->isChecked())
            return 0;
        return edit->date().startOfDay().toMSecsSinceEpoch();
    };
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.setDates(m_currentIssueNumber,
                        dateMs(m_issueStartDateEdit, m_issueStartDateEnable),
                        dateMs(m_issueEndDateEdit, m_issueEndDateEnable),
                        &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update dates." : error,
                             true);
        return;
    }
    setIssueInlineNotice("Dates updated.");
    reloadIssues();
}

void MainWindow::editIssuePriority()
{
    if (m_currentIssueNumber < 0)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (m_issuePriorityStack)
        m_issuePriorityStack->setCurrentIndex(1);
    if (m_issuePriorityEdit)
        m_issuePriorityEdit->setFocus();
}

void MainWindow::saveIssuePriorityInline()
{
    if (m_currentIssueNumber < 0 || !m_issuePriorityEdit)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    const int priority = m_issuePriorityEdit->value();
    if (!store.setPriority(m_currentIssueNumber, priority, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update priority." : error,
                             true);
        return;
    }
    setIssueInlineNotice(priority > 0 ? "Priority updated." : "Priority cleared.");
    reloadIssues();
}

void MainWindow::nudgeIssuePriority(int direction)
{
    if (m_currentIssueNumber < 0 || direction == 0)
        return;
    const int kHighest = 1;
    const int kLowest = 8999;
    const int step = 1;
    int original = 0;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.number == m_currentIssueNumber) {
            original = issue.priority;
            break;
        }
    const int base = original > 0 ? original : kLowest;
    const int next = qBound(kHighest, base + direction * step, kLowest);
    if (next == original) {
        setIssueInlineNotice(direction < 0 ? "Already at the highest priority."
                                           : "Already at the lowest priority.");
        return;
    }
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.setPriority(m_currentIssueNumber, next, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update priority." : error,
                             true);
        return;
    }
    setIssueInlineNotice(QStringLiteral("Priority set to %1.").arg(next));
    reloadIssues();
}

void MainWindow::syncIssueDetailTabBar()
{
    if (!m_issueDetailTabs || !m_issueDetailTabs->tabBar())
        return;
    m_issueDetailTabs->tabBar()->setVisible(
        m_issueFilesTabIndex >= 0 &&
        m_issueDetailTabs->isTabVisible(m_issueFilesTabIndex));
}

void MainWindow::applyIssueSearchQuery(const QString &query)
{
    // The visible box is the top search; keep it in step without waking its
    // global result popup, then drive the list from the issue-page mirror.
    if (m_globalSearch) {
        QSignalBlocker block(m_globalSearch);
        m_globalSearch->setText(query);
    }
    if (m_issueSearch && m_issueSearch->text() == query) {
        refreshIssueList();
        return;
    }
    if (m_issueSearch)
        m_issueSearch->setText(query); // textChanged refreshes the list
    else
        refreshIssueList();
}

void MainWindow::showIssueListForQuery(const QString &query)
{
    if (m_issueTabGroup && m_issueTabGroup->button(0))
        m_issueTabGroup->button(0)->setChecked(true);
    selectIssueListTab(0);
    applyIssueSearchQuery(query);
}

bool MainWindow::handleIssueRowChipClick(QMouseEvent *ev)
{
    const bool press = ev->type() == QEvent::MouseButtonPress;
    const bool move = ev->type() == QEvent::MouseMove;
    if (!press && !move)
        return false;
    if (press && ev->button() != Qt::LeftButton)
        return false;

    QString query;
    int voteIssue = 0;      // thumbs-up chip: cast a vote on that row's issue
    QString openAttachment; // attachment preview: open the file itself
    const QModelIndex idx = m_issueTable->indexAt(ev->pos());
    if (idx.isValid() && idx.column() == 1) {
        const IssueRowGeometry g = issueRowGeometry(
            m_issueTable->visualRect(idx), m_issueTable->font(), idx);
        const QPoint pos = ev->pos();
        for (const auto &pill : g.labels)
            if (pill.second.contains(pos))
                query = QStringLiteral("label:\"%1\"").arg(pill.first);
        if (query.isEmpty() && !g.milestone.isNull() && g.milestone.contains(pos))
            query = QStringLiteral("milestone:\"%1\"").arg(g.milestoneName);
        if (query.isEmpty() && g.votes.contains(pos))
            voteIssue = idx.data(Qt::UserRole).toInt();
        if (query.isEmpty() && !voteIssue && !g.files.isNull() &&
            g.files.contains(pos))
            openAttachment = idx.data(IssueAttachmentRole).toString();
    }

    if (move) {
        const Qt::CursorShape want =
            query.isEmpty() && !voteIssue && openAttachment.isEmpty()
                ? Qt::ArrowCursor
                : Qt::PointingHandCursor;
        if (m_issueTable->viewport()->cursor().shape() != want)
            m_issueTable->viewport()->setCursor(want);
        return false; // hover only — never swallow the view's own tracking
    }
    if (voteIssue > 0) {
        // Same deferral as the filter chips: voting rebuilds the list, so it
        // must not run inside the view's own mouse handler.
        QTimer::singleShot(0, this, [this, voteIssue] { voteOnIssue(voteIssue); });
        return true;
    }
    if (!openAttachment.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(openAttachment));
        return true;
    }
    if (query.isEmpty())
        return false;
    // Rebuilding the list from inside the view's own mouse handler would delete
    // the item under the cursor mid-event, so hand it to the next event loop.
    QTimer::singleShot(0, this, [this, query] { applyIssueSearchQuery(query); });
    return true; // consume: filtering by a chip must not also select the row
}

bool MainWindow::handleIssueProgressDrag(QMouseEvent *ev)
{
    constexpr int kProgressCol = 11;
    if (ev->type() == QEvent::MouseButtonPress) {
        if (ev->button() != Qt::LeftButton)
            return false;
        const QModelIndex idx = m_issueTable->indexAt(ev->pos());
        if (!idx.isValid() || idx.column() != kProgressCol ||
            !m_issueTable->item(idx.row(), kProgressCol))
            return false;
        m_issueProgressDragRow = idx.row();
        applyIssueProgressDragAt(ev->pos());
        return true; // consume so the click doesn't select/open the row
    }
    if (m_issueProgressDragRow < 0)
        return false; // not a progress drag we started
    if (ev->type() == QEvent::MouseMove) {
        if (!(ev->buttons() & Qt::LeftButton))
            return false;
        applyIssueProgressDragAt(ev->pos());
        return true;
    }
    commitIssueProgressDrag();
    m_issueProgressDragRow = -1;
    return true;
}

void MainWindow::applyIssueProgressDragAt(const QPoint &pos)
{
    QTableWidgetItem *item = m_issueTable->item(m_issueProgressDragRow, 11);
    if (!item)
        return;
    const int pct = progressPctForX(m_issueTable->visualItemRect(item), pos.x(),
                                    m_issueTable->fontMetrics());
    item->setData(kProgressBarRole, pct);
    item->setData(kTableSortRole, pct);
    item->setToolTip(QStringLiteral("%1% complete").arg(pct));
}

void MainWindow::commitIssueProgressDrag()
{
    QTableWidgetItem *progressItem = m_issueTable->item(m_issueProgressDragRow, 11);
    QTableWidgetItem *numberItem = m_issueTable->item(m_issueProgressDragRow, 0);
    if (!progressItem || !numberItem)
        return;
    const int number = numberItem->data(Qt::UserRole).toInt();
    const int pct = qBound(0, progressItem->data(kProgressBarRole).toInt(), 100);
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.setProgress(number, pct, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update progress." : error,
                             true);
        return;
    }
    setIssueInlineNotice(
        QStringLiteral("Issue #%1 progress set to %2%.").arg(number).arg(pct));
    reloadIssues();
}

void MainWindow::nudgeIssueProgress(int deltaPercent)
{
    if (m_currentIssueNumber < 0 || deltaPercent == 0)
        return;
    int current = 0;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.number == m_currentIssueNumber) {
            current = qBound(0, issue.progress, 100);
            break;
        }
    const int next = qBound(0, current + deltaPercent, 100);
    if (next == current) {
        setIssueInlineNotice(deltaPercent > 0 ? "Already at 100% complete."
                                              : "Already at 0%.");
        return;
    }
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.setProgress(m_currentIssueNumber, next, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update progress." : error,
                             true);
        return;
    }
    setIssueInlineNotice(QStringLiteral("Progress set to %1%.").arg(next));
    reloadIssues();
}

double MainWindow::openAiEstimateUsd(const Issue &issue)
{
    int chars = issue.title.size();
    for (const IssueEvent &ev : issue.events) {
        if (ev.type == QLatin1String("open")) {
            chars += ev.body.size();
            break;
        }
    }
    const double specTokens = chars / 4.0;
    const double inputTokens = 12000.0 + specTokens * 3.0; // context + re-reads
    const double outputTokens = 3000.0 + specTokens * 2.0; // the patch + messages
    const double usd =
        (inputTokens * 1.25 + outputTokens * 10.0) / 1000000.0;
    return qMax(0.05, usd);
}

void MainWindow::editIssueProgress()
{
    if (m_currentIssueNumber < 0)
        return;
    int current = 0;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.number == m_currentIssueNumber) {
            current = qBound(0, issue.progress, 100);
            break;
        }
    bool ok = false;
    const int progress = QInputDialog::getInt(
        this, QStringLiteral("Set progress"),
        QString::fromUtf8("Percent complete (0\xE2\x80\x93""100):"), current, 0, 100, 5,
        &ok);
    if (!ok)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    QString error;
    if (!store.setProgress(m_currentIssueNumber, progress, &error)) {
        setIssueInlineNotice(error.isEmpty() ? "Could not update progress." : error,
                             true);
        return;
    }
    setIssueInlineNotice(QStringLiteral("Progress set to %1%.").arg(progress));
    reloadIssues();
}

int MainWindow::estimateIssueProgress(const Issue &issue,
                                      const QSet<int> &mergedIssues) const
{
    if (issue.status == QLatin1String("closed") ||
        mergedIssues.contains(issue.number))
        return 100;
    if (const AgentSession *session = latestAgentSessionForIssue(issue.number)) {
        const QString s = session->status;
        if (s == AgentStatus::Success)
            return session->prNumber > 0 ? 90 : 75;
        if (s == AgentStatus::Running || s == AgentStatus::Waiting)
            return 40;
        if (s == AgentStatus::Queued)
            return 15;
        if (s == AgentStatus::Failed || s == AgentStatus::Stopped)
            return 10;
    }
    if (!issue.assignees.isEmpty())
        return 10;
    return 0;
}

void MainWindow::reprioritizeBacklog()
{
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        setIssueInlineNotice("This repo is read-only here; can't reprioritize.", true);
        return;
    }

    QSet<int> mergedIssues;
    for (const PullRequest &pr : pullStoreForCurrentRepo().loadAll())
        if (pr.status == QLatin1String("merged"))
            for (const int number : issuesLinkedFromPull(pr))
                mergedIssues.insert(number);

    QList<Issue> todo;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.status != QLatin1String("closed") && issue.priority == 0)
            todo.append(issue);

    int progressPending = 0;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.progress == 0 && estimateIssueProgress(issue, mergedIssues) > 0)
            ++progressPending;

    if (todo.isEmpty() && progressPending == 0) {
        setIssueInlineNotice("Nothing to triage: priorities and progress are set.");
        return;
    }
    const int answer = QMessageBox::question(
        this, QStringLiteral("Reprioritize backlog"),
        QStringLiteral("Assign a priority and an MVP/Phase 2 label to %1 open, "
                       "unprioritized issue(s), and estimate progress for %2 "
                       "issue(s) from whether the work landed (closed / merged "
                       "PR / agent activity)?\n\nIssues with votes are treated as "
                       "MVP; the rest become Phase 2.")
            .arg(todo.size())
            .arg(progressPending),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes)
        return;

    std::sort(todo.begin(), todo.end(), [](const Issue &a, const Issue &b) {
        if (a.votes != b.votes)
            return a.votes > b.votes;
        return a.number < b.number;
    });

    int done = 0, failed = 0, prio = 1;
    for (const Issue &issue : std::as_const(todo)) {
        const bool mvp = issue.votes > 0;
        const QString phaseLabel =
            mvp ? QStringLiteral("MVP") : QStringLiteral("Phase 2");
        QStringList labels = issue.labels;
        labels.removeAll(QStringLiteral("MVP"));
        labels.removeAll(QStringLiteral("Phase 2"));
        labels << phaseLabel;
        QString error;
        const bool okLabels = store.setLabels(issue.number, labels, &error);
        const bool okPrio =
            store.setPriority(issue.number, qMin(8999, prio), &error);
        if (okLabels && okPrio)
            ++done;
        else
            ++failed;
        ++prio;
    }

    int progressed = 0;
    for (const Issue &issue : std::as_const(m_currentIssues)) {
        if (issue.progress != 0)
            continue;
        const int pct = estimateIssueProgress(issue, mergedIssues);
        if (pct <= 0)
            continue;
        QString error;
        if (store.setProgress(issue.number, pct, &error))
            ++progressed;
    }

    setIssueInlineNotice(
        failed == 0
            ? QStringLiteral("Prioritized %1 issue(s); estimated progress on %2.")
                  .arg(done)
                  .arg(progressed)
            : QStringLiteral("Prioritized %1 issue(s) (%2 failed); estimated "
                             "progress on %3.")
                  .arg(done)
                  .arg(failed)
                  .arg(progressed),
        failed != 0);
    reloadIssues();
}

QString MainWindow::currentRepoReadme() const
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return QString();
    const RepositoryRecord repo = writableRecordFor(m_repositories.at(idx));

    const QString workTree = repo.localPath.trimmed();
    if (!workTree.isEmpty()) {
        QDir dir(workTree);
        QString chosen;
        for (const QString &name : dir.entryList(QDir::Files)) {
            if (!name.startsWith(QStringLiteral("README"), Qt::CaseInsensitive))
                continue;
            if (name.compare(QStringLiteral("README.md"), Qt::CaseInsensitive) ==
                0) {
                chosen = name;
                break;
            }
            if (chosen.isEmpty())
                chosen = name;
        }
        if (!chosen.isEmpty()) {
            QFile file(dir.filePath(chosen));
            if (file.open(QIODevice::ReadOnly))
                return QString::fromUtf8(file.readAll());
        }
    }

    const QString mirror = repo.mirrorPath.trimmed();
    if (!mirror.isEmpty() && QDir(mirror).exists()) {
        for (const QString &name :
             {QStringLiteral("README.md"), QStringLiteral("README"),
              QStringLiteral("readme.md")}) {
            QByteArray out;
            if (runGitCapture(mirror, {"show", "HEAD:" + name}, &out, nullptr) &&
                !out.isEmpty() && !out.contains('\0'))
                return QString::fromUtf8(out);
        }
    }
    return QString();
}

void MainWindow::prioritizeIssuesFromReadme()
{
    if (m_prioritizeInFlight || !m_networkAccess)
        return;

    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        setIssueInlineNotice("This repo is read-only here; can't reprioritize.",
                             true);
        return;
    }

    QList<Issue> open;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.status != QLatin1String("closed"))
            open.append(issue);
    if (open.size() < 2) {
        setIssueInlineNotice("Need at least two open issues to prioritize.");
        return;
    }

    QString readme = currentRepoReadme().trimmed();
    if (readme.isEmpty()) {
        setIssueInlineNotice(
            "No README found for this repo to prioritize against.", true);
        return;
    }
    if (readme.size() > 8000)
        readme = readme.left(8000) + QStringLiteral("\n\n[README truncated]");

    const QString provider = m_issuePrioritizeAgentCombo
                                 ? m_issuePrioritizeAgentCombo->currentData()
                                       .toString()
                                 : defaultAgentProvider();
    const bool claude = agentIsClaudeProvider(provider);
    const bool claudeCode = provider == QLatin1String("claude-code");
    // "Claude Code" authenticates with the claude.ai subscription
    // OAuth token the CLI keeps in ~/.claude/.credentials.json, never a metered
    // API key. Read that token first and ignore any stored Claude API key, so an
    // explicit Claude Code selection can't silently fall through to the
    // credit-billed API and fail with "credit balance too low". "Claude API" and
    // "OpenAI API" keep using their respective keys.
    const QString oauthToken = claudeCode ? claudeCodeOAuthToken() : QString();
    const QString apiKey =
        claudeCode ? QString()
                   : (claude ? QSettings().value(kClaudeApiKeySetting)
                             : QSettings().value(kCodexApiKeySetting))
                         .toString()
                         .trimmed();
    if (apiKey.isEmpty() && oauthToken.isEmpty()) {
        setIssueInlineNotice(
            claudeCode
                ? "Sign in to Claude Code first (run `claude` and log in)."
                : claude ? "Add a Claude API key in Settings first."
                         : "Add an OpenAI API key in Settings first.",
            true);
        return;
    }

    QStringList lines;
    for (const Issue &issue : std::as_const(open)) {
        QString line =
            QStringLiteral("#%1: %2").arg(issue.number).arg(issue.title.trimmed());
        QString body;
        for (const IssueEvent &ev : issue.events)
            if (ev.type == QLatin1String("open")) {
                body = ev.body.trimmed();
                break;
            }
        if (!body.isEmpty()) {
            body = body.simplified();
            if (body.size() > 200)
                body = body.left(200) + QString::fromUtf8("\xE2\x80\xA6");
            line += QString::fromUtf8(" \xE2\x80\x94 ") + body;
        }
        lines << line;
    }

    const QString task =
        QStringLiteral(
            "%1\n\n----- README -----\n%2\n\n----- OPEN ISSUES -----\n%3")
            .arg(prioritizePromptSetting(), readme, lines.join('\n'));
    const QString model =
        claude ? QStringLiteral("claude-haiku-4-5") : kIssueAskAiModel;
    const int outTok = qBound(256, open.size() * 8 + 256, 4000);

    QNetworkReply *reply = nullptr;
    if (claude) {
        QJsonObject payload;
        payload.insert("model", model);
        payload.insert("max_tokens", outTok);
        QJsonArray messages;
        QJsonObject um;
        um.insert("role", "user");
        um.insert("content", task);
        messages.append(um);
        payload.insert("messages", messages);
        QNetworkRequest req(
            QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
        if (!oauthToken.isEmpty()) {
            // Claude Code's subscription OAuth token authenticates with a Bearer
            // header and requires the Claude Code system identity.
            req.setRawHeader("Authorization", "Bearer " + oauthToken.toUtf8());
            req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
            payload.insert("system", kClaudeCodeOAuthSystem);
        } else {
            req.setRawHeader("x-api-key", apiKey.toUtf8());
        }
        req.setRawHeader("anthropic-version", "2023-06-01");
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    } else {
        QJsonObject payload;
        payload.insert("model", model);
        payload.insert("input", task);
        payload.insert("max_output_tokens", outTok);
        QNetworkRequest req = openAiRequest(
            QUrl(QStringLiteral("https://api.openai.com/v1/responses")), apiKey);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    m_prioritizeInFlight = true;
    if (m_issuePrioritizeButton) {
        m_issuePrioritizeButton->setEnabled(false);
        m_issuePrioritizeButton->setText(
            QString::fromUtf8("Prioritizing\xE2\x80\xA6"));
    }
    setIssueInlineNotice(
        QString::fromUtf8(claude ? "Asking Claude to prioritize\xE2\x80\xA6"
                                 : "Asking OpenAI to prioritize\xE2\x80\xA6"));

    connect(reply, &QNetworkReply::finished, this, [this, reply, claude] {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        m_prioritizeInFlight = false;
        if (m_issuePrioritizeButton) {
            m_issuePrioritizeButton->setEnabled(true);
            m_issuePrioritizeButton->setText("Prioritize from README");
        }
        if (reply->error() != QNetworkReply::NoError) {
            setIssueInlineNotice(
                "Prioritize request failed: " + apiErrorSummary(reply, body),
                true);
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
        } else {
            text = openAiResponseText(obj);
        }

        const int lb = text.indexOf('[');
        const int rb = text.lastIndexOf(']');
        const QJsonArray order =
            (lb >= 0 && rb > lb)
                ? QJsonDocument::fromJson(text.mid(lb, rb - lb + 1).toUtf8())
                      .array()
                : QJsonArray();
        if (order.isEmpty()) {
            setIssueInlineNotice(
                "Could not read a priority list from the agent's response.",
                true);
            return;
        }

        IssueStore writeStore = issueStoreForCurrentRepo();
        if (!writeStore.canWrite()) {
            setIssueInlineNotice(
                "This repo is read-only here; can't reprioritize.", true);
            return;
        }
        QSet<int> openNow;
        for (const Issue &issue : std::as_const(m_currentIssues))
            if (issue.status != QLatin1String("closed"))
                openNow.insert(issue.number);

        int prio = 1, applied = 0, failed = 0;
        QSet<int> seen;
        for (const QJsonValue &v : order) {
            const int number = v.toInt(-1);
            if (number < 0 || seen.contains(number) || !openNow.contains(number))
                continue;
            seen.insert(number);
            QString error;
            if (writeStore.setPriority(number, qMin(8999, prio), &error))
                ++applied;
            else
                ++failed;
            ++prio;
        }

        if (applied == 0) {
            setIssueInlineNotice("No open issues matched the agent's ranking.",
                                 true);
            return;
        }
        setIssueInlineNotice(
            failed == 0
                ? QStringLiteral("Prioritized %1 issue(s) from the README.")
                      .arg(applied)
                : QStringLiteral(
                      "Prioritized %1 issue(s) from the README (%2 failed).")
                      .arg(applied)
                      .arg(failed),
            failed != 0);
        reloadIssues();
    });
}

void MainWindow::analyzeIssueCompleteness()
{
    if (m_completenessInFlight || !m_networkAccess)
        return;

    if (!issueStoreForCurrentRepo().canWrite()) {
        setIssueInlineNotice("This repo is read-only here; can't update issues.",
                             true);
        return;
    }

    QList<Issue> open;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.status != QLatin1String("closed"))
            open.append(issue);
    if (open.isEmpty()) {
        setIssueInlineNotice("No open issues to analyze.");
        return;
    }

    QString readme = currentRepoReadme().trimmed();
    if (readme.size() > 8000)
        readme = readme.left(8000) + QStringLiteral("\n\n[README truncated]");

    QString fileTree;
    {
        const int repoIdx = issuesRepoIndex();
        if (repoIdx >= 0) {
            const RepositoryRecord repo =
                writableRecordFor(m_repositories.at(repoIdx));
            QByteArray out;
            if (!repo.localPath.trimmed().isEmpty() &&
                runGitCapture(repo.localPath, {"ls-files"}, &out, nullptr))
                fileTree = QString::fromUtf8(out).trimmed();
        }
    }
    if (fileTree.size() > 12000)
        fileTree = fileTree.left(12000) + QStringLiteral("\n[file list truncated]");

    const QString provider = m_issuePrioritizeAgentCombo
                                 ? m_issuePrioritizeAgentCombo->currentData()
                                       .toString()
                                 : defaultAgentProvider();
    const bool claude = agentIsClaudeProvider(provider);
    const bool claudeCode = provider == QLatin1String("claude-code");
    // "Claude Code" authenticates with the claude.ai subscription OAuth token,
    // never a metered API key (see prioritizeIssuesFromReadme for the rationale).
    const QString oauthToken = claudeCode ? claudeCodeOAuthToken() : QString();
    const QString apiKey =
        claudeCode ? QString()
                   : (claude ? QSettings().value(kClaudeApiKeySetting)
                             : QSettings().value(kCodexApiKeySetting))
                         .toString()
                         .trimmed();
    if (apiKey.isEmpty() && oauthToken.isEmpty()) {
        setIssueInlineNotice(
            claudeCode
                ? "Sign in to Claude Code first (run `claude` and log in)."
                : claude ? "Add a Claude API key in Settings first."
                         : "Add an OpenAI API key in Settings first.",
            true);
        return;
    }

    QStringList lines;
    for (const Issue &issue : std::as_const(open)) {
        QString line =
            QStringLiteral("#%1: %2").arg(issue.number).arg(issue.title.trimmed());
        QString body;
        for (const IssueEvent &ev : issue.events)
            if (ev.type == QLatin1String("open")) {
                body = ev.body.trimmed();
                break;
            }
        if (!body.isEmpty()) {
            body = body.simplified();
            if (body.size() > 400)
                body = body.left(400) + QString::fromUtf8("\xE2\x80\xA6");
            line += QString::fromUtf8(" \xE2\x80\x94 ") + body;
        }
        lines << line;
    }

    const QString readmeSection =
        readme.isEmpty()
            ? QStringLiteral("(no README found)")
            : readme;
    const QString filesSection =
        fileTree.isEmpty() ? QStringLiteral("(file listing unavailable)") : fileTree;
    const QString task =
        QStringLiteral("%1\n\n----- README -----\n%2\n\n----- REPOSITORY FILES "
                       "-----\n%3\n\n----- OPEN ISSUES -----\n%4")
            .arg(completenessPrompt(), readmeSection, filesSection,
                 lines.join('\n'));
    const QString model =
        claude ? QStringLiteral("claude-opus-5") : kIssueAskAiModel;
    const int outTok = qBound(1024, open.size() * 220 + 512, 8000);

    QNetworkReply *reply = nullptr;
    if (claude) {
        QJsonObject payload;
        payload.insert("model", model);
        payload.insert("max_tokens", outTok);
        QJsonArray messages;
        QJsonObject um;
        um.insert("role", "user");
        um.insert("content", task);
        messages.append(um);
        payload.insert("messages", messages);
        QNetworkRequest req(
            QUrl(QStringLiteral("https://api.anthropic.com/v1/messages")));
        if (!oauthToken.isEmpty()) {
            req.setRawHeader("Authorization", "Bearer " + oauthToken.toUtf8());
            req.setRawHeader("anthropic-beta", "oauth-2025-04-20");
            payload.insert("system", kClaudeCodeOAuthSystem);
        } else {
            req.setRawHeader("x-api-key", apiKey.toUtf8());
        }
        req.setRawHeader("anthropic-version", "2023-06-01");
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    } else {
        QJsonObject payload;
        payload.insert("model", model);
        payload.insert("input", task);
        payload.insert("max_output_tokens", outTok);
        QNetworkRequest req = openAiRequest(
            QUrl(QStringLiteral("https://api.openai.com/v1/responses")), apiKey);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        reply = m_networkAccess->post(
            req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    }

    m_completenessInFlight = true;
    if (m_issueCompletenessButton) {
        m_issueCompletenessButton->setEnabled(false);
        m_issueCompletenessButton->setText(
            QString::fromUtf8("Analyzing\xE2\x80\xA6"));
    }
    setIssueInlineNotice(QString::fromUtf8(
        claude ? "Asking Claude to analyze completeness\xE2\x80\xA6"
               : "Asking OpenAI to analyze completeness\xE2\x80\xA6"));

    connect(reply, &QNetworkReply::finished, this, [this, reply, claude] {
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        m_completenessInFlight = false;
        if (m_issueCompletenessButton) {
            m_issueCompletenessButton->setEnabled(true);
            m_issueCompletenessButton->setText("Analyze completeness");
        }
        if (reply->error() != QNetworkReply::NoError) {
            setIssueInlineNotice(
                "Completeness request failed: " + apiErrorSummary(reply, body),
                true);
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
        } else {
            text = openAiResponseText(obj);
        }
        text = text.trimmed();
        if (text.isEmpty()) {
            setIssueInlineNotice(
                "The agent returned an empty completeness report.", true);
            return;
        }

        const int lb = text.indexOf('[');
        const int rb = text.lastIndexOf(']');
        const QJsonArray verdicts =
            (lb >= 0 && rb > lb)
                ? QJsonDocument::fromJson(text.mid(lb, rb - lb + 1).toUtf8())
                      .array()
                : QJsonArray();
        if (verdicts.isEmpty()) {
            setIssueInlineNotice(
                "Could not read completeness verdicts from the agent's response.",
                true);
            return;
        }

        IssueStore writeStore = issueStoreForCurrentRepo();
        if (!writeStore.canWrite()) {
            setIssueInlineNotice(
                "This repo is read-only here; can't update issues.", true);
            return;
        }

        QHash<int, Issue> openByNumber;
        for (const Issue &issue : std::as_const(m_currentIssues))
            if (issue.status != QLatin1String("closed"))
                openByNumber.insert(issue.number, issue);

        static const QStringList kCompletenessLabels = {
            QStringLiteral("Complete"), QStringLiteral("Partial"),
            QStringLiteral("Incomplete")};

        const QString dash = QString::fromUtf8(" \xE2\x80\x94 ");
        int applied = 0, failed = 0;
        QStringList reportLines;
        for (const QJsonValue &v : verdicts) {
            const QJsonObject o = v.toObject();
            const int number = o.value("number").toInt(-1);
            if (!openByNumber.contains(number))
                continue;
            const QString rating = o.value("rating").toString().toLower();
            QString label;
            if (rating.contains(QLatin1String("incomplete")))
                label = QStringLiteral("Incomplete");
            else if (rating.contains(QLatin1String("complete")))
                label = QStringLiteral("Complete");
            else if (rating.contains(QLatin1String("partial")))
                label = QStringLiteral("Partial");
            const int pct = qBound(0, o.value("completeness").toInt(), 100);
            QString comment = o.value("comment").toString().trimmed();
            if (comment.isEmpty())
                comment = o.value("reason").toString().trimmed();

            QStringList labels = openByNumber.value(number).labels;
            for (const QString &cl : kCompletenessLabels)
                labels.removeAll(cl);
            if (!label.isEmpty())
                labels << label;

            QString error;
            const bool okLabels =
                label.isEmpty() ? true
                                : writeStore.setLabels(number, labels, &error);
            const bool okProgress = writeStore.setProgress(number, pct, &error);
            bool okComment = true;
            if (!comment.isEmpty()) {
                const QString commentBody =
                    QStringLiteral("**Completeness analysis:** %1 (%2%)\n\n%3")
                        .arg(label.isEmpty() ? QStringLiteral("?") : label)
                        .arg(pct)
                        .arg(comment);
                okComment = writeStore.addComment(number, commentBody, {}, &error);
            }
            if (okLabels && okProgress && okComment)
                ++applied;
            else
                ++failed;

            reportLines
                << QStringLiteral("- #%1 **%2**")
                           .arg(number)
                           .arg(label.isEmpty() ? QStringLiteral("?") : label) +
                       dash + QStringLiteral("%1%").arg(pct) + dash +
                       (comment.isEmpty() ? QStringLiteral("(no detail)")
                                          : comment);
        }

        if (applied == 0) {
            setIssueInlineNotice("No open issues matched the agent's verdicts.",
                                 true);
            return;
        }

        reloadIssues();
        setIssueInlineNotice(
            failed == 0
                ? QStringLiteral(
                      "Updated %1 issue(s) from the completeness analysis.")
                      .arg(applied)
                : QStringLiteral("Updated %1 issue(s) (%2 failed) from the "
                                 "completeness analysis.")
                      .arg(applied)
                      .arg(failed),
            failed != 0);

        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Issue completeness"));
        dialog.resize(560, 480);
        auto *layout = new QVBoxLayout(&dialog);
        auto *view = new QTextBrowser(&dialog);
        view->setOpenExternalLinks(true);
        view->setMarkdown(reportLines.join('\n'));
        layout->addWidget(view);
        auto *buttons =
            new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        connect(buttons, &QDialogButtonBox::rejected, &dialog,
                &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog,
                &QDialog::accept);
        layout->addWidget(buttons);
        dialog.exec();
    });
}

namespace {
// One person the issue sidebar can name. An issue belongs to people, not to
// the machines they serve from, so the candidates are ForkMesh accounts (you,
// everyone already on or talking in this issue, then the user directory),
// never the serving nodes those accounts own.
struct IssuePerson {
    QString name;
    QString detail; // "you", "reported this issue", ...
    bool self = false;
};

QList<IssuePerson> issuePeopleFor(const QString &userName,
                                  const QStringList &picked,
                                  const QString &pickedDetail,
                                  const Issue *issue,
                                  const QHash<QString, MemberInfo> &directory)
{
    QList<IssuePerson> people;
    QSet<QString> seen;
    auto add = [&](const QString &rawName, const QString &detail, bool self) {
        const QString name = rawName.trimmed();
        if (name.isEmpty() || seen.contains(name.toLower()))
            return;
        seen.insert(name.toLower());
        people.append({name, detail, self});
    };

    add(userName, QStringLiteral("you"), true);
    for (const QString &name : picked)
        add(name, pickedDetail, false);
    if (issue) {
        add(issue->authorName, QStringLiteral("reported this issue"), false);
        for (const QString &name : issue->assignees)
            add(name, QStringLiteral("assigned"), false);
        for (const IssueEvent &ev : issue->events)
            if (ev.type == QLatin1String("comment"))
                add(ev.authorName, QStringLiteral("commented here"), false);
    }
    QStringList names;
    names.reserve(directory.size());
    for (const MemberInfo &user : directory)
        names << user.name;
    names.sort(Qt::CaseInsensitive);
    for (const QString &name : std::as_const(names))
        add(name, QString(), false);
    return people;
}

// The issue a sidebar picker is editing, for the candidate list above.
const Issue *issueByNumber(const QList<Issue> &issues, int number)
{
    for (const Issue &issue : issues)
        if (issue.number == number)
            return &issue;
    return nullptr;
}
} // namespace

// The names the inline assignee field suggests; see where its completer is made.
void MainWindow::refreshIssueAssigneeSuggestions()
{
    if (!m_issueAssigneesSuggestionModel)
        return;
    QStringList names;
    for (const IssuePerson &person :
         issuePeopleFor(m_userName, QStringList(), QString(),
                        issueByNumber(m_currentIssues, m_currentIssueNumber),
                        m_chatDirectoryUsers))
        names << person.name;
    if (m_issueAssigneesSuggestionModel->stringList() != names)
        m_issueAssigneesSuggestionModel->setStringList(names);
}

void MainWindow::pickIssueAssignees()
{
    if (m_currentIssueNumber < 0 || !m_issueAssigneesButton)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (!issueStoreForCurrentRepo().canWrite()) {
        setIssueInlineNotice(kIssueAssigneesHostOnlyNotice, true,
                             forkmesh::kLocalOperationNoticeKind);
        return;
    }

    const QStringList current = m_issueAssigneesEdit
                                    ? splitIssueFieldList(m_issueAssigneesEdit->text())
                                    : QStringList();
    QSet<QString> selected(current.begin(), current.end());

    const QList<IssuePerson> candidates = issuePeopleFor(
        m_userName, current, QStringLiteral("assigned"),
        issueByNumber(m_currentIssues, m_currentIssueNumber),
        m_chatDirectoryUsers);

    QMenu menu(this);
    QAction *header = menu.addAction(QStringLiteral("Assign users"));
    header->setEnabled(false);

    auto *searchEdit = new QLineEdit(&menu);
    searchEdit->setPlaceholderText(QStringLiteral("Search users") +
                                   QString::fromUtf8("\xE2\x80\xA6"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(240);
    auto *searchAction = new QWidgetAction(&menu);
    searchAction->setDefaultWidget(searchEdit);
    menu.addAction(searchAction);
    menu.addSeparator();

    if (candidates.isEmpty()) {
        QAction *empty = menu.addAction(QStringLiteral("No users yet"));
        empty->setEnabled(false);
    }

    auto *listWidget = new QListWidget(&menu);
    listWidget->setObjectName("assigneePickerList");
    listWidget->setMinimumWidth(240);
    listWidget->setMaximumHeight(320);
    listWidget->setFrameShape(QFrame::NoFrame);
    for (const IssuePerson &c : candidates) {
        const QString detail = c.self ? QStringLiteral("you") : c.detail;
        auto *item = new QListWidgetItem(
            issueAssigneeAvatar(c.name),
            detail.isEmpty() ? c.name
                             : QStringLiteral("%1 (%2)").arg(c.name, detail),
            listWidget);
        item->setData(Qt::UserRole, c.name);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(selected.contains(c.name) ? Qt::Checked : Qt::Unchecked);
    }
    connect(listWidget, &QListWidget::itemChanged, &menu,
            [&selected](QListWidgetItem *item) {
                const QString name = item->data(Qt::UserRole).toString();
                if (item->checkState() == Qt::Checked)
                    selected.insert(name);
                else
                    selected.remove(name);
            });
    auto *listAction = new QWidgetAction(&menu);
    listAction->setDefaultWidget(listWidget);
    menu.addAction(listAction);

    connect(searchEdit, &QLineEdit::textChanged, listWidget,
            [listWidget](const QString &text) {
                const QString needle = text.trimmed().toLower();
                for (int i = 0; i < listWidget->count(); ++i) {
                    QListWidgetItem *it = listWidget->item(i);
                    const QString name = it->data(Qt::UserRole).toString().toLower();
                    it->setHidden(!needle.isEmpty() && !name.contains(needle));
                }
            });
    QTimer::singleShot(0, searchEdit, [searchEdit] { searchEdit->setFocus(); });

    menu.exec(m_issueAssigneesButton->mapToGlobal(
        QPoint(0, m_issueAssigneesButton->height())));

    const QSet<QString> before(current.begin(), current.end());
    if (selected == before || !m_issueAssigneesEdit)
        return;
    QStringList result;
    for (const QString &a : current)
        if (selected.contains(a))
            result << a;
    for (const IssuePerson &c : candidates)
        if (selected.contains(c.name) && !result.contains(c.name))
            result << c.name;
    m_issueAssigneesEdit->setText(result.join(", "));
    saveIssueAssigneesInline();
}

// A report filed through ForkBot intake, the Discord bug channel or an agent is
// signed by this node but credited to the person who reported it, and that
// credit is a guess often enough to need a way to correct it. Picking a name
// appends a signed "reporter" event; the opening event's signature and its
// signing key are untouched.
void MainWindow::pickIssueReporter()
{
    if (m_currentIssueNumber < 0 || !m_issueReporterButton)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (!issueStoreForCurrentRepo().canWrite()) {
        setIssueInlineNotice(kIssueReporterHostOnlyNotice, true,
                             forkmesh::kLocalOperationNoticeKind);
        return;
    }

    const QString current = m_issueReporterEdit
                                ? m_issueReporterEdit->text().trimmed()
                                : m_issueReporterShown;
    const QList<IssuePerson> candidates = issuePeopleFor(
        m_userName, QStringList{current}, QStringLiteral("reported this issue"),
        issueByNumber(m_currentIssues, m_currentIssueNumber),
        m_chatDirectoryUsers);

    QMenu menu(this);
    QAction *header = menu.addAction(QStringLiteral("Credit the report to"));
    header->setEnabled(false);

    auto *searchEdit = new QLineEdit(&menu);
    searchEdit->setPlaceholderText(QStringLiteral("Search users") +
                                   QString::fromUtf8("\xE2\x80\xA6"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(240);
    auto *searchAction = new QWidgetAction(&menu);
    searchAction->setDefaultWidget(searchEdit);
    menu.addAction(searchAction);
    menu.addSeparator();

    if (candidates.isEmpty()) {
        QAction *empty = menu.addAction(QStringLiteral("No users yet"));
        empty->setEnabled(false);
    }

    auto *listWidget = new QListWidget(&menu);
    listWidget->setObjectName("reporterPickerList");
    listWidget->setMinimumWidth(240);
    listWidget->setMaximumHeight(320);
    listWidget->setFrameShape(QFrame::NoFrame);
    for (const IssuePerson &c : candidates) {
        const QString detail = c.self ? QStringLiteral("you") : c.detail;
        auto *item = new QListWidgetItem(
            issueAssigneeAvatar(c.name),
            detail.isEmpty() ? c.name
                             : QStringLiteral("%1 (%2)").arg(c.name, detail),
            listWidget);
        item->setData(Qt::UserRole, c.name);
        if (c.name.compare(current, Qt::CaseInsensitive) == 0)
            listWidget->setCurrentItem(item);
    }
    // One reporter, so a click is the whole choice: take it and close.
    QString chosen;
    connect(listWidget, &QListWidget::itemClicked, &menu,
            [&chosen, &menu](QListWidgetItem *item) {
                chosen = item->data(Qt::UserRole).toString();
                menu.close();
            });
    auto *listAction = new QWidgetAction(&menu);
    listAction->setDefaultWidget(listWidget);
    menu.addAction(listAction);

    connect(searchEdit, &QLineEdit::textChanged, listWidget,
            [listWidget](const QString &text) {
                const QString needle = text.trimmed().toLower();
                for (int i = 0; i < listWidget->count(); ++i) {
                    QListWidgetItem *it = listWidget->item(i);
                    const QString name = it->data(Qt::UserRole).toString().toLower();
                    it->setHidden(!needle.isEmpty() && !name.contains(needle));
                }
            });
    QTimer::singleShot(0, searchEdit, [searchEdit] { searchEdit->setFocus(); });

    menu.exec(m_issueReporterButton->mapToGlobal(
        QPoint(0, m_issueReporterButton->height())));

    if (chosen.isEmpty() || chosen == current || !m_issueReporterEdit)
        return;
    m_issueReporterEdit->setText(chosen);
    saveIssueReporterInline();
}

// Same deal as the Assignees value below: the name doubles as a way into the
// editor where the save can land, and says who can change it where it cannot.
void MainWindow::renderIssueReporterValue(QString reporter)
{
    m_issueReporterShown = reporter.trimmed();
    if (!m_issueReporterValue)
        return;
    const bool writable = issueStoreForCurrentRepo().canWrite();
    const bool hostOnly = !writable && issuesRepoIndex() >= 0;
    const Issue *issue = issueByNumber(m_currentIssues, m_currentIssueNumber);
    const QString authorKey = issue ? issue->author : QString();
    const QString key = QStringLiteral("%1%2|%3|%4")
                            .arg(writable ? 1 : 0)
                            .arg(hostOnly ? 1 : 0)
                            .arg(m_issueReporterShown, authorKey);
    if (key == m_issueReporterRenderKey)
        return;
    m_issueReporterRenderKey = key;

    QStringList lines;
    if (m_issueReporterShown.isEmpty()) {
        lines << (writable ? QStringLiteral("Unknown - <a href='#'>Credit yourself</a>")
                           : QStringLiteral("Unknown"));
    } else {
        // The same face the opening card wears, so re-crediting a report moves
        // the picture with the name.
        const QPixmap avatar =
            issueTimelineAvatar(authorKey, m_issueReporterShown, 18);
        lines << QStringLiteral("%1&nbsp;%2")
                     .arg(inlinePixmapMarkup(avatar, 18),
                          userProfileLinkHtml(m_issueReporterShown,
                                              m_issueReporterShown.left(24)));
    }
    if (hostOnly)
        lines << QStringLiteral("<i>%1</i>").arg(kIssueReporterHostOnlyNotice);
    m_issueReporterValue->setText(lines.join("<br>"));
    m_issueReporterValue->setToolTip(writable ? m_issueReporterShown
                                              : kIssueReporterHostOnlyNotice);
}

void MainWindow::editIssueReporter()
{
    if (m_currentIssueNumber < 0)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (!issueStoreForCurrentRepo().canWrite()) {
        setIssueInlineNotice(kIssueReporterHostOnlyNotice, true,
                             forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (m_issueReporterStack)
        m_issueReporterStack->setCurrentIndex(1);
    if (m_issueReporterEdit) {
        m_issueReporterEdit->setText(m_issueReporterShown);
        m_issueReporterEdit->setFocus();
        m_issueReporterEdit->selectAll();
    }
}

void MainWindow::saveIssueReporterInline()
{
    if (m_currentIssueNumber < 0 || !m_issueReporterEdit)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        // The control is disabled on a mirror, so this catches only a checkout
        // that went away while the editor was open.
        m_issueReporterEdit->setText(m_issueReporterShown);
        cancelIssueSidebarEditors();
        setIssueInlineNotice(kIssueReporterHostOnlyNotice, true,
                             forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QString reporter = m_issueReporterEdit->text().trimmed();
    if (reporter == m_issueReporterShown) {
        cancelIssueSidebarEditors();
        return;
    }
    QString error;
    if (!store.setReporter(m_currentIssueNumber, reporter, &error)) {
        setIssueInlineNotice(
            error.isEmpty() ? "Could not update the reporter." : error, true);
        return;
    }
    setIssueInlineNotice(QStringLiteral("Issue #%1 credited to %2.")
                             .arg(m_currentIssueNumber)
                             .arg(reporter));
    reloadIssues();
    // The open issue's row is still selected, so the reload's selectRow() fires
    // no selection change; re-render or the sidebar keeps the old reporter.
    if (const Issue *fresh = issueByNumber(m_currentIssues, m_currentIssueNumber)) {
        renderIssueThread(*fresh);
        updateIssueActionState();
    }
}

// The sidebar's Assignees value doubles as a way into the editor, so it only
// offers "Assign yourself" where the save can land; elsewhere it names the
// assignees and says who can change them.
void MainWindow::renderIssueAssigneesValue(QStringList assignees)
{
    m_issueAssigneesShown = std::move(assignees);
    if (!m_issueAssigneesValue)
        return;
    const bool writable = issueStoreForCurrentRepo().canWrite();
    const bool hostOnly = !writable && issuesRepoIndex() >= 0;
    // Every avatar here is a paint plus a PNG round trip and
    // updateIssueActionState() re-renders after each issue action, so redo the
    // markup only when one of the three things it reads has moved.
    const QString key = QStringLiteral("%1%2|%3")
                            .arg(writable ? 1 : 0)
                            .arg(hostOnly ? 1 : 0)
                            .arg(m_issueAssigneesShown.join(QLatin1Char(',')));
    if (key == m_issueAssigneesRenderKey)
        return;
    m_issueAssigneesRenderKey = key;

    QStringList lines;
    for (const QString &a : std::as_const(m_issueAssigneesShown)) {
        // Same resolver the reporter line and the timeline cards use: the real
        // picture when the directory has one, the seeded portrait otherwise.
        // Reading the seed directly pinned this line to the stand-in forever.
        const QPixmap avatar = issueTimelineAvatar(QString(), a, 18);
        lines << QStringLiteral("%1&nbsp;%2")
                     .arg(inlinePixmapMarkup(avatar, 18),
                          userProfileLinkHtml(a, a.left(24)));
    }
    if (lines.isEmpty())
        lines << (writable ? QStringLiteral("No one - <a href='#'>Assign yourself</a>")
                           : QStringLiteral("No one"));
    // Italic, not a colour: #statusLine is already the muted grey, and it is a
    // different grey in each theme.
    if (hostOnly)
        lines << QStringLiteral("<i>%1</i>").arg(kIssueAssigneesHostOnlyNotice);
    m_issueAssigneesValue->setText(lines.join("<br>"));
    m_issueAssigneesValue->setToolTip(writable ? QString()
                                               : kIssueAssigneesHostOnlyNotice);
}

void MainWindow::editIssueAssignees()
{
    if (m_currentIssueNumber < 0)
        return;
    m_issueDeleteConfirmPending = false;
    setIssueInlineNotice(QString());
    if (!issueStoreForCurrentRepo().canWrite()) {
        setIssueInlineNotice(kIssueAssigneesHostOnlyNotice, true,
                             forkmesh::kLocalOperationNoticeKind);
        return;
    }
    refreshIssueAssigneeSuggestions();
    refreshChatUserDirectory(); // throttled; a newer roster refills the list
    if (m_issueAssigneesStack)
        m_issueAssigneesStack->setCurrentIndex(1);
    if (m_issueAssigneesEdit) {
        m_issueAssigneesEdit->setFocus();
        m_issueAssigneesEdit->selectAll();
    }
}

void MainWindow::saveIssueAssigneesInline()
{
    if (m_currentIssueNumber < 0 || !m_issueAssigneesEdit)
        return;
    IssueStore store = issueStoreForCurrentRepo();
    if (!store.canWrite()) {
        // The control is disabled on a mirror, so this catches only a checkout
        // that went away while the editor was open. Same words either way, and
        // the unsaved selection goes back to what the issue actually says.
        m_issueAssigneesEdit->setText(m_issueAssigneesShown.join(", "));
        cancelIssueSidebarEditors();
        setIssueInlineNotice(kIssueAssigneesHostOnlyNotice, true,
                             forkmesh::kLocalOperationNoticeKind);
        return;
    }
    QString error;
    const QStringList assignees = splitIssueFieldList(m_issueAssigneesEdit->text());
    if (!store.setAssignees(m_currentIssueNumber, assignees, &error)) {
        // A refused write on a host that can write is a defect worth reporting.
        setIssueInlineNotice(
            error.isEmpty() ? "Could not update assignees." : error, true);
        return;
    }
    setIssueInlineNotice("Assignees updated.");
    if (!assignees.isEmpty() && issuesRepoIndex() == m_repoDetailIndex)
        triggerTeammatesForEvent(
            QStringLiteral("assignments"),
            QStringLiteral("Issue #%1 assigned to %2")
                .arg(m_currentIssueNumber)
                .arg(assignees.join(QStringLiteral(", "))));
    reloadIssues();
    // The open issue's row is still selected, so the reload's selectRow() fires
    // no selection change; re-render or the sidebar keeps the old assignees.
    if (const Issue *fresh = issueByNumber(m_currentIssues, m_currentIssueNumber)) {
        renderIssueThread(*fresh);
        updateIssueActionState();
    }
}

void MainWindow::cancelIssueSidebarEditors()
{
    if (m_issueAssigneesStack)
        m_issueAssigneesStack->setCurrentIndex(0);
    if (m_issueReporterStack)
        m_issueReporterStack->setCurrentIndex(0);
    if (m_issueLabelsStack)
        m_issueLabelsStack->setCurrentIndex(0);
    if (m_issueMilestoneStack)
        m_issueMilestoneStack->setCurrentIndex(0);
    if (m_issueDatesStack)
        m_issueDatesStack->setCurrentIndex(0);
    if (m_issuePriorityStack)
        m_issuePriorityStack->setCurrentIndex(0);
}

QUrl MainWindow::issuesApiUrl(const RepositoryRecord &repo) const
{
    QUrl url = catalogApiUrl();
    url.setPath("/api/repo/" + repoSegment(repo.owner, QStringLiteral("owner")) +
                "/" + repoSegment(repo.name, QStringLiteral("repository")) +
                "/issues");
    return url;
}

QUrl MainWindow::sharesApiUrl(const RepositoryRecord &repo) const
{
    QUrl url = catalogApiUrl();
    url.setPath("/api/repo/" + repoSegment(repo.owner, QStringLiteral("owner")) +
                "/" + repoSegment(repo.name, QStringLiteral("repository")) +
                "/shares");
    return url;
}

void MainWindow::shareRepoRequest(const RepositoryRecord &repo,
                                  const QString &grantee, const QString &action)
{
    // Grant ("add") or revoke ("remove") a collaborator on a private repo
    // Owner-signed: only the repo owner may change the ACL. The
    // action is bound into the signature so an add token can't be replayed as a
    // remove and vice versa (matches the relay's forkmesh-share-v1 canonical).
    if (!PrivateMirrorRuntime::privateRepositoryCloudAccessAllowed(
            repo.isPrivate, repo.publishToNetwork) ||
        !m_networkAccess || !m_profileIdentity.isValid() ||
        !hasOwnerSigningCapability(repo.owner))
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-share-v1\n" + repo.owner + "\n" + repo.name + "\n" + grantee +
         "\n" + action + "\n" + ts).toUtf8();
    const QJsonObject payload{{"action", action},
                              {"grantee", grantee},
                              {"ts", ts},
                              {"sig", m_profileIdentity.signData(canonical)}};
    QNetworkRequest request(sharesApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, repo, grantee, action] {
                const QByteArray body = reply->readAll();
                const auto err = reply->error();
                const QString errStr = reply->errorString();
                reply->deleteLater();
                const QJsonObject obj = QJsonDocument::fromJson(body).object();
                if (err != QNetworkReply::NoError || !obj.value("ok").toBool()) {
                    flashMessage(
                        QStringLiteral("Could not update collaborators: %1")
                            .arg(obj.value("error").toString(errStr)),
                        true);
                    return;
                }
                // The ACL mutation is not the encryption transition. Fetch the
                // relay's exact public-only recipient set and rotate to it
                // before reporting success. A missing/malformed grantee key
                // fails closed: no new catalog epoch or route is published.
                requestPrivateRecipientBundles(
                    repo, /*updateCollaboratorList=*/true,
                    [this, repo, grantee, action](
                        bool recipientsReady,
                        QList<QJsonObject> recipientBundles,
                        QStringList, QString recipientError) {
                        if (!recipientsReady) {
                            flashMessage(
                                QStringLiteral(
                                    "The access list changed, but the encrypted "
                                    "replica was not rotated: %1")
                                    .arg(recipientError),
                                true);
                            return;
                        }
                        resealPrivateRepositoryRecipients(
                            repo, recipientBundles,
                            [this, grantee, action](bool rotated,
                                                   QString rotateError) {
                                if (!rotated) {
                                    flashMessage(
                                        QStringLiteral(
                                            "The access list changed, but the "
                                            "encrypted replica was not "
                                            "republished: %1")
                                            .arg(rotateError),
                                        true);
                                    return;
                                }
                                const QString verb =
                                    action == QLatin1String("add")
                                        ? QStringLiteral("Added")
                                        : QStringLiteral("Removed");
                                logSystem(
                                    QStringLiteral("%1 collaborator %2 and "
                                                   "rotated the encrypted "
                                                   "replica.")
                                        .arg(verb, grantee));
                                flashMessage(
                                    QStringLiteral(
                                        "%1 collaborator %2; private mirror "
                                        "keys rotated.")
                                        .arg(verb, grantee));
                            });
                    });
            });
}

void MainWindow::requestPrivateRecipientBundles(
    const RepositoryRecord &repo, bool updateCollaboratorList,
    std::function<void(bool, QList<QJsonObject>, QStringList, QString)> onDone)
{
    auto finish =
        [onDone = std::move(onDone)](
            bool ok, QList<QJsonObject> bundles = {},
            QStringList grantees = {}, const QString &error = QString()) mutable {
            if (onDone)
                onDone(ok, std::move(bundles), std::move(grantees), error);
        };
    if (!PrivateMirrorRuntime::privateRepositoryCloudAccessAllowed(
            repo.isPrivate, repo.publishToNetwork) ||
        !m_networkAccess ||
        !m_profileIdentity.isValid() ||
        !hasOwnerSigningCapability(repo.owner)) {
        finish(false, {}, {},
               QStringLiteral("the owner recipient list is unavailable"));
        return;
    }

    const QString ts =
        QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-shares-list-v1\n" + repo.owner + "\n" + repo.name +
         "\n" + ts)
            .toUtf8();
    QUrl url = sharesApiUrl(repo);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ts"), ts);
    query.addQueryItem(QStringLiteral("sig"),
                       m_profileIdentity.signData(canonical));
    url.setQuery(query);
    QNetworkReply *reply =
        m_networkAccess->get(QNetworkRequest(url));
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, repo, updateCollaboratorList,
         finish = std::move(finish)]() mutable {
            const QByteArray body = reply->readAll();
            const int status =
                reply->attribute(
                         QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            const auto networkError = reply->error();
            reply->deleteLater();
            const QJsonObject object =
                QJsonDocument::fromJson(body).object();
            const QJsonArray granteeValues =
                object.value(QStringLiteral("grantees")).toArray();
            QStringList grantees;
            QSet<QString> granteeSet;
            bool granteesValid = true;
            for (const QJsonValue &value : granteeValues) {
                const QString grantee =
                    value.toString().trimmed().toLower();
                if (grantee.isEmpty() ||
                    grantee.contains(QLatin1Char('/')) ||
                    granteeSet.contains(grantee)) {
                    granteesValid = false;
                    break;
                }
                granteeSet.insert(grantee);
                grantees.append(grantee);
            }

            const bool showingSameRepository =
                m_repoDetailIndex >= 0 &&
                m_repoDetailIndex < m_repositories.size() &&
                m_repositories.at(m_repoDetailIndex).owner == repo.owner &&
                m_repositories.at(m_repoDetailIndex).name == repo.name;
            if (updateCollaboratorList && showingSameRepository &&
                m_collabList) {
                m_collabList->clear();
                for (const QString &grantee : std::as_const(grantees))
                    m_collabList->addItem(grantee);
                if (m_collabEmptyHint)
                    m_collabEmptyHint->setVisible(grantees.isEmpty());
            }

            const QJsonArray recipientValues =
                object.value(QStringLiteral("recipients")).toArray();
            QList<QJsonObject> bundles;
            QSet<QString> recipientGrantees;
            QSet<QString> keyIds;
            bool recipientsValid =
                recipientValues.size() == grantees.size();
            for (const QJsonValue &value : recipientValues) {
                const QJsonObject recipient = value.toObject();
                const QString grantee =
                    recipient.value(QStringLiteral("grantee"))
                        .toString()
                        .trimmed()
                        .toLower();
                const QString keyId =
                    recipient.value(QStringLiteral("keyId"))
                        .toString();
                const QJsonObject publicBundle =
                    recipient.value(QStringLiteral("publicBundle"))
                        .toObject();
                const QString computedKeyId =
                    MirrorCrypto::publicKeyId(publicBundle);
                if (grantee.isEmpty() ||
                    !granteeSet.contains(grantee) ||
                    recipientGrantees.contains(grantee) ||
                    keyId.isEmpty() || keyIds.contains(keyId) ||
                    computedKeyId != keyId) {
                    recipientsValid = false;
                    break;
                }
                recipientGrantees.insert(grantee);
                keyIds.insert(keyId);
                bundles.append(publicBundle);
            }
            const bool ready =
                networkError == QNetworkReply::NoError &&
                status >= 200 && status < 300 &&
                object.value(QStringLiteral("ok")).toBool() &&
                !object.value(QStringLiteral("privateKeysStored"))
                     .toBool(true) &&
                object.value(QStringLiteral("encryptionReady"))
                    .toBool(false) &&
                object.value(QStringLiteral("missingEncryptionKeys"))
                    .toArray()
                    .isEmpty() &&
                granteesValid && recipientsValid &&
                recipientGrantees == granteeSet;
            if (!ready) {
                finish(
                    false, {}, grantees,
                    object
                            .value(QStringLiteral(
                                "missingEncryptionKeys"))
                            .toArray()
                            .isEmpty()
                        ? QStringLiteral(
                              "the public-only recipient keys could not be "
                              "verified")
                        : QStringLiteral(
                              "a collaborator must sign in with the desktop "
                              "client once to register their encryption key"));
                return;
            }
            finish(true, bundles, grantees, QString());
        });
}

void MainWindow::addRepoCollaborator(const QString &nameRaw)
{
    const QString grantee = nameRaw.trimmed().toLower();
    if (grantee.isEmpty())
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    if (grantee == repo.owner) {
        flashMessage(QStringLiteral("A repository is already readable by its "
                                    "owner."),
                     true);
        return;
    }
    shareRepoRequest(repo, grantee, QStringLiteral("add"));
    if (m_collabEdit)
        m_collabEdit->clear();
}

void MainWindow::removeRepoCollaborator(const QString &nameRaw)
{
    const QString grantee = nameRaw.trimmed().toLower();
    if (grantee.isEmpty())
        return;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    shareRepoRequest(m_repositories.at(m_repoDetailIndex), grantee,
                     QStringLiteral("remove"));
}

void MainWindow::refreshRepoCollaborators()
{
    if (!m_collabSection)
        return;
    const bool haveRepo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size();
    const RepositoryRecord blank;
    const RepositoryRecord &repo =
        haveRepo ? m_repositories.at(m_repoDetailIndex) : blank;
    const bool show = haveRepo && !accountOwner().isEmpty() &&
                      repo.owner == accountOwner() && repo.isPrivate &&
                      repo.publishToNetwork &&
                      hasOwnerSigningCapability(repo.owner);
    m_collabSection->setVisible(show);
    if (m_collabList)
        m_collabList->clear();
    if (!show || !m_networkAccess || !m_profileIdentity.isValid())
        return;
    // refreshRepoSettings() runs on every repo-detail open and on unrelated
    // settings edits, and the fetch below names a private repository to the
    // relay. Only the Settings page displays this list, and opening that tab
    // calls refreshRepoSettings() again, so defer until it is the page on
    // screen instead of announcing the repository just to fill a hidden panel.
    if (!m_repoDetailStack || m_settingsTabIndex < 0 ||
        m_repoDetailStack->currentIndex() != m_settingsTabIndex) {
        if (m_collabEmptyHint) {
            m_collabEmptyHint->setText(QStringLiteral("No collaborators yet."));
            m_collabEmptyHint->setVisible(true);
        }
        return;
    }
    requestPrivateRecipientBundles(
        repo, /*updateCollaboratorList=*/true,
        [this](bool ready, QList<QJsonObject>, QStringList grantees,
               QString) {
            if (!m_collabEmptyHint)
                return;
            m_collabEmptyHint->setVisible(grantees.isEmpty());
            if (!ready && !grantees.isEmpty())
                m_collabEmptyHint->setText(
                    QStringLiteral(
                        "A collaborator encryption key is not ready; private "
                        "mirror publication remains blocked."));
            else
                m_collabEmptyHint->setText(
                    QStringLiteral("No collaborators yet."));
        });
}

void MainWindow::showBountyQrDialog(const RepositoryRecord &repo, int number,
                                    const QString &uri, const QString &address,
                                    double amountUsd, const QString &amountSol,
                                    const QString &kind, const QString &payee)
{
    QMessageBox::information(
        this, QStringLiteral("Legacy bounty funding disabled"),
        QStringLiteral(
            "No funding request was created. ForkMesh has retired Worker-held "
            "bounty escrow; historical bounty addresses are migration-only and "
            "must not receive new funds. A future reward must use explicit "
            "approval in an external self-custodial wallet or a separately "
            "reviewed program/multisig flow."));
    return;

#if 0 // Historical Worker-held escrow QR/payout implementation; never compiled.
    const bool isPr = kind == QLatin1String("pr");
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Fund bounty"));
    const bool dark = currentThemeIsDark();
    const QString pageBg = dark ? "#0d1117" : "#ffffff";
    const QString cardBg = dark ? "#161b22" : "#f6f8fa";
    const QString border = dark ? "#30363d" : "#d0d7de";
    const QString fg = dark ? "#e6edf3" : "#1f2328";
    const QString muted = dark ? "#8b949e" : "#57606a";
    const QString accent =
        Theme::nextOrHex(Theme::Role::AccentFg, dark ? "#58a6ff" : "#0969da");
    dialog.setStyleSheet(Theme::remapSheet(
        QStringLiteral(
            "QDialog { background:%1; }"
            "QFrame#bountyCard { background:%2; border:1px solid %3;"
            "  border-radius:10px; }"
            "QFrame#bountyQrCard { background:#ffffff; border:1px solid %3;"
            "  border-radius:10px; }"
            "QLabel#bountyHeading { color:%4; font-size:15px; font-weight:600; }"
            "QLabel#bountyAmount { color:%4; font-size:26px; font-weight:700; }"
            "QLabel#bountyUsd { color:%5; font-size:13px; }"
            "QLabel#bountyEntityName { color:%4; font-weight:600; }"
            "QLabel#bountyEntitySub { color:%5; font-size:11px; }"
            "QLabel#bountyEntityPct { color:#3fb950; font-weight:600; }"
            "QLabel#bountyAddr { color:%6; font-family:monospace; background:%1;"
            "  border:1px solid %3; border-radius:6px; padding:6px 8px; }")
            .arg(pageBg, cardBg, border, fg, muted, accent)));
    dialog.setMinimumWidth(380);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->setContentsMargins(18, 18, 18, 18);

    auto *heading = new QLabel(
        isPr ? QStringLiteral("Reward the merged pull request")
             : QString::fromUtf8("Legacy bounty record #%1").arg(number));
    heading->setObjectName("bountyHeading");
    heading->setWordWrap(true);
    layout->addWidget(heading);

    auto *intro = new QLabel(QString::fromUtf8(
        "Migration-only historical display. Do not send funds to this retired "
        "escrow address. Any future reward requires explicit approval in an "
        "external self-custodial wallet."));
    intro->setObjectName("bountyUsd");
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *amountCard = new QFrame;
    amountCard->setObjectName("bountyCard");
    auto *amountBox = new QVBoxLayout(amountCard);
    amountBox->setContentsMargins(14, 12, 14, 12);
    amountBox->setSpacing(2);
    auto *amountLabel = new QLabel(
        QString::fromUtf8("%1 SOL")
            .arg(amountSol.isEmpty() ? QString::fromUtf8("\xE2\x80\xA6") : amountSol));
    amountLabel->setObjectName("bountyAmount");
    amountBox->addWidget(amountLabel);
    auto *usdLabel = new QLabel(
        QString::fromUtf8("\xE2\x89\x88 $%1 USD").arg(
            QString::number(amountUsd, 'f', 2)));
    usdLabel->setObjectName("bountyUsd");
    amountBox->addWidget(usdLabel);
    layout->addWidget(amountCard);

    auto *qrCard = new QFrame;
    qrCard->setObjectName("bountyQrCard");
    auto *qrBox = new QVBoxLayout(qrCard);
    qrBox->setContentsMargins(12, 12, 12, 12);
    auto *qrLabel = new QLabel;
    qrLabel->setAlignment(Qt::AlignCenter);
    const auto modules = QrCode::encode(uri.toUtf8());
    if (!modules.empty()) {
        constexpr int kMargin = 3;
        constexpr int kMaxQrPx = 300;
        const int span = static_cast<int>(modules.size()) + 2 * kMargin;
        const int scale = qMax(2, kMaxQrPx / span);
        const QImage qr = QrCode::encodeToImage(uri, scale, kMargin);
        qrLabel->setPixmap(QPixmap::fromImage(qr));
    } else {
        qrLabel->setText(QStringLiteral("Scan the escrow address below"));
        qrLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#57606a;")));
    }
    qrBox->addWidget(qrLabel);
    layout->addWidget(qrCard, 0, Qt::AlignCenter);

    auto *addr = new QLabel(address);
    addr->setObjectName("bountyAddr");
    addr->setTextInteractionFlags(Qt::TextSelectableByMouse);
    addr->setAlignment(Qt::AlignCenter);
    addr->setWordWrap(true);
    addr->setToolTip(QStringLiteral("Escrow deposit address"));
    layout->addWidget(addr);

    const bool havePayee = !payee.trimmed().isEmpty();
    const QString authorName = havePayee
                                   ? QString::fromUtf8("@%1").arg(payee.trimmed())
                                   : QStringLiteral("Pull request author");
    struct Entity {
        QString name;
        QString sub;
        int pct;
    };
    const QList<Entity> entities{
        {authorName,
         havePayee ? QStringLiteral("Pull request author")
                   : QStringLiteral("Paid to the merged PR's author"),
         90},
        {QStringLiteral("ForkMesh treasury"),
         QStringLiteral("Protocol fee"), 10},
    };
    auto *splitCard = new QFrame;
    splitCard->setObjectName("bountyCard");
    auto *splitGrid = new QGridLayout(splitCard);
    splitGrid->setContentsMargins(14, 12, 14, 12);
    splitGrid->setHorizontalSpacing(10);
    splitGrid->setVerticalSpacing(10);
    splitGrid->setColumnStretch(0, 1);
    int gridRow = 0;
    for (const Entity &e : entities) {
        auto *nameCol = new QVBoxLayout;
        nameCol->setSpacing(0);
        auto *name = new QLabel(e.name);
        name->setObjectName("bountyEntityName");
        name->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto *sub = new QLabel(e.sub);
        sub->setObjectName("bountyEntitySub");
        nameCol->addWidget(name);
        nameCol->addWidget(sub);
        splitGrid->addLayout(nameCol, gridRow, 0);
        auto *pct = new QLabel(
            QString::fromUtf8("%1%  \xC2\xB7  \xE2\x89\x88 $%2")
                .arg(e.pct)
                .arg(QString::number(amountUsd * e.pct / 100.0, 'f', 2)));
        pct->setObjectName("bountyEntityPct");
        pct->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        splitGrid->addWidget(pct, gridRow, 1);
        ++gridRow;
    }
    layout->addWidget(splitCard);

    auto *status = new QLabel(QStringLiteral("Waiting for the deposit…"));
    status->setObjectName("modeHint");
    status->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1;").arg(muted)));
    status->setWordWrap(true);
    status->setAlignment(Qt::AlignCenter);
    layout->addWidget(status);

    static const char *kBountySpin[] = {
        "\xE2\xA0\x8B", "\xE2\xA0\x99", "\xE2\xA0\xB9", "\xE2\xA0\xB8",
        "\xE2\xA0\xBC", "\xE2\xA0\xB4", "\xE2\xA0\xA6", "\xE2\xA0\xA7",
        "\xE2\xA0\x87", "\xE2\xA0\x8F"};
    int spinIdx = 0;
    QString confirmMsg;
    auto *spin = new QTimer(&dialog);
    spin->setInterval(120);
    connect(spin, &QTimer::timeout, &dialog, [&]() {
        status->setText(QString::fromUtf8(kBountySpin[spinIdx]) +
                        QStringLiteral(" ") + confirmMsg);
        spinIdx = (spinIdx + 1) % 10;
    });

    auto *copyBtn = new QPushButton(QStringLiteral("Copy address"));
    connect(copyBtn, &QPushButton::clicked, this, [address] {
        QGuiApplication::clipboard()->setText(address);
    });
    auto *closeBtn = new QPushButton(QStringLiteral("Done"));
    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    auto *row = new QHBoxLayout;
    row->addWidget(copyBtn);
    row->addStretch();
    row->addWidget(closeBtn);
    layout->addLayout(row);

    bool paid = false;
    auto *poll = new QTimer(&dialog);
    poll->setInterval(4000);
    connect(poll, &QTimer::timeout, &dialog, [&, this]() {
        if (!m_networkAccess)
            return;
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
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (obj.value("status").toString() == QLatin1String("paid")) {
            paid = true;
            poll->stop();
            spin->stop();
            if (!isPr) {
                IssueStore writeStore = issueStoreForCurrentRepo();
                QString err;
                writeStore.setBounty(number, amountUsd,
                                     obj.value("payee").toString(),
                                     QStringLiteral("paid"), &err);
                if (m_repoDetailIndex == issuesRepoIndex())
                    reloadIssues();
            }
            status->setText(
                QString::fromUtf8("\xE2\x9C\x94 Paid out to the author + "
                                  "treasury (tx %1).")
                    .arg(obj.value("payoutSig").toString().left(12)));
            status->setStyleSheet(Theme::remapSheet("color:#3fb950; background:transparent;"));
            closeBtn->setText(QStringLiteral("Close"));
            QTimer::singleShot(1800, &dialog, &QDialog::accept);
            return;
        }
        const qint64 got =
            obj.value("receivedLamports").toVariant().toLongLong();
        if (got > 0) {
            confirmMsg = QStringLiteral("Received %1 SOL — confirming…")
                             .arg(got / 1000000000.0, 0, 'f', 9);
            if (!spin->isActive()) {
                spinIdx = 0;
                spin->start();
            }
        }
    });
    poll->start();
    dialog.exec();
    poll->stop();
    if (!paid && !isPr)
        pollBountyPayout(repo, number, amountUsd, kind);
#endif
}

void MainWindow::showBountyWalletDialog()
{
    QMessageBox::information(
        this, QStringLiteral("Legacy bounty wallet disabled"),
        QStringLiteral(
            "ForkMesh no longer creates, funds, or spends from Worker-held "
            "bounty wallets. Do not send new funds to a legacy address.\n\n"
            "No replacement issue or pull-request escrow is active. A future "
            "reward flow must use a direct external-wallet approval or reviewed "
            "program/multisig contract. The separate community reward pool is "
            "managed from Control node: its imported signer remains encrypted "
            "on the first-instance owner's device and every transfer requires "
            "explicit local review.\n\n"
            "An operator must use the documented legacy-custody migration "
            "procedure to inventory and recover any historical balance."));
}

void MainWindow::editIssueBounty()
{
    if (m_currentIssueNumber < 0)
        return;
    setIssueInlineNotice(
        QStringLiteral(
            "New issue bounties are disabled. The former funding flow depended "
            "on Worker-held escrow keys; historical entries are read-only and "
            "must be handled through the operator migration procedure."),
        true);
}

void MainWindow::bountyAllOpenIssues(double amountUsd)
{
    Q_UNUSED(amountUsd);
    setIssueInlineNotice(
        QStringLiteral(
            "Bulk bounties are disabled because the former payout path used "
            "Worker-held escrow keys. No pledge or transfer was created."),
        true);
}

namespace {
QJsonArray remoteAttachmentDataJson(const QList<RemoteAttachment> &attachments)
{
    QJsonArray array;
    for (const RemoteAttachment &att : attachments)
        array.append(QJsonObject{{"name", att.name},
                                 {"data", QString::fromLatin1(att.data.toBase64())}});
    return array;
}
} // namespace

void MainWindow::submitIssueCommentToInbox(const QString &body,
                                           const QStringList &attachmentSrcPaths,
                                           const QStringList &attachmentPlaceholders)
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return;
    const RepositoryRecord &repo = m_repositories.at(idx);

    const QList<RemoteAttachment> attachmentData =
        IssueStore::readAttachmentsForRemoteSubmit(attachmentSrcPaths);
    QStringList attachmentNames;
    for (const RemoteAttachment &att : attachmentData)
        attachmentNames << att.name;

    QString text = IssueStore::substituteAttachmentPlaceholders(
        body, attachmentSrcPaths, attachmentPlaceholders, attachmentNames);
    while (text.endsWith('\n') || text.endsWith('\r'))
        text.chop(1);

    IssueStore store = issueStoreForCurrentRepo();
    IssueEvent ev;
    ev.type = "comment";
    ev.body = text;
    ev.attachments = attachmentNames;
    ev = store.makeSignedEvent(m_currentIssueNumber, ev);

    QJsonObject eventJson = ev.toJson();
    eventJson.insert("body", ev.body); // worker needs the text to verify the sig
    const QJsonObject payload{{"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", m_currentIssueNumber},
                              {"event", eventJson},
                              {"attachmentData", remoteAttachmentDataJson(attachmentData)}};

    QNetworkRequest request(issuesApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::NoError) {
            if (m_issueComposer) {
                m_issueComposer->setMarkdown(QString());
                m_issueComposer->clearPendingAttachments();
            }
            setIssueInlineNotice(
                "Your signed comment was delivered to the maintainer's inbox.");
        } else {
            setIssueInlineNotice("Could not send the comment: " + reply->errorString(),
                                 true);
        }
    });
}

void MainWindow::submitIssueAssigneesToInbox(int number,
                                             const QStringList &assignees)
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return;
    const RepositoryRecord &repo = m_repositories.at(idx);

    IssueStore store = issueStoreForCurrentRepo();
    IssueEvent ev;
    ev.type = "assignees";
    ev.assignees = assignees;
    ev = store.makeSignedEvent(number, ev);

    const QJsonObject payload{{"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", number},
                              {"event", ev.toJson()}};

    QNetworkRequest request(issuesApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply] { reply->deleteLater(); });
}

bool MainWindow::submitNewIssueToInbox(
    const QString &title, const QString &body, const QStringList &labels,
    const QString &milestone, int priority, const QStringList &assignees,
    const QStringList &attachmentSrcPaths, const QStringList &attachmentPlaceholders,
    std::function<void(bool ok, const QString &error)> onDone)
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return false;
    const RepositoryRecord &repo = m_repositories.at(idx);

    // Propose the next number from the mirror's view; the owner reassigns it if
    // it collides with an issue we haven't synced yet (the signature is advisory
    // once the owner commits and vouches for the merge).
    int proposed = 1;
    for (const Issue &issue : std::as_const(m_currentIssues))
        if (issue.number >= proposed)
            proposed = issue.number + 1;

    const QList<RemoteAttachment> attachmentData =
        IssueStore::readAttachmentsForRemoteSubmit(attachmentSrcPaths);
    QStringList attachmentNames;
    for (const RemoteAttachment &att : attachmentData)
        attachmentNames << att.name;

    IssueStore store = issueStoreForCurrentRepo();
    IssueEvent ev;
    ev.type = "open";
    ev.id = QStringLiteral("open-%1").arg(proposed);
    ev.title = title;
    ev.attachments = attachmentNames;
    ev.body = IssueStore::substituteAttachmentPlaceholders(
        body, attachmentSrcPaths, attachmentPlaceholders, attachmentNames);
    while (ev.body.endsWith('\n') || ev.body.endsWith('\r'))
        ev.body.chop(1);
    ev = store.makeSignedEvent(proposed, ev);

    QJsonObject eventJson = ev.toJson();
    eventJson.insert("body", ev.body); // worker needs the text to verify the sig
    // Issue-level metadata isn't part of the open-event signature; the owner
    // applies it on merge (vouched, like the rest of an accepted submission).
    QJsonObject meta{{"labels", QJsonArray::fromStringList(labels)},
                     {"milestone", milestone},
                     {"priority", priority},
                     {"assignees", QJsonArray::fromStringList(assignees)}};
    const QJsonObject payload{{"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", proposed},
                              {"titleIfNew", title},
                              {"event", eventJson},
                              {"meta", meta},
                              {"attachmentData", remoteAttachmentDataJson(attachmentData)}};

    QNetworkRequest request(issuesApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, onDone] {
        reply->deleteLater();
        const bool ok = reply->error() == QNetworkReply::NoError;
        if (!ok) {
            setIssueInlineNotice("Could not send the issue: " + reply->errorString(),
                                 true);
        }
        if (onDone)
            onDone(ok, ok ? QString() : reply->errorString());
    });
    return true;
}

int MainWindow::availableCredits() const
{
    const qint64 live =
        m_connectedAtMs > 0 ? QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs : 0;
    const int earned = int((m_totalConnectionMs + live) / 3600000); // 1 per hour
    const int spent = QSettings().value(kVotesSpentSetting).toInt();
    return std::max(0, earned - spent);
}

void MainWindow::submitIssueVoteToInbox(int number)
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return;
    const RepositoryRecord &repo = m_repositories.at(idx);
    IssueStore store = issueStoreForCurrentRepo();
    IssueEvent ev;
    ev.type = "vote";
    ev = store.makeSignedEvent(number, ev);
    const QJsonObject payload{{"owner", repo.owner},
                              {"repo", repo.name},
                              {"number", number},
                              {"event", ev.toJson()}};
    QNetworkRequest request(issuesApiUrl(repo));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            setIssueInlineNotice("Could not send your vote: " + reply->errorString(),
                                 true);
    });
}

void MainWindow::voteOnCurrentIssue() { voteOnIssue(m_currentIssueNumber); }

// Votes are cast from the detail pane's button and from any row's thumbs-up
// chip, so the issue is a parameter rather than "whatever is selected".
void MainWindow::voteOnIssue(int number)
{
    const int idx = issuesRepoIndex();
    if (idx < 0 || number < 0)
        return;
    const RepositoryRecord &repo = m_repositories.at(idx);
    const QString key =
        repo.owner + "/" + repo.name + "#" + QString::number(number);
    QStringList voted = QSettings().value(kVotedSetting).toStringList();
    if (availableCredits() <= 0) {
        setIssueInlineNotice(
            "No voting credits yet. You earn 1 credit for every hour online.",
            true);
        return;
    }

    IssueStore store = issueStoreForCurrentRepo();
    if (store.canWrite()) {
        QString error;
        if (!store.addVote(number, &error)) {
            setIssueInlineNotice(error.isEmpty() ? "Could not record your vote." : error,
                                 true);
            return;
        }
    } else {
        // Not the host: submit a signed vote to the maintainer's inbox.
        submitIssueVoteToInbox(number);
        setIssueInlineNotice("Your signed vote was sent to the maintainer's inbox.");
    }

    QSettings s;
    s.setValue(kVotesSpentSetting, s.value(kVotesSpentSetting).toInt() + 1);
    if (!voted.contains(key)) {
        voted << key;
        s.setValue(kVotedSetting, voted);
    }
    reloadIssues();
    setIssueInlineNotice("Vote recorded.");
    updateVoteUi();
}

void MainWindow::updateVoteUi()
{
    if (m_issueCreditsButton)
        m_issueCreditsButton->setText(QString::number(availableCredits()));
    if (!m_issueVoteButton)
        return;
    const bool haveIssue = m_currentIssueNumber >= 0;
    int votes = 0;
    if (haveIssue) {
        for (const Issue &issue : m_currentIssues)
            if (issue.number == m_currentIssueNumber)
                votes = issue.votes;
    }
    const int credits = availableCredits();
    m_issueVoteButton->setText(
        QStringLiteral("Vote (%1)").arg(formatCount(votes)));
    m_issueVoteButton->setEnabled(haveIssue && credits > 0);
    m_issueVoteButton->setToolTip(
        credits > 0
            ? QStringLiteral("Upvote this issue (spends 1 of %1 voting credits)")
                  .arg(credits)
            : QString::fromUtf8("No voting credits yet \xE2\x80\x94 you earn 1 per "
                             "hour online"));
}

void MainWindow::syncIssuesInbox()
{
    const int idx = issuesRepoIndex();
    if (idx < 0)
        return;
    showPendingInbox(m_repositories.at(idx), QStringLiteral("issues"));
}

void MainWindow::noteInboxDrainFailure(const QString &backoffKey, int)
{
	m_pollBackoff.noteFailure(backoffKey,
							  QDateTime::currentMSecsSinceEpoch());
}

void MainWindow::noteInboxDrainSuccess(const QString &backoffKey)
{
    m_pollBackoff.noteSuccess(backoffKey);
}

void MainWindow::drainIssuesInboxFor(RepositoryRecord repo, bool interactive,
									 bool force, int page,
                                     std::function<void()> onFinished)
{
    // A local-only repository has no relay inbox to drain, and the drain URL
    // would carry its owner and name to the relay. Fail closed before any
    // signing or network work.
    if (!PrivateMirrorRuntime::repositoryRelayInboxAllowed(
            repo.publishToNetwork)) {
        if (onFinished)
            onFinished();
        return;
    }
    const RepositoryRecord writable = writableRecordFor(repo);
	IssueStore probe(writable.localPath, writable.mirrorPath, &m_profileIdentity,
					 m_userName);
	if (!probe.canWrite()) {
        if (onFinished)
            onFinished();
        return;
    }
    if (!hasOwnerSigningCapability()) {
        if (onFinished)
            onFinished();
        return;
    }

	const QString signer =
		repoSegment(repo.owner, QStringLiteral("owner"));
	if (signer.isEmpty()) {
        if (onFinished)
            onFinished();
        return;
    }

    QUrl url = issuesApiUrl(repo);
	const QString backoffKey = url.toString();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!interactive && !force && !m_pollBackoff.ready(backoffKey, nowMs)) {
        if (onFinished)
            onFinished();
        return;
    }
    if (page == 0) {
        if (onFinished)
            m_issueInboxDrainWaiters[backoffKey].append(
                std::move(onFinished));
        if (m_issueInboxDraining.contains(backoffKey))
            return;
        m_issueInboxDraining.insert(backoffKey);
    }
    const auto finishDrain = [this, backoffKey] {
        m_issueInboxDraining.remove(backoffKey);
        const QList<std::function<void()>> waiters =
            m_issueInboxDrainWaiters.take(backoffKey);
        for (const auto &waiter : waiters) {
            if (waiter)
                waiter();
        }
    };
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
            [this, reply, repo, interactive, backoffKey, signer, page,
             finishDrain] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
			noteInboxDrainFailure(backoffKey, status);
            if (status == 401 || status == 403) {
                static QSet<QString> s_inboxAuthWarned;
                if (!s_inboxAuthWarned.contains(backoffKey)) {
                    s_inboxAuthWarned.insert(backoffKey);
					logSystem(QStringLiteral(
						"The relay rejected this node's signed issue drain for "
						"%1/%2 as \"%3\" (HTTP %4), so web-filed issues can't "
						"sync down. Re-link this desktop to the account that owns %2.")
							  .arg(repo.owner, repo.name, signer)
							  .arg(status));
                }
            }
            if (interactive)
                setIssueInlineNotice("Could not reach the inbox: " +
                                         reply->errorString(),
                                     true);
            finishDrain();
            return;
        }
        noteInboxDrainSuccess(backoffKey);
        const QJsonObject response =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray pending = response.value("pending").toArray();
        const bool more = response.value("more").toBool();
        constexpr int kMaxDrainPages = 64;
        applyIssuesInboxPayload(
            repo, pending, interactive,
            [this, repo, interactive, more, page,
             finishDrain](bool applied) {
                if (applied && more && page + 1 < kMaxDrainPages) {
                    // The DELETE acknowledgement has completed, so the next
                    // GET cannot replay the same leased page.
                    drainIssuesInboxFor(repo, interactive, /*force=*/true,
                                        page + 1);
                    return;
                }
                if (more && page + 1 >= kMaxDrainPages)
                    logSystem(QStringLiteral(
                        "Issue inbox drain reached its 64-page safety cap for "
                        "%1/%2; the next refresh will continue it.")
                                  .arg(repo.owner, repo.name));
                finishDrain();
            });
    });
}

void MainWindow::syncIssuesFromMeshOnOpen(int index)
{
    if (index < 0 || index >= m_repositories.size())
        return;
    // The same visible feedback is used for the automatic page-load pull and
    // an explicit Refresh click. spinRefreshButton coalesces overlapping calls.
    spinRefreshButton(m_issueSyncButton);
    const RepositoryRecord repo = m_repositories.at(index);
    // Anything still queued on the relay is ours to merge. The poll backoff
    // exists to keep the *background* sweep off a relay that just failed us; a
    // page the user opened is bounded by the click, so it always asks.
    if (!isSourceOfTruthRepo(repo)) {
        drainIssuesInboxFor(repo, /*interactive=*/false, /*force=*/true);
        return;
    }
    // The drain alone cannot make this node whole. A submission a mirror won
    // the inbox lease for is committed to that mirror's own copy and deleted
    // from the queue on ack, so it never comes back through the inbox at all --
    // only a fetch from the mesh returns it. The periodic sweep acts at most
    // once per peer refs fingerprint, so one missed convergence leaves the
    // source of truth permanently serving fewer issues than its own mirrors
    // advertise. Opening the Issues page is exactly when that gap must close.
    // Re-entry is already bounded: a repo that is mid-sync is skipped.
    const QString owner = repo.owner;
    const QString name = repo.name;
    drainIssuesInboxFor(
        repo, /*interactive=*/false, /*force=*/true, /*page=*/0,
        [this, owner, name] {
            const int liveIndex = repoIndexFor(owner, name);
            if (liveIndex >= 0)
                convergeSourceRepoFromMesh(liveIndex, /*force=*/true);
        });
}

// code-health: allow(deep-nested-loop) the agent-request match walks issues x events only for issues opened by this inbox page that asked for an agent (usually none or one), each stopping at its match
void MainWindow::applyIssuesInboxPayload(const RepositoryRecord &repo,
                                         const QJsonArray &pending,
										 bool interactive,
                                         std::function<void(bool)> onApplied)
{
    if (!hasOwnerSigningCapability()) {
        if (onApplied)
            onApplied(false);
        return;
    }
    if (pending.isEmpty()) {
        if (interactive)
            setIssueInlineNotice("The collaboration inbox is up to date.");
        if (onApplied)
            onApplied(true);
        return;
    }
    const RepositoryRecord writable = writableRecordFor(repo);
    IssueStore store(writable.localPath, writable.mirrorPath, &m_profileIdentity,
                     m_userName);
    if (!store.canWrite()) {
        if (onApplied)
            onApplied(false);
        return;
    }
    int merged = 0;
    int comments = 0;
    int newIssues = 0;
    QString lastCommentAuthor;
    int lastCommentNumber = 0;
    QString lastCommentBody;
    QString lastIssueAuthor;
    QString lastIssueTitle;
    int lastIssueNumber = 0;
    struct AgentRequest {
        IssueEvent event;
        QString model;
        QString provider;
    };
    QList<AgentRequest> agentRequests;
    struct CommentAgentRequest {
        int number = 0;
        QString model;
        QString provider;
    };
    QList<CommentAgentRequest> commentAgentRequests;
    struct FediverseMaterialization {
        QString mentionId;
        QString eventId;
        QString inboxId;
    };
    QList<FediverseMaterialization> fediverseMaterializations;
    struct DuplicateMaterialization {
        QString inboxId;
        QString eventId;
        QString signature;
        QString type;
    };
    QList<DuplicateMaterialization> duplicateMaterializations;
    QStringList drainedIds;
    for (const QJsonValue &value : pending) {
        const QJsonObject item = value.toObject();
        const QJsonValue idVal = item.value("id");
        const QString inboxId =
            idVal.isDouble()
                ? QString::number(static_cast<qint64>(idVal.toDouble()))
                : QString();
        const int number = item.value("number").toInt();
        const QJsonObject eventObj = item.value("event").toObject();
        IssueEvent ev = IssueEvent::fromJson(eventObj);
        ev.body = eventObj.value("body").toString();
        const QString mentionId =
            item.value("fediverseMentionId").toString().toLower();
        const bool validMentionId =
            ev.type == QLatin1String("open") && !ev.id.isEmpty() &&
            mentionId.size() == 32 &&
            std::all_of(
                mentionId.cbegin(), mentionId.cend(), [](QChar ch) {
                    return ch.isDigit() ||
                           (ch >= QLatin1Char('a') &&
                            ch <= QLatin1Char('f'));
                });
        if (validMentionId) {
            // Queue the lookup even when applyRemoteEvent reports a duplicate:
            // the previous owner-node write may have committed successfully
            // while its signed acknowledgement was lost. We only drain and
            // confirm below if the matching open event is actually present.
            fediverseMaterializations.append(
                FediverseMaterialization{mentionId, ev.id, inboxId});
        }
        const QString titleIfNew = item.value("titleIfNew").toString();
        const QJsonObject metaObj = item.value("meta").toObject();
        RemoteIssueMeta meta;
        for (const QJsonValue &l : metaObj.value("labels").toArray())
            meta.labels << l.toString();
        meta.milestone = metaObj.value("milestone").toString();
        meta.priority = metaObj.value("priority").toInt();
        for (const QJsonValue &a : metaObj.value("assignees").toArray())
            meta.assignees << a.toString();
        meta.reporter = metaObj.value("reporter").toString().trimmed();
        meta.rewardEstimateLamports = qint64(
            metaObj.value("rewardEstimateLamports").toDouble());
        meta.rewardRecipient = metaObj.value("rewardRecipient").toString();
        meta.rewardId = metaObj.value("rewardId").toString();
        meta.rewardCondition = metaObj.value("rewardCondition").toString();
        meta.rewardStatus = metaObj.value("rewardStatus").toString();
        meta.wantsAgent = metaObj.value("wantsAgent").toBool();
        meta.wantsAgentModel = metaObj.value("model").toString();
        meta.wantsAgentProvider = metaObj.value("provider").toString();
        QList<RemoteAttachment> attachmentData;
        for (const QJsonValue &a : item.value("attachmentData").toArray()) {
            const QJsonObject obj = a.toObject();
            attachmentData.append(RemoteAttachment{
                obj.value("name").toString(),
                QByteArray::fromBase64(obj.value("data").toString().toLatin1())});
        }
        const bool applied = store.applyRemoteEvent(
            number, ev, titleIfNew, nullptr, meta, attachmentData);
        if (applied) {
			const bool ownerOnlyRow = validMentionId;
            if (!ownerOnlyRow && !inboxId.isEmpty() &&
                !drainedIds.contains(inboxId))
                drainedIds << inboxId;
            ++merged;
            const QString who =
                ev.type == QLatin1String("open") && !meta.reporter.isEmpty()
                    ? meta.reporter
                    : (ev.authorName.isEmpty() ? ev.author.left(8)
                                               : ev.authorName);
            if (ev.type == QLatin1String("comment")) {
                ++comments;
                lastCommentAuthor = who;
                lastCommentNumber = number;
                lastCommentBody = ev.body.simplified();
                if (meta.wantsAgent && number > 0)
                    commentAgentRequests << CommentAgentRequest{
                        number, meta.wantsAgentModel, meta.wantsAgentProvider};
            } else if (ev.type == QLatin1String("open")) {
                ++newIssues;
                lastIssueAuthor = who;
                lastIssueTitle = ev.title.isEmpty() ? titleIfNew : ev.title;
                lastIssueNumber = number;
                if (meta.wantsAgent)
                    agentRequests << AgentRequest{ev, meta.wantsAgentModel,
                                                  meta.wantsAgentProvider};
            }
        } else if (!inboxId.isEmpty() &&
                   (!ev.id.isEmpty() || !ev.sig.isEmpty())) {
            // A previous apply may have committed successfully while its
            // DELETE reply was lost. Keep enough signed identity to prove the
            // event is already present before acknowledging the replay.
            duplicateMaterializations.append(
                DuplicateMaterialization{inboxId, ev.id, ev.sig, ev.type});
        }
    }
    QStringList materialized;
    QList<Issue> mergedIssues;
    if (!fediverseMaterializations.isEmpty() ||
        !duplicateMaterializations.isEmpty())
        mergedIssues = store.loadAll();
    if (!duplicateMaterializations.isEmpty()) {
        for (const DuplicateMaterialization &wanted :
             std::as_const(duplicateMaterializations)) {
            bool found = false;
            for (const Issue &candidate : std::as_const(mergedIssues)) {
                found = std::any_of(
                    candidate.events.cbegin(), candidate.events.cend(),
                    [&wanted](const IssueEvent &event) {
                        if (event.type != wanted.type)
                            return false;
                        const bool sameId = !wanted.eventId.isEmpty() &&
                                            event.id == wanted.eventId;
                        const bool sameSignature = !wanted.signature.isEmpty() &&
                                                   event.sig == wanted.signature;
                        return sameId || sameSignature;
                    });
                if (found)
                    break;
            }
            if (found && !drainedIds.contains(wanted.inboxId))
                drainedIds.append(wanted.inboxId);
        }
    }
    if (!fediverseMaterializations.isEmpty()) {
        for (const FediverseMaterialization &wanted :
             std::as_const(fediverseMaterializations)) {
            for (const Issue &candidate : mergedIssues) {
                const bool found = std::any_of(
                    candidate.events.cbegin(),
                    candidate.events.cend(),
                    [&wanted](const IssueEvent &event) {
                        return event.type == QLatin1String("open") &&
                               event.id == wanted.eventId;
                    });
                if (!found)
                    continue;
                materialized.append(
                    wanted.mentionId + QLatin1Char(':') +
                    QString::number(candidate.number));
                if (!wanted.inboxId.isEmpty() &&
                    !drainedIds.contains(wanted.inboxId))
                    drainedIds << wanted.inboxId;
                break;
            }
        }
    }
    if (!drainedIds.isEmpty()) {
        QUrl ackUrl = issuesApiUrl(repo);
		const QString ackSigner =
			repoSegment(repo.owner, QStringLiteral("owner"));
        QUrlQuery ackQuery = signedInboxQuery(ackSigner);
        ackQuery.addQueryItem("ids", drainedIds.join(QStringLiteral(",")));
		if (!materialized.isEmpty())
            ackQuery.addQueryItem(
                QStringLiteral("materialized"),
                materialized.join(QStringLiteral(",")));
        ackUrl.setQuery(ackQuery);
        QNetworkReply *ackReply =
            m_networkAccess->deleteResource(QNetworkRequest(ackUrl));
        if (onApplied) {
            connect(ackReply, &QNetworkReply::finished, this,
                    [ackReply, onApplied = std::move(onApplied)]() mutable {
                        const bool ok =
                            ackReply->error() == QNetworkReply::NoError;
                        ackReply->deleteLater();
                        onApplied(ok);
                    });
        } else {
            connect(ackReply, &QNetworkReply::finished, ackReply,
                    &QObject::deleteLater);
        }
    } else if (onApplied) {
        // A non-empty page that yielded no safe acknowledgement would repeat
        // forever. Stop here and leave its rows leased for a later retry.
        onApplied(false);
    }
    const int curIdx = issuesRepoIndex();
    if (curIdx >= 0 &&
        m_repositories.at(curIdx).owner == repo.owner &&
        m_repositories.at(curIdx).name == repo.name)
        reloadIssues();
	if (!agentRequests.isEmpty() || !commentAgentRequests.isEmpty()) {
        const QList<Issue> mergedIssues = store.loadAll();
        const auto issueHasAgent = [](const Issue &issue) {
            for (const IssueEvent &e : issue.events) {
                if (e.type == QLatin1String("agent") && e.agentSessionId > 0)
                    return true;
            }
            return false;
        };
        for (const auto &request : std::as_const(commentAgentRequests)) {
            const QString provider = request.provider.trimmed().isEmpty()
                                         ? defaultAgentProvider()
                                         : request.provider.trimmed();
            for (const Issue &candidate : mergedIssues) {
                if (candidate.number != request.number || candidate.isDeleted())
                    continue;
                if (!issueHasAgent(candidate))
                    startAgentForIssue(candidate, provider,
                                       /*createPr=*/false, /*quiet=*/true,
                                       request.model, &repo);
                break;
            }
        }
        for (const auto &request : std::as_const(agentRequests)) {
            const IssueEvent &wanted = request.event;
            const QString &wantedModel = request.model;
            const QString wantedProvider = request.provider.trimmed().isEmpty()
                                               ? defaultAgentProvider()
                                               : request.provider.trimmed();
            for (const Issue &candidate : mergedIssues) {
                if (candidate.isDeleted())
                    continue;
                bool matched = false;
                for (const IssueEvent &e : candidate.events) {
                    if (e.type != QLatin1String("open"))
                        continue;
                    const bool sameSig =
                        !wanted.sig.isEmpty() && e.sig == wanted.sig;
                    const bool sameAuthorTs =
                        wanted.sig.isEmpty() && e.author == wanted.author &&
                        e.ts == wanted.ts && e.title == wanted.title;
                    if (sameSig || sameAuthorTs) {
                        matched = true;
                        break;
                    }
                }
                if (matched) {
                    if (!issueHasAgent(candidate))
                        startAgentForIssue(candidate, wantedProvider,
                                           /*createPr=*/false, /*quiet=*/true,
                                           wantedModel, &repo);
                    break;
                }
            }
        }
    }
    if (merged > 0) {
        propagateRepoUpdate(repoIndexFor(repo.owner, repo.name));
		scanRepoMentionsFor(writable);
    }
    if (interactive)
		setIssueInlineNotice(QStringLiteral(
			"Merged %1 submission(s) into .forkmesh/issues/.")
						 .arg(merged));

	if (newIssues > 0) {
        const QString body =
            newIssues == 1
                ? QStringLiteral("%1 filed a new issue on %2/%3: %4")
                      .arg(lastIssueAuthor, repo.owner, repo.name, lastIssueTitle)
                : QStringLiteral("%1 new issues filed on %2/%3")
                      .arg(newIssues)
                      .arg(repo.owner, repo.name);
        const QString where = repo.owner + QLatin1Char('/') + repo.name;
        const QString alertTitle =
            newIssues == 1
                ? forkmesh::pings::alertLeadTitle(lastIssueAuthor, where,
                                                  lastIssueNumber)
                : where;
        const QString alertBody =
            newIssues == 1 && !lastIssueTitle.trimmed().isEmpty()
                ? forkmesh::pings::alertSnippet(lastIssueTitle)
                : QStringLiteral("%1 new issues").arg(newIssues);
        flashPingMessage(body);
        if (notifyEnabled(kIssueAlertSetting))
            notifyIfInactive(alertTitle, alertBody);
        NotificationLink link;
        link.kind = QStringLiteral("issue");
        link.owner = repo.owner;
        link.name = repo.name;
        link.number = newIssues == 1 ? lastIssueNumber : -1;
        addNotification(alertTitle, alertBody, false, link);
        if (notifyEnabled(kIssueAlertSetting))
            showPingTrayMessage(alertTitle, alertBody);
        if (m_repoDetailIndex == repoIndexFor(repo.owner, repo.name))
            triggerTeammatesForEvent(QStringLiteral("new-issues"), body);
    }
	if (comments > 0) {
        const QString where = repo.owner + QLatin1Char('/') + repo.name;
        const QString alertTitle =
            comments == 1
                ? forkmesh::pings::alertLeadTitle(lastCommentAuthor, where,
                                                  lastCommentNumber)
                : where;
        const QString alertBody =
            comments == 1 && !lastCommentBody.isEmpty()
                ? forkmesh::pings::alertSnippet(lastCommentBody)
                : (comments == 1 ? QStringLiteral("Commented on issue #%1")
                                       .arg(lastCommentNumber)
                                 : QStringLiteral("%1 new comments on issues")
                                       .arg(comments));
        if (notifyEnabled(kCommentAlertSetting))
            notifyIfInactive(alertTitle, alertBody);
        NotificationLink link;
        link.kind = QStringLiteral("issue");
        link.owner = repo.owner;
        link.name = repo.name;
        link.number = comments == 1 ? lastCommentNumber : -1;
        addNotification(alertTitle, alertBody, false, link);
        if (notifyEnabled(kCommentAlertSetting))
            showPingTrayMessage(alertTitle, alertBody);
    }
}

QWidget *MainWindow::buildChatSection()
{
    auto *page = new QWidget;
    page->setProperty(forkmesh::sonify::kQuietSoundProperty, true);

    QPushButton *addChannelButton = nullptr;
    QWidget *sidebar = buildChatSidebar(addChannelButton);
    QWidget *header = buildChatHeader();
    buildChatMembersPanel();
    QPushButton *sendButton = nullptr;
    QWidget *mainColumnHost = buildChatMainColumn(header, sendButton);
    buildChatThreadPane();

    m_chatSplit = new QSplitter(Qt::Horizontal);
    m_chatSplit->setObjectName(QStringLiteral("chatSidebarSplit"));
    m_chatSplit->setChildrenCollapsible(false);
    mainColumnHost->setMinimumWidth(kChatMainColumnMinWidth);
    m_chatSplit->addWidget(sidebar);
    m_chatSplit->addWidget(mainColumnHost);
    m_chatSplit->addWidget(m_chatThreadPane);
    // Neither pane may be dragged shut. setChildrenCollapsible alone governs
    // the behaviour, but isCollapsible() only reports explicit per-pane
    // settings, so set both (only valid once the panes exist).
    m_chatSplit->setCollapsible(0, false);
    m_chatSplit->setCollapsible(1, false);
    m_chatSplit->setCollapsible(2, false);
    // Only the transcript column stretches, so the sidebar keeps the width it
    // was dragged to when the window resizes.
    m_chatSplit->setStretchFactor(0, 0);
    m_chatSplit->setStretchFactor(1, 1);
    m_chatSplit->setStretchFactor(2, 0);
    const int savedSidebarWidth = qBound(
        kChatSidebarMinWidth,
        QSettings()
            .value(kChatSidebarWidthSetting, kChatSidebarDefaultWidth)
            .toInt(),
        kChatSidebarMaxWidth);
    m_chatSplit->setSizes(
        {savedSidebarWidth, 1000, kChatThreadPaneDefaultWidth});
    m_chatThreadPane->hide();
    connect(m_chatSplit, &QSplitter::splitterMoved, this,
            [this] { persistChatSidebarWidth(m_chatSplit->sizes().value(0)); });

    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_chatSplit);

    m_chatMembersPanel->hide();
    refreshChatMembers();
    refreshChatUserDirectory();

    connectChatSectionSignals(addChannelButton, sendButton);

    return page;
}

namespace {

constexpr int kChatRoomRowHeight = 32;
constexpr int kChatRoomTallRowHeight = 44;
constexpr int kChatRoomRowPadX = 16;
constexpr int kChatRoomGearPadX = 2;
constexpr int kChatRoomGlyphGap = 8;
constexpr int kChatRoomChevronGap = 4;
constexpr int kChatRoomGlyphSize = 14;
constexpr int kChatRoomLeadSize = 28;
constexpr int kChatRoomHashFontPx = 20;
constexpr int kChatRoomGearSize = 14;
constexpr int kChatRoomUnreadDot = 6;
constexpr int kChatRoomPresenceDot = 8;
constexpr int kChatRoomBadgeHeight = 18;
constexpr int kChatRoomGroupFontPx = 13;
constexpr int kChatRoomFontPx = 15;
constexpr int kChatRoomPreviewFontPx = 13;
constexpr int kChatRoomTimeFontPx = 11;
constexpr int kChatRoomLineGap = 1;
constexpr int kChatRoomRowInset = 8;
constexpr int kChatRoomRowRadius = 4;

struct ChatRoomRow {
    QString label;
    QString preview;
    qint64 previewTimeMs = 0;
    QPixmap avatar;
    bool group = false;
    bool collapsed = false;
    bool unread = false;
    bool locked = false;
    bool direct = false;
    bool online = false;
    int unreadCount = 0;
};

ChatRoomRow chatRoomRow(const QModelIndex &index)
{
    ChatRoomRow row;
    row.label = index.data(Qt::DisplayRole).toString();
    row.group = index.data(ChannelGroupTree::kGroupRole).isValid();
    if (row.group && row.label.size() > 1 &&
        (row.label.at(0) == QChar(0x25B8) || row.label.at(0) == QChar(0x25BE))) {
        row.collapsed = row.label.at(0) == QChar(0x25B8);
        row.label.remove(0, 2);
    }
    const QString unreadMark = QString(QChar(0x25CF)) + QLatin1Char(' ');
    if (row.label.startsWith(unreadMark)) {
        row.unread = true;
        row.label.remove(0, unreadMark.size());
    }
    const QString lockMark = QString::fromUtf8("\xF0\x9F\x94\x92 ");
    if (row.label.startsWith(lockMark)) {
        row.locked = true;
        row.label.remove(0, lockMark.size());
    }
    const QVariant presence = index.data(ChannelGroupTree::kPresenceRole);
    row.direct = presence.isValid();
    row.online = presence.toBool();
    row.unreadCount = index.data(ChannelGroupTree::kUnreadCountRole).toInt();
    row.preview = index.data(ChannelGroupTree::kPreviewRole).toString();
    row.previewTimeMs =
        index.data(ChannelGroupTree::kPreviewTimeRole).toLongLong();
    row.avatar = index.data(ChannelGroupTree::kAvatarRole).value<QPixmap>();
    return row;
}

class ChatRoomRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize hint = QStyledItemDelegate::sizeHint(option, index);
        const bool group =
            index.siblingAtColumn(0).data(ChannelGroupTree::kGroupRole).isValid();
        if (Theme::isNext()) {
            hint.setHeight(group ? kChatRoomRowHeight : kChatRoomTallRowHeight);
            return hint;
        }
        if (!group)
            hint.setHeight(qMax(hint.height() +
                                    QFontMetrics(previewFont(option.font))
                                        .height() +
                                    kChatRoomLineGap,
                                kChatRoomLeadSize + 12));
        return hint;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            paintClassic(painter, option, index);
            return;
        }
        const auto *tree = qobject_cast<const QTreeView *>(option.widget);
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        const bool gearCell =
            tree && index.column() == ChannelGroupTree::kSettingsColumn;
        const ChatRoomRow row = chatRoomRow(index.siblingAtColumn(0));
        const bool gearColumn = tree && !row.group;
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);

        QRect band = option.rect;
        if (view) {
            band.setLeft(0);
            band.setRight(view->viewport()->width() - 1);
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
        if ((selected && !row.group) || hovered) {
            QRect clip = option.rect;
            if (!gearCell)
                clip.setLeft(band.left());
            if (!gearColumn || gearCell)
                clip.setRight(band.right());
            painter->save();
            painter->setClipRect(clip, Qt::IntersectClip);
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::chatColor(
                selected && !row.group ? QStringLiteral("chatRowSelected")
                                       : QStringLiteral("chatRowHover")));
            painter->drawRoundedRect(
                QRectF(band.adjusted(kChatRoomRowInset, 1, -kChatRoomRowInset, -1)),
                kChatRoomRowRadius, kChatRoomRowRadius);
            painter->restore();
        }
        const QColor railText = Theme::chatColor(QStringLiteral("chatMuted"));
        const QColor ink = (selected && !row.group) || row.unread || hovered
                               ? Theme::chatColor(QStringLiteral("chatTextStrong"))
                               : railText;

        if (gearCell) {
            if ((selected || hovered) &&
                index.data(Qt::DecorationRole).isValid()) {
                painter->drawPixmap(
                    glyphRect(option.rect, option.rect.left() +
                                               (option.rect.width() -
                                                kChatRoomGearSize) / 2,
                              kChatRoomGearSize),
                    tintedOcticonPixmap(QStringLiteral("gear"), ink,
                                        kChatRoomGearSize));
            }
            painter->restore();
            return;
        }

        int left = option.rect.left() + kChatRoomRowPadX;
        int right = option.rect.right() - kChatRoomRowPadX;
        if (gearColumn)
            right = option.rect.right() - kChatRoomGearPadX;
        else if (row.unread || !row.preview.isEmpty())
            right = option.rect.right() - kChatRoomGearPadX -
                    ChannelGroupTree::kSettingsColumnWidth;
        if (row.group) {
            painter->drawPixmap(
                glyphRect(option.rect, left, kChatRoomGlyphSize),
                tintedOcticonPixmap(
                    row.collapsed ? QStringLiteral("chevron-right")
                                  : QStringLiteral("chevron-down"),
                    railText, kChatRoomGlyphSize));
            left += qMax(tree ? tree->indentation() : 0,
                         kChatRoomGlyphSize + kChatRoomChevronGap);
        }

        QFont font = option.font;
        font.setPixelSize(row.group ? kChatRoomGroupFontPx : kChatRoomFontPx);
        font.setWeight(row.unread ? QFont::Bold : QFont::Normal);

        if (row.direct && !row.avatar.isNull()) {
            painter->drawPixmap(glyphRect(option.rect, left, kChatRoomLeadSize),
                                row.avatar);
            left += kChatRoomLeadSize + kChatRoomGlyphGap;
        } else if (row.direct) {
            const QRectF dot(left + (kChatRoomLeadSize - kChatRoomPresenceDot) / 2.0,
                             option.rect.top() +
                                 (option.rect.height() - kChatRoomPresenceDot) / 2.0,
                             kChatRoomPresenceDot, kChatRoomPresenceDot);
            if (row.online) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(Theme::chatColor(QStringLiteral("chatOnline")));
            } else {
                painter->setPen(QPen(ink, 1.4));
                painter->setBrush(Qt::NoBrush);
            }
            painter->drawEllipse(dot.adjusted(0.7, 0.7, -0.7, -0.7));
            left += kChatRoomLeadSize + kChatRoomGlyphGap;
        } else if (row.locked) {
            painter->drawPixmap(
                glyphRect(option.rect,
                          left + (kChatRoomLeadSize - kChatRoomGlyphSize) / 2,
                          kChatRoomGlyphSize),
                tintedOcticonPixmap(QStringLiteral("lock"), ink,
                                    kChatRoomGlyphSize));
            left += kChatRoomLeadSize + kChatRoomGlyphGap;
        } else if (!row.group) {
            QFont hashFont = font;
            hashFont.setWeight(QFont::Normal);
            hashFont.setPixelSize(kChatRoomHashFontPx);
            painter->setFont(hashFont);
            painter->setPen(railText);
            painter->drawText(QRect(left, option.rect.top(), kChatRoomLeadSize,
                                    option.rect.height()),
                              Qt::AlignCenter, QStringLiteral("#"));
            left += kChatRoomLeadSize + kChatRoomGlyphGap;
        }

        QString label = row.label;
        if (row.direct && label.startsWith(kDmPrefix))
            label.remove(0, kDmPrefix.size());
        else if (!row.group && label.startsWith(QLatin1Char('#')))
            label.remove(0, 1);

        const bool twoLine = !row.group && !row.preview.isEmpty();
        const QFont subFont = previewFont(option.font);
        const QFontMetrics nameMetrics(font);
        const QFontMetrics subMetrics(subFont);
        QRect nameLine = option.rect;
        QRect subLine;
        if (twoLine) {
            const int block =
                nameMetrics.height() + kChatRoomLineGap + subMetrics.height();
            nameLine.setTop(option.rect.top() +
                            (option.rect.height() - block) / 2);
            nameLine.setHeight(nameMetrics.height());
            subLine = QRect(nameLine.left(),
                            nameLine.bottom() + 1 + kChatRoomLineGap,
                            nameLine.width(), subMetrics.height());
        }
        int nameRight = right;
        int subRight = right;

        if (twoLine) {
            const QString when = forkmesh::chatlist::timeLabel(
                row.previewTimeMs, QDateTime::currentDateTime());
            if (!when.isEmpty()) {
                QFont timeFont = option.font;
                timeFont.setPixelSize(kChatRoomTimeFontPx);
                timeFont.setWeight(QFont::Normal);
                const int width = QFontMetrics(timeFont).horizontalAdvance(when);
                painter->setFont(timeFont);
                painter->setPen(railText);
                painter->drawText(QRect(nameRight + 1 - width, nameLine.top(),
                                        width, nameLine.height()),
                                  Qt::AlignRight | Qt::AlignVCenter, when);
                nameRight -= width + kChatRoomGlyphGap;
            }
        }

        if (row.unread && !row.group) {
            const QRect badgeLine = twoLine ? subLine : option.rect;
            int &badgeRight = twoLine ? subRight : nameRight;
            const QString count =
                row.unreadCount > 99 ? QStringLiteral("99+")
                                     : row.unreadCount > 0
                                           ? QString::number(row.unreadCount)
                                           : QString();
            if (count.isEmpty()) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(ink);
                painter->drawEllipse(QRectF(
                    badgeRight + 1 - kChatRoomUnreadDot,
                    badgeLine.top() +
                        (badgeLine.height() - kChatRoomUnreadDot) / 2.0,
                    kChatRoomUnreadDot, kChatRoomUnreadDot));
                badgeRight -= kChatRoomUnreadDot + kChatRoomGlyphGap;
            } else {
                QFont badgeFont = option.font;
                badgeFont.setPixelSize(11);
                badgeFont.setWeight(QFont::Bold);
                const int width = qMax(
                    kChatRoomBadgeHeight,
                    QFontMetrics(badgeFont).horizontalAdvance(count) + 12);
                const int height =
                    qMin(kChatRoomBadgeHeight, qMax(badgeLine.height(), 14));
                const QRectF badge(
                    badgeRight + 1 - width,
                    badgeLine.top() + (badgeLine.height() - height) / 2.0,
                    width, height);
                painter->setPen(Qt::NoPen);
                painter->setBrush(Theme::chatColor(QStringLiteral("chatNew")));
                painter->drawRoundedRect(badge, height / 2.0, height / 2.0);
                painter->setFont(badgeFont);
                painter->setPen(Qt::white);
                painter->drawText(badge, Qt::AlignCenter, count);
                badgeRight -= width + kChatRoomGlyphGap;
            }
        }

        const QRect textRect(left, nameLine.top(), qMax(0, nameRight + 1 - left),
                             nameLine.height());
        painter->setFont(font);
        painter->setPen(ink);
        painter->drawText(textRect,
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          nameMetrics.elidedText(label, Qt::ElideRight,
                                                 textRect.width()));
        if (twoLine) {
            const QRect previewRect(left, subLine.top(),
                                    qMax(0, subRight + 1 - left),
                                    subLine.height());
            painter->setFont(subFont);
            painter->setPen(Theme::chatColor(row.unread ? QStringLiteral("chatText")
                                                        : QStringLiteral("chatMuted")));
            painter->drawText(previewRect,
                              Qt::AlignLeft | Qt::AlignVCenter |
                                  Qt::TextSingleLine,
                              subMetrics.elidedText(row.preview, Qt::ElideRight,
                                                    previewRect.width()));
        }
        painter->restore();
    }

private:
    static QFont previewFont(const QFont &base)
    {
        QFont font = base;
        font.setPixelSize(Theme::isNext() ? kChatRoomPreviewFontPx : 12);
        font.setWeight(QFont::Normal);
        font.setBold(false);
        return font;
    }

    void paintClassic(QPainter *painter, const QStyleOptionViewItem &option,
                      const QModelIndex &index) const
    {
        const ChatRoomRow row = chatRoomRow(index.siblingAtColumn(0));
        if (row.group || index.column() != 0) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        if (!row.avatar.isNull()) {
            QIcon icon;
            icon.addPixmap(row.avatar, QIcon::Normal);
            icon.addPixmap(row.avatar, QIcon::Selected);
            icon.addPixmap(row.avatar, QIcon::Active);
            opt.icon = icon;
            opt.decorationSize = row.avatar.deviceIndependentSize().toSize();
            opt.decorationAlignment = Qt::AlignCenter;
            opt.features |= QStyleOptionViewItem::HasDecoration;
        }
        if (!row.preview.isEmpty())
            opt.displayAlignment = Qt::AlignLeft | Qt::AlignTop;
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        if (row.preview.isEmpty())
            return;
        const QRect text =
            style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
        const QFont subFont = previewFont(option.font);
        const QFontMetrics metrics(subFont);
        const QRect previewRect(text.left() + 4,
                                text.bottom() + 1 - metrics.height(),
                                qMax(0, text.width() - 8), metrics.height());
        const bool selected = opt.state.testFlag(QStyle::State_Selected);
        QColor color = selected ? QColor(Qt::white)
                                : opt.palette.color(QPalette::Text);
        color.setAlphaF(selected || row.unread ? 0.85 : 0.6);
        painter->save();
        painter->setFont(subFont);
        painter->setPen(color);
        painter->drawText(previewRect,
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                          metrics.elidedText(row.preview, Qt::ElideRight,
                                             previewRect.width()));
        painter->restore();
    }

    static QRect glyphRect(const QRect &cell, int left, int size)
    {
        return QRect(left, cell.top() + (cell.height() - size) / 2, size, size);
    }
};

}

QWidget *MainWindow::buildChatSidebar(QPushButton *&addChannelButton)
{
    auto *sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setMinimumWidth(kChatSidebarMinWidth);
    sidebar->setMaximumWidth(kChatSidebarMaxWidth);

    m_statusLine = new QLabel(sidebar);
    m_statusLine->setObjectName("statusLine");
    m_statusLine->setWordWrap(true);
    m_statusLine->hide();

    auto *workspaceHeader = new QWidget;
    workspaceHeader->setObjectName(QStringLiteral("chatWorkspaceHeader"));
    workspaceHeader->setFixedHeight(kChatBarHeight);
    auto *workspaceName = new QPushButton(QStringLiteral("ForkMesh"));
    workspaceName->setObjectName(QStringLiteral("chatWorkspaceMenu"));
    workspaceName->setCursor(Qt::PointingHandCursor);
    workspaceName->setToolTip(QStringLiteral("Start a direct message"));
    workspaceName->setLayoutDirection(Qt::RightToLeft);
    setOcticon(workspaceName, QStringLiteral("chevron-down"), 14);
    connect(workspaceName, &QPushButton::clicked, this,
            &MainWindow::showNewMessageDialog);
    auto *composeButton = new QPushButton(QString());
    composeButton->setObjectName(QStringLiteral("chatComposeButton"));
    composeButton->setCursor(Qt::PointingHandCursor);
    composeButton->setToolTip(QStringLiteral("New message"));
    composeButton->setFixedSize(28, 28);
    setOcticon(composeButton, QStringLiteral("pencil"), 16);
    connect(composeButton, &QPushButton::clicked, this,
            &MainWindow::showNewMessageDialog);
    auto *workspaceLayout = new QHBoxLayout(workspaceHeader);
    workspaceLayout->setContentsMargins(8, 0, 10, 0);
    workspaceLayout->setSpacing(4);
    workspaceLayout->addWidget(workspaceName, 0, Qt::AlignVCenter);
    workspaceLayout->addStretch(1);
    workspaceLayout->addWidget(composeButton, 0, Qt::AlignVCenter);

    auto *userPanel = new QWidget;
    userPanel->setObjectName(QStringLiteral("chatUserPanel"));
    userPanel->setFixedHeight(kChatUserPanelHeight);
    m_chatWorkspaceAvatar = new QLabel;
    m_chatWorkspaceAvatar->setObjectName(QStringLiteral("chatUserAvatar"));
    m_chatWorkspaceAvatar->setFixedSize(kChatUserAvatarSide, kChatUserAvatarSide);
    m_chatWorkspaceUser = new QLabel;
    m_chatWorkspaceUser->setObjectName(QStringLiteral("chatWorkspaceUser"));
    m_chatWorkspaceUser->setTextFormat(Qt::RichText);
    m_chatWorkspaceUser->setMinimumWidth(0);
    m_chatWorkspaceUser->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);
    auto *userPanelLayout = new QHBoxLayout(userPanel);
    userPanelLayout->setContentsMargins(8, 0, 8, 0);
    userPanelLayout->setSpacing(8);
    userPanelLayout->addWidget(m_chatWorkspaceAvatar, 0, Qt::AlignVCenter);
    userPanelLayout->addWidget(m_chatWorkspaceUser, 1, Qt::AlignVCenter);

    m_chatAllUnreadButton = new QPushButton(QStringLiteral("All unread"));
    m_chatAllUnreadButton->setObjectName(QStringLiteral("chatNavButton"));
    m_chatAllUnreadButton->setCursor(Qt::PointingHandCursor);
    m_chatAllUnreadButton->setToolTip(
        QStringLiteral("Open the next conversation with unread messages"));
    setOcticon(m_chatAllUnreadButton, QStringLiteral("list-unordered"), 16);
    m_chatAllUnreadBadge = new QLabel(m_chatAllUnreadButton);
    m_chatAllUnreadBadge->setObjectName(QStringLiteral("chatNavBadge"));
    m_chatAllUnreadBadge->setAlignment(Qt::AlignCenter);
    m_chatAllUnreadBadge->hide();
    auto *allUnreadLayout = new QHBoxLayout(m_chatAllUnreadButton);
    allUnreadLayout->setContentsMargins(0, 0, 16, 0);
    allUnreadLayout->addStretch();
    allUnreadLayout->addWidget(m_chatAllUnreadBadge, 0, Qt::AlignVCenter);
    connect(m_chatAllUnreadButton, &QPushButton::clicked, this,
            &MainWindow::openNextUnreadConversation);

    auto *divider = new QFrame;
    divider->setObjectName(QStringLiteral("chatSidebarDivider"));
    divider->setFixedHeight(1);

    m_channelList = new ChannelGroupTree;
    m_channelGroups = ChannelGroups::fromJson(
        QSettings().value(kChannelGroupsSetting).toString());
    m_channelList->setMinimumHeight(0);
    m_channelList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_channelList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_channelList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_channelList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_channelList->setItemDelegate(new ChatRoomRowDelegate(m_channelList));

    m_dmList = new QListWidget;
    m_dmList->setObjectName("chatDmList");
    m_dmList->setMinimumHeight(0);
    m_dmList->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_dmList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_dmList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_dmList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_dmList->setItemDelegate(new ChatRoomRowDelegate(m_dmList));

    const auto fitToRows = [](QAbstractItemView *view) {
        QAbstractItemModel *model = view->model();
        int height = 0;
        if (auto *tree = qobject_cast<QTreeView *>(view)) {
            for (QModelIndex index = model->index(0, 0); index.isValid();
                 index = tree->indexBelow(index))
                height += qMax(1, view->sizeHintForIndex(index).height());
        } else {
            for (int row = 0; row < model->rowCount(); ++row)
                height += qMax(1, view->sizeHintForRow(row));
        }
        view->setFixedHeight(height + 2 * view->frameWidth());
    };
    for (QAbstractItemView *view :
         {static_cast<QAbstractItemView *>(m_channelList),
          static_cast<QAbstractItemView *>(m_dmList)}) {
        QAbstractItemModel *model = view->model();
        const auto fit = [view, fitToRows] { fitToRows(view); };
        connect(model, &QAbstractItemModel::rowsInserted, view, fit);
        connect(model, &QAbstractItemModel::rowsRemoved, view, fit);
        connect(model, &QAbstractItemModel::modelReset, view, fit);
        connect(model, &QAbstractItemModel::layoutChanged, view, fit);
        fit();
    }
    connect(m_channelList, &QTreeView::expanded, m_channelList,
            [this, fitToRows] { fitToRows(m_channelList); });
    connect(m_channelList, &QTreeView::collapsed, m_channelList,
            [this, fitToRows] { fitToRows(m_channelList); });

    const auto sectionHeader = [](const QString &title, QWidget *body,
                                  QPushButton *addButton) {
        auto *row = new QWidget;
        row->setObjectName(QStringLiteral("chatSectionHeader"));
        auto *toggle = new QPushButton(title);
        toggle->setObjectName(QStringLiteral("chatSectionToggle"));
        toggle->setCursor(Qt::PointingHandCursor);
        toggle->setCheckable(true);
        toggle->setChecked(true);
        toggle->setLayoutDirection(Qt::RightToLeft);
        const auto setChevron = [toggle](bool open) {
            setOcticon(toggle,
                       open ? QStringLiteral("chevron-down")
                            : QStringLiteral("chevron-right"),
                       10);
        };
        setChevron(true);
        connect(toggle, &QPushButton::toggled, body,
                [body, setChevron](bool open) {
                    body->setVisible(open);
                    setChevron(open);
                });
        addButton->setObjectName(QStringLiteral("chatSectionAddButton"));
        addButton->setCursor(Qt::PointingHandCursor);
        addButton->setFixedSize(20, 20);
        setOcticon(addButton, QStringLiteral("plus"), 14);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(4, 0, 10, 0);
        layout->setSpacing(0);
        layout->addWidget(toggle);
        layout->addStretch();
        layout->addWidget(addButton);
        return row;
    };

    addChannelButton = new QPushButton(QString());
    addChannelButton->setToolTip(
        QStringLiteral("Create a public channel, a private room that only "
                       "invited people can see, or a direct message"));
    auto *newMessageButton = new QPushButton(QString());
    newMessageButton->setToolTip(
        QStringLiteral("Start a private conversation with someone"));
    connect(newMessageButton, &QPushButton::clicked, this,
            &MainWindow::showNewMessageDialog);

    auto *roomsScroll = new QScrollArea;
    roomsScroll->setObjectName("chatSidebarScroll");
    roomsScroll->setWidgetResizable(true);
    roomsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    roomsScroll->setFrameShape(QFrame::NoFrame);
    roomsScroll->setMinimumHeight(0);
    roomsScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    auto *roomsContainer = new QWidget;
    roomsContainer->setObjectName(QStringLiteral("chatSidebarRooms"));
    auto *roomsLayout = new QVBoxLayout(roomsContainer);
    roomsLayout->setContentsMargins(0, 8, 0, 12);
    roomsLayout->setSpacing(0);
    roomsLayout->addWidget(m_chatAllUnreadButton);
    roomsLayout->addSpacing(8);
    roomsLayout->addWidget(divider);
    roomsLayout->addSpacing(12);
    roomsLayout->addWidget(
        sectionHeader(QStringLiteral("Text Channels"), m_channelList, addChannelButton));
    roomsLayout->addWidget(m_channelList);
    roomsLayout->addSpacing(16);
    roomsLayout->addWidget(sectionHeader(QStringLiteral("Direct Messages"),
                                         m_dmList, newMessageButton));
    roomsLayout->addWidget(m_dmList);
    roomsLayout->addStretch();
    roomsScroll->setWidget(roomsContainer);

    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);
    sidebarLayout->addWidget(workspaceHeader);
    sidebarLayout->addWidget(roomsScroll, 1);
    sidebarLayout->addWidget(userPanel);
    refreshChatWorkspaceUser();
    return sidebar;
}

// The conversation header (title, settings, invite, members, seal) and the
// firewall and unread banners under it.
QWidget *MainWindow::buildChatHeader()
{
    auto *header = new QWidget;
    header->setObjectName("chatHeader");
    m_channelTitle = new QLabel(QStringLiteral("general"));
    m_channelTitle->setObjectName("channelTitle");
    // #general begins as an explicitly public channel. Conversation switches
    // update both this transport icon and the adjacent access-policy badge.
    m_directRouteLabel = new QLabel;
    m_directRouteLabel->setObjectName(QStringLiteral("directRouteLabel"));
    m_directRouteLabel->setTextFormat(Qt::PlainText);
    m_directRouteLabel->setStyleSheet(QStringLiteral("color: #8b949e; font-size: 11px;"));
    m_directRouteLabel->setToolTip(QStringLiteral(
        "Messages reach the other person's linked device directly over the local "
        "network or Bluetooth"));
    m_directRouteLabel->hide();
    m_serverCopiesLabel = new QLabel;
    m_serverCopiesLabel->setObjectName("serverCopiesLabel");
    m_serverCopiesLabel->setTextFormat(Qt::PlainText);
    m_serverCopiesLabel->hide();
    m_encryptionLabel = new QLabel;
    m_encryptionLabel->setObjectName("encryptionLabel");
    m_encryptionLabel->setPixmap(
        tintedOcticonPixmap("unlock", Theme::remapColor(QColor("#8b949e")), 16));
    m_encryptionLabel->setToolTip(
        "Public channel; the controller can read it for integrations");
    m_roomSealButton = new QPushButton(QStringLiteral("Public"));
    m_roomSealButton->setObjectName("ghostButton");
    m_roomSealButton->setProperty("buttonSize", "sm");
    m_roomSealButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_roomSealButton, "unlock", 14);
    connect(m_roomSealButton, &QPushButton::clicked, this,
            &MainWindow::promptRoomSeal);
    // Channel settings belong beside the channel they change, not in a menu
    // three clicks away: name, topic, visibility, webhooks, and deletion.
    m_channelSettingsButton = new QPushButton(QString());
    m_channelSettingsButton->setObjectName(QStringLiteral("chatHeaderIconButton"));
    m_channelSettingsButton->setCursor(Qt::PointingHandCursor);
    m_channelSettingsButton->setToolTip(
        QStringLiteral("This channel's details and settings: name, topic, "
                       "visibility, webhooks"));
    m_channelSettingsButton->setFixedSize(30, 30);
    setOcticon(m_channelSettingsButton, "info", 18);
    m_channelSettingsButton->hide();
    connect(m_channelSettingsButton, &QPushButton::clicked, this,
            &MainWindow::openChannelSettings);
    m_meshDebugToggleButton = new QPushButton(QString());
    m_meshDebugToggleButton->setObjectName(QStringLiteral("chatHeaderIconButton"));
    m_meshDebugToggleButton->setCursor(Qt::PointingHandCursor);
    m_meshDebugToggleButton->setToolTip(
        QStringLiteral("Show or hide the mesh diagnostics"));
    m_meshDebugToggleButton->setCheckable(true);
    m_meshDebugToggleButton->setFixedSize(30, 30);
    setOcticon(m_meshDebugToggleButton, "pulse", 18);
    m_meshDebugToggleButton->hide();
    connect(m_meshDebugToggleButton, &QPushButton::toggled, this,
            [this](bool checked) {
                m_meshDebugVisible = checked;
                refreshMeshDebugBar();
            });
    m_inviteButton = new QPushButton(QStringLiteral("Invite"));
    m_inviteButton->setObjectName("ghostButton");
    m_inviteButton->setCursor(Qt::PointingHandCursor);
    m_inviteButton->setToolTip(QStringLiteral("Invite a member to this private room"));
    m_inviteButton->hide();
    connect(m_inviteButton, &QPushButton::clicked, this,
            &MainWindow::promptInviteToPrivateChannel);
    const auto makeCallButton = [this](const QString &media, const QString &label,
                                       const QString &tip) {
        auto *button = new QPushButton(label);
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setProperty("buttonSize", "sm");
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(tip);
        button->hide();
        connect(button, &QPushButton::clicked, this,
                [this, media] { startDirectCallOnWeb(media); });
        return button;
    };
    m_callAudioButton = makeCallButton(
        QStringLiteral("audio"), QStringLiteral("Voice call"),
        QStringLiteral("Start an encrypted peer-to-peer voice call in your browser"));
    m_callVideoButton = makeCallButton(
        QStringLiteral("video"), QStringLiteral("Video call"),
        QStringLiteral("Start an encrypted peer-to-peer video call in your browser; "
                       "you can pick a different face for the other person to see"));
    m_chatMembersButton = new QPushButton(QStringLiteral("0"));
    m_chatMembersButton->setObjectName(QStringLiteral("chatHeaderMeta"));
    m_chatMembersButton->setProperty("buttonSize", "sm");
    m_chatMembersButton->setCursor(Qt::PointingHandCursor);
    m_chatMembersButton->setToolTip(
        QStringLiteral("Show users in this room"));
    setOcticon(m_chatMembersButton, "people", 15);
    m_channelTopicLabel = new QLabel;
    m_channelTopicLabel->setObjectName(QStringLiteral("channelTopic"));
    m_channelTopicLabel->setTextFormat(Qt::PlainText);
    m_channelTopicLabel->setMinimumWidth(0);
    m_channelTopicLabel->setSizePolicy(QSizePolicy::Ignored,
                                       QSizePolicy::Preferred);
    m_channelTopicLabel->hide();
    header->setFixedHeight(kChatBarHeight);
    auto *hashGlyph = new QLabel(QStringLiteral("#"));
    hashGlyph->setObjectName(QStringLiteral("channelHash"));
    auto *topicDivider = new QFrame;
    topicDivider->setObjectName(QStringLiteral("channelTopicDivider"));
    topicDivider->setFixedSize(1, 24);
    m_chatMembersButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    m_chatMembersButton->setCheckable(true);
    m_chatMembersButton->setChecked(m_chatMembersPanelWanted);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(16, 0, 16, 0);
    headerLayout->setSpacing(8);
    auto *titleBox = new QWidget;
    auto *titleBoxLayout = new QVBoxLayout(titleBox);
    titleBoxLayout->setContentsMargins(0, 0, 0, 0);
    titleBoxLayout->setSpacing(0);
    titleBoxLayout->addWidget(m_channelTitle);
    titleBoxLayout->addWidget(m_directRouteLabel);
    headerLayout->addWidget(hashGlyph, 0, Qt::AlignVCenter);
    headerLayout->addWidget(titleBox, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_serverCopiesLabel, 0, Qt::AlignVCenter);
    headerLayout->addSpacing(4);
    headerLayout->addWidget(topicDivider, 0, Qt::AlignVCenter);
    headerLayout->addSpacing(4);
    headerLayout->addWidget(m_channelTopicLabel, 1, Qt::AlignVCenter);
    headerLayout->addStretch();
    headerLayout->addWidget(m_callAudioButton, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_callVideoButton, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_inviteButton, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_roomSealButton, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_encryptionLabel, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_meshDebugToggleButton, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_channelSettingsButton, 0, Qt::AlignVCenter);
    headerLayout->addWidget(m_chatMembersButton, 0, Qt::AlignVCenter);
    topicDivider->hide();

    m_firewallBanner = new QWidget;
    m_firewallBanner->setObjectName("firewallBanner");
    m_firewallBannerLabel = new QLabel;
    m_firewallBannerLabel->setObjectName("firewallBannerLabel");
    m_firewallBannerLabel->setWordWrap(true);
    m_firewallAllowButton = new QPushButton("Allow through firewall");
    m_firewallAllowButton->setObjectName("primaryButton");
    m_firewallAllowButton->setCursor(Qt::PointingHandCursor);
    auto *firewallDismiss = new QPushButton(QString());
    firewallDismiss->setObjectName("ghostButton");
    firewallDismiss->setCursor(Qt::PointingHandCursor);
    firewallDismiss->setToolTip("Dismiss");
    setOcticon(firewallDismiss, "x", 16);
    auto *firewallLayout = new QHBoxLayout(m_firewallBanner);
    firewallLayout->setContentsMargins(16, 10, 12, 10);
    firewallLayout->setSpacing(10);
    firewallLayout->addWidget(m_firewallBannerLabel, 1);
    firewallLayout->addWidget(m_firewallAllowButton);
    firewallLayout->addWidget(firewallDismiss);
    m_firewallBanner->hide();
    connect(m_firewallAllowButton, &QPushButton::clicked, this,
            &MainWindow::allowFirewall);
    connect(firewallDismiss, &QPushButton::clicked, m_firewallBanner,
            &QWidget::hide);

    m_devicePendingBanner = new QWidget;
    m_devicePendingBanner->setObjectName(QStringLiteral("firewallBanner"));
    m_devicePendingLabel = new QLabel;
    m_devicePendingLabel->setObjectName(QStringLiteral("firewallBannerLabel"));
    m_devicePendingLabel->setWordWrap(true);
    auto *approveDevice = new QPushButton(QStringLiteral("Approve"));
    approveDevice->setObjectName(QStringLiteral("primaryButton"));
    approveDevice->setCursor(Qt::PointingHandCursor);
    auto *blockDevice = new QPushButton(QStringLiteral("Block"));
    blockDevice->setObjectName(QStringLiteral("ghostButton"));
    blockDevice->setCursor(Qt::PointingHandCursor);
    auto *safetyButton = new QPushButton(QStringLiteral("Safety number"));
    safetyButton->setObjectName(QStringLiteral("ghostButton"));
    safetyButton->setCursor(Qt::PointingHandCursor);
    auto *pendingLayout = new QHBoxLayout(m_devicePendingBanner);
    pendingLayout->setContentsMargins(16, 10, 12, 10);
    pendingLayout->setSpacing(10);
    pendingLayout->addWidget(m_devicePendingLabel, 1);
    pendingLayout->addWidget(safetyButton);
    pendingLayout->addWidget(blockDevice);
    pendingLayout->addWidget(approveDevice);
    m_devicePendingBanner->hide();
    connect(approveDevice, &QPushButton::clicked, this, [this] {
        if (m_officeChannelMirror && !m_devicePendingKid.isEmpty())
            m_officeChannelMirror->approveDirectDevice(m_currentConversation,
                                                       m_devicePendingKid);
        refreshDevicePendingBanner();
    });
    connect(blockDevice, &QPushButton::clicked, this, [this] {
        if (m_officeChannelMirror && !m_devicePendingKid.isEmpty())
            m_officeChannelMirror->blockDirectDevice(m_currentConversation,
                                                     m_devicePendingKid);
        refreshDevicePendingBanner();
    });
    connect(safetyButton, &QPushButton::clicked, this,
            [this] { showDirectDetails(m_currentConversation); });

    m_meshDebugBar = new QWidget;
    m_meshDebugBar->setObjectName("meshDebugBar");
    m_meshDebugLabel = new QLabel;
    m_meshDebugLabel->setObjectName("meshDebugLabel");
    m_meshDebugLabel->setWordWrap(true);
    m_meshDebugLabel->setTextFormat(Qt::PlainText);
    m_meshDebugLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_meshDebugLabel->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont));
    auto *meshDebugCopy = new QPushButton(QStringLiteral("copy"));
    meshDebugCopy->setObjectName("meshDebugCopy");
    meshDebugCopy->setCursor(Qt::PointingHandCursor);
    meshDebugCopy->setToolTip(
        QStringLiteral("Copy the mesh diagnostics, with one row per device"));
    auto *meshDebugLayout = new QHBoxLayout(m_meshDebugBar);
    meshDebugLayout->setContentsMargins(16, 2, 8, 2);
    meshDebugLayout->setSpacing(6);
    meshDebugLayout->addWidget(m_meshDebugLabel, 1);
    meshDebugLayout->addWidget(meshDebugCopy, 0, Qt::AlignTop);
    m_meshDebugBar->hide();
    connect(meshDebugCopy, &QPushButton::clicked, this,
            &MainWindow::copyMeshDiagnostics);
    m_meshDebugTimer = new QTimer(this);
    m_meshDebugTimer->setInterval(1000);
    connect(m_meshDebugTimer, &QTimer::timeout, this,
            &MainWindow::refreshMeshDebugBar);

    m_chatUnreadBanner = new QWidget;
    m_chatUnreadBanner->setObjectName("chatUnreadBanner");
    m_chatUnreadBannerLabel = new QLabel;
    m_chatUnreadBannerLabel->setMinimumWidth(0);
    m_chatUnreadBannerLabel->setSizePolicy(QSizePolicy::Ignored,
                                           QSizePolicy::Preferred);
    auto *unreadReadButton = new QPushButton(QStringLiteral("Mark all read"));
    unreadReadButton->setObjectName("chatUnreadBannerButton");
    unreadReadButton->setCursor(Qt::PointingHandCursor);
    unreadReadButton->setToolTip(
        QStringLiteral("Mark every conversation read and jump to the newest messages"));
    setOcticon(unreadReadButton, "chevron-up", 16);
    m_chatUnreadJumpButton = new QPushButton(QStringLiteral("Open"));
    m_chatUnreadJumpButton->setObjectName("chatUnreadBannerButton");
    m_chatUnreadJumpButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_chatUnreadJumpButton, "arrow-left", 16, 180);
    m_chatUnreadJumpButton->setLayoutDirection(Qt::RightToLeft);
    auto *unreadLayout = new QHBoxLayout(m_chatUnreadBanner);
    unreadLayout->setContentsMargins(18, 6, 12, 6);
    unreadLayout->setSpacing(10);
    unreadLayout->addWidget(m_chatUnreadBannerLabel, 1);
    unreadLayout->addWidget(m_chatUnreadJumpButton);
    unreadLayout->addWidget(unreadReadButton);
    connect(m_chatUnreadJumpButton, &QPushButton::clicked, this,
            &MainWindow::openNextUnreadConversation);
    m_chatUnreadBanner->hide();
    connect(unreadReadButton, &QPushButton::clicked, this, &MainWindow::markAllChatRead);
    return header;
}

QWidget *MainWindow::buildChatMainColumn(QWidget *header, QPushButton *&sendButton)
{
    m_messageScroll = new QScrollArea;
    m_messageScroll->setObjectName("messageView");
    m_messageScroll->setWidgetResizable(true);
    m_messageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messageScroll->setMinimumHeight(0);
    m_messageScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_messageContainer = new QWidget;
    m_messageContainer->setObjectName("messageContainer");
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(4, 8, 4, 8);
    m_messageLayout->setSpacing(0);
    m_messageLayout->addStretch();
    m_messageScroll->setWidget(m_messageContainer);

    QScrollBar *vbar = m_messageScroll->verticalScrollBar();
    connect(vbar, &QScrollBar::rangeChanged, this, [this](int, int max) {
        if (m_transcriptKeepFromBottom >= 0)
            m_messageScroll->verticalScrollBar()->setValue(
                max - m_transcriptKeepFromBottom);
        else if (m_stickToBottom)
            m_messageScroll->verticalScrollBar()->setValue(max);
    });
    connect(vbar, &QScrollBar::actionTriggered, this,
            [this](int) { m_transcriptKeepFromBottom = -1; });
    connect(vbar, &QScrollBar::valueChanged, this, [this](int value) {
        QScrollBar *bar = m_messageScroll->verticalScrollBar();
        m_stickToBottom = value >= bar->maximum() - 4;
    });

    auto *composer = new QWidget;
    composer->setObjectName("composerBar");
    composer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *inputRow = new QWidget;
    inputRow->setObjectName("composerInputRow");
    inputRow->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *attachButton = new QPushButton(QString());
    attachButton->setObjectName("iconButton");
    attachButton->setCursor(Qt::PointingHandCursor);
    attachButton->setToolTip("Share a file (any type, including GIFs)");
    setOcticon(attachButton, "paperclip", 18);
    connect(attachButton, &QPushButton::clicked, this, &MainWindow::attachFile);
    m_messageInput = new QLineEdit;
    m_messageInput->setObjectName("messageInput");
    m_messageInput->setPlaceholderText("Message");
    m_messageInput->setToolTip(QStringLiteral(
        "Type / to see available chat commands\n"
        "*bold*  _italic_  ~strike~  `code`  > quote  - list  <https://url|text>\n"
        "Ctrl+B bold, Ctrl+I italic, Ctrl+Shift+X strike, Ctrl+Shift+C code, "
        "Ctrl+Shift+U link"));
    m_messageInput->setMaxLength(16000);
    m_messageInput->setMinimumWidth(0);
    m_messageInput->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    m_messageInput->installEventFilter(this);

    m_mentionModel = new QStringListModel(this);
    m_mentionCompleter = new QCompleter(m_mentionModel, this);
    m_mentionCompleter->setWidget(m_messageInput);
    m_mentionCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_mentionCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_mentionCompleter->setFilterMode(Qt::MatchContains);
    connect(m_mentionCompleter,
            QOverload<const QString &>::of(&QCompleter::activated), this,
            &MainWindow::insertComposerCompletion);
    QAbstractItemView *mentionPopup = m_mentionCompleter->popup();
    mentionPopup->installEventFilter(this);
    m_mentionCompleterPopup = mentionPopup;
    refreshMentionCandidates();
    auto *emojiButton = new QPushButton(QString::fromUtf8("\xF0\x9F\x99\x82"));
    emojiButton->setObjectName("iconButton");
    emojiButton->setCursor(Qt::PointingHandCursor);
    emojiButton->setToolTip("Insert emoji");
    connect(emojiButton, &QPushButton::clicked, this,
            [this, emojiButton] { showEmojiPicker(emojiButton); });
    // Links live inside the sentence, so the composer needs a way to write one
    // without remembering the [label](url) syntax — the same affordance the
    // web chat's formatting bar offers.
    auto *linkButton = new QPushButton(QString());
    linkButton->setObjectName("iconButton");
    linkButton->setCursor(Qt::PointingHandCursor);
    linkButton->setToolTip(QStringLiteral("Insert link: [text](https://...)"));
    setOcticon(linkButton, "link", 18);
    connect(linkButton, &QPushButton::clicked, this,
            &MainWindow::insertLinkIntoComposer);
    const std::pair<QKeySequence, std::pair<QString, QString>> formatShortcuts[] = {
        {QKeySequence(Qt::CTRL | Qt::Key_B), {QStringLiteral("*"), QStringLiteral("*")}},
        {QKeySequence(Qt::CTRL | Qt::Key_I), {QStringLiteral("_"), QStringLiteral("_")}},
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_X),
         {QStringLiteral("~"), QStringLiteral("~")}},
        {QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_C),
         {QStringLiteral("`"), QStringLiteral("`")}},
    };
    for (const auto &[keys, markers] : formatShortcuts) {
        auto *shortcut = new QShortcut(keys, m_messageInput);
        shortcut->setContext(Qt::WidgetShortcut);
        connect(shortcut, &QShortcut::activated, this,
                [this, before = markers.first, after = markers.second] {
                    wrapComposerSelection(before, after);
                });
    }
    auto *linkShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_U), m_messageInput);
    linkShortcut->setContext(Qt::WidgetShortcut);
    connect(linkShortcut, &QShortcut::activated, this,
            &MainWindow::insertLinkIntoComposer);
    auto *mentionButton = new QPushButton(QStringLiteral("@"));
    mentionButton->setToolTip(QStringLiteral("Mention someone"));
    connect(mentionButton, &QPushButton::clicked, this, [this] {
        m_messageInput->insert(QStringLiteral("@"));
        m_messageInput->setFocus();
    });
    for (QPushButton *button :
         {attachButton, linkButton, mentionButton, emojiButton}) {
        button->setObjectName(QStringLiteral("composerToolButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFocusPolicy(Qt::NoFocus);
        button->setFixedSize(32, 32);
    }
    attachButton->setObjectName(QStringLiteral("composerAttachButton"));
    attachButton->setFixedSize(24, 24);
    setOcticon(attachButton, "plus", 16);
    setOcticon(linkButton, "link", 20);
    sendButton = new QPushButton(QString());
    sendButton->setObjectName(QStringLiteral("composerSendButton"));
    sendButton->setCursor(Qt::PointingHandCursor);
    sendButton->setToolTip(QStringLiteral("Send (Enter)"));
    sendButton->setFocusPolicy(Qt::NoFocus);
    sendButton->setFixedSize(32, 32);
    setOcticon(sendButton, "paper-airplane", 18);
    auto *inputRowLayout = new QHBoxLayout(inputRow);
    inputRowLayout->setContentsMargins(16, 6, 10, 6);
    inputRowLayout->setSpacing(4);
    inputRowLayout->addWidget(attachButton, 0, Qt::AlignVCenter);
    inputRowLayout->addSpacing(8);
    inputRowLayout->addWidget(m_messageInput, 1, Qt::AlignVCenter);
    for (QPushButton *button : {linkButton, mentionButton, emojiButton, sendButton})
        inputRowLayout->addWidget(button, 0, Qt::AlignVCenter);
    auto *composerLayout = new QHBoxLayout(composer);
    composerLayout->setContentsMargins(16, 0, 16, 20);
    composerLayout->setSpacing(8);
    composerLayout->addWidget(inputRow, 1);

    m_typingLabel = new QLabel;
    m_typingLabel->setObjectName("typingLabel");
    m_typingLabel->setFixedHeight(20);
    m_typingLabel->setText(QString());

    auto *mainColumnHost = new QWidget;
    mainColumnHost->setObjectName("chatMainColumn");
    mainColumnHost->setMinimumHeight(0);
    mainColumnHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    auto *mainColumn = new QVBoxLayout(mainColumnHost);
    mainColumn->setContentsMargins(0, 0, 0, 0);
    mainColumn->setSpacing(0);
    mainColumn->addWidget(header);
    mainColumn->addWidget(m_firewallBanner);
    mainColumn->addWidget(m_devicePendingBanner);
    mainColumn->addWidget(m_meshDebugBar);
    mainColumn->addWidget(m_chatUnreadBanner);
    m_chatBody = new QWidget;
    m_chatBody->setObjectName(QStringLiteral("chatBody"));
    m_chatBody->setMinimumHeight(0);
    m_chatBody->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    m_chatBody->installEventFilter(this);
    auto *transcriptColumn = new QVBoxLayout;
    transcriptColumn->setContentsMargins(0, 0, 0, 0);
    transcriptColumn->setSpacing(0);
    transcriptColumn->addWidget(m_messageScroll, 1);
    transcriptColumn->addWidget(m_typingLabel);
    transcriptColumn->addWidget(composer);
    auto *bodyLayout = new QHBoxLayout(m_chatBody);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);
    bodyLayout->addLayout(transcriptColumn, 1);
    bodyLayout->addWidget(m_chatMembersPanel);
    mainColumn->addWidget(m_chatBody, 1);
    return mainColumnHost;
}

void MainWindow::buildChatThreadPane()
{
    m_chatThreadPane = new QWidget;
    m_chatThreadPane->setObjectName(QStringLiteral("chatThreadPane"));
    m_chatThreadPane->setMinimumWidth(kChatThreadPaneMinWidth);
    m_chatThreadPane->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *threadLayout = new QVBoxLayout(m_chatThreadPane);
    threadLayout->setContentsMargins(0, 0, 0, 0);
    threadLayout->setSpacing(0);

    auto *threadHeader = new QWidget;
    threadHeader->setObjectName(QStringLiteral("chatThreadHeader"));
    auto *threadHeaderLayout = new QHBoxLayout(threadHeader);
    threadHeaderLayout->setContentsMargins(16, 12, 12, 12);
    threadHeaderLayout->setSpacing(8);
    m_chatThreadTitle = new QLabel(QStringLiteral("Thread"));
    m_chatThreadTitle->setObjectName(QStringLiteral("chatThreadTitle"));
    m_chatThreadCountLabel = new QLabel;
    m_chatThreadCountLabel->setObjectName(QStringLiteral("threadCountLabel"));
    auto *closeThread = new QPushButton(QString());
    closeThread->setObjectName(QStringLiteral("ghostButton"));
    closeThread->setCursor(Qt::PointingHandCursor);
    closeThread->setToolTip(QStringLiteral("Close thread"));
    setOcticon(closeThread, "x", 16);
    threadHeaderLayout->addWidget(m_chatThreadTitle);
    threadHeaderLayout->addWidget(m_chatThreadCountLabel);
    threadHeaderLayout->addStretch();
    threadHeaderLayout->addWidget(closeThread);
    threadLayout->addWidget(threadHeader);

    auto *threadScroll = new QScrollArea;
    threadScroll->setObjectName(QStringLiteral("chatThreadView"));
    threadScroll->setWidgetResizable(true);
    threadScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *threadRows = new QWidget;
    m_chatThreadRowsLayout = new QVBoxLayout(threadRows);
    m_chatThreadRowsLayout->setContentsMargins(4, 8, 4, 8);
    m_chatThreadRowsLayout->setSpacing(2);
    m_chatThreadRowsLayout->addStretch();
    threadScroll->setWidget(threadRows);
    threadLayout->addWidget(threadScroll, 1);

    auto *threadComposer = new QWidget;
    threadComposer->setObjectName(QStringLiteral("chatThreadComposer"));
    auto *threadComposerLayout = new QVBoxLayout(threadComposer);
    threadComposerLayout->setContentsMargins(12, 10, 12, 12);
    threadComposerLayout->setSpacing(8);
    m_chatThreadInput = new QPlainTextEdit;
    m_chatThreadInput->setMaximumBlockCount(20);
    m_chatThreadInput->setFixedHeight(88);
    (new PromptSpellCheckHighlighter(m_chatThreadInput->document()))
        ->attachTo(m_chatThreadInput);
    auto *threadSend = new QPushButton(QStringLiteral("Reply"));
    threadSend->setObjectName(QStringLiteral("primaryButton"));
    threadSend->setCursor(Qt::PointingHandCursor);
    threadComposerLayout->addWidget(m_chatThreadInput);
    threadComposerLayout->addWidget(threadSend, 0, Qt::AlignRight);
    threadLayout->addWidget(threadComposer);
    connect(closeThread, &QPushButton::clicked, this, &MainWindow::closeChatThread);
    connect(threadSend, &QPushButton::clicked, this,
            &MainWindow::sendChatThreadReply);
}

void MainWindow::buildChatMembersPanel()
{
    m_chatMembersPanel = new QWidget;
    m_chatMembersPanel->setObjectName(QStringLiteral("chatMembersPanel"));
    m_chatMembersPanel->setFixedWidth(kChatMembersPanelWidth);
    m_chatMembersPanel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    m_chatMembersHeading =
        new QLabel("DATABASE USERS IN THIS ROOM \xE2\x80\x94 0");
    m_chatMembersHeading->setObjectName(QStringLiteral("chatMembersHeading"));
    m_chatMembersHeading->setWordWrap(true);
    auto *membersScroll = new QScrollArea;
    membersScroll->setObjectName(QStringLiteral("chatMembersScroll"));
    membersScroll->setWidgetResizable(true);
    membersScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    membersScroll->setFrameShape(QFrame::NoFrame);
    auto *membersContainer = new QWidget;
    membersContainer->setObjectName(QStringLiteral("chatMembersList"));
    m_chatMembersLayout = new QVBoxLayout(membersContainer);
    m_chatMembersLayout->setContentsMargins(8, 0, 8, 12);
    m_chatMembersLayout->setSpacing(2);
    m_chatMembersLayout->addStretch();
    membersScroll->setWidget(membersContainer);
    auto *membersLayout = new QVBoxLayout(m_chatMembersPanel);
    membersLayout->setContentsMargins(0, 16, 0, 0);
    membersLayout->setSpacing(4);
    membersLayout->addWidget(m_chatMembersHeading);
    membersLayout->addWidget(membersScroll, 1);
    m_chatMembersPanelWanted =
        QSettings().value(kChatMembersPanelSetting, true).toBool();
    connect(m_chatMembersButton, &QPushButton::clicked, this,
            [this](bool checked) {
                m_chatMembersPanelWanted = checked;
                QSettings().setValue(kChatMembersPanelSetting, checked);
                syncChatMembersPanel();
            });
}

void MainWindow::syncChatMembersPanel()
{
    if (!m_chatMembersPanel || !m_chatBody)
        return;
    if (m_chatMembersButton &&
        m_chatMembersButton->isChecked() != m_chatMembersPanelWanted) {
        const QSignalBlocker blocker(m_chatMembersButton);
        m_chatMembersButton->setChecked(m_chatMembersPanelWanted);
    }
    const bool shown = m_chatMembersPanelWanted &&
                       m_chatBody->width() >= kChatMembersPanelMinBodyWidth;
    if (shown == m_chatMembersMenuOpen && shown == !m_chatMembersPanel->isHidden())
        return;
    m_chatMembersMenuOpen = shown;
    m_chatMembersPanel->setVisible(shown);
    if (shown)
        refreshChatMembers();
}

void MainWindow::connectChatSectionSignals(QPushButton *addChannelButton,
                                           QPushButton *sendButton)
{
    connect(m_channelList, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
                // Group headers are enabled (they take clicks and drops) but
                // carry no conversation; only channel rows switch.
                const QString channel = ChannelGroupTree::channelOf(item);
                if (!channel.isEmpty())
                    switchConversation(channel);
            });
    connect(m_channelList, &QTreeWidget::itemClicked, this,
            [this](QTreeWidgetItem *item, int column) {
                if (ChannelGroupTree::isGroupItem(item)) {
                    item->setExpanded(!item->isExpanded());
                    return;
                }
                const QString channel = ChannelGroupTree::channelOf(item);
                if (column == ChannelGroupTree::kSettingsColumn &&
                    !channel.isEmpty())
                    openChannelSettingsFor(channel);
            });
    connect(m_channelList, &QTreeWidget::itemExpanded, this,
            [this](QTreeWidgetItem *item) {
                if (ChannelGroupTree::isGroupItem(item))
                    setChannelGroupCollapsed(
                        ChannelGroupTree::groupNameOf(item), false);
            });
    connect(m_channelList, &QTreeWidget::itemCollapsed, this,
            [this](QTreeWidgetItem *item) {
                if (ChannelGroupTree::isGroupItem(item))
                    setChannelGroupCollapsed(
                        ChannelGroupTree::groupNameOf(item), true);
            });
    m_channelList->onMoveChannel = [this](const QString &channel,
                                          const QString &group, int index) {
        moveChannelToGroup(channel, group, index);
    };
    m_channelList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_channelList, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                QTreeWidgetItem *item = m_channelList->itemAt(pos);
                const QPoint global =
                    m_channelList->viewport()->mapToGlobal(pos);
                QMenu menu(m_channelList);
                if (!item) {
                    QAction *newGroup =
                        menu.addAction(QStringLiteral("New group…"));
                    if (menu.exec(global) == newGroup)
                        promptAddChannelGroup();
                    return;
                }
                if (ChannelGroupTree::isGroupItem(item)) {
                    const QString group = ChannelGroupTree::groupNameOf(item);
                    QAction *toggle = menu.addAction(
                        item->isExpanded() ? QStringLiteral("Collapse group")
                                           : QStringLiteral("Expand group"));
                    QAction *rename =
                        menu.addAction(QStringLiteral("Rename group…"));
                    QAction *remove =
                        menu.addAction(QStringLiteral("Delete group"));
                    remove->setToolTip(QStringLiteral(
                        "The channels stay in the sidebar, ungrouped"));
                    QAction *selected = menu.exec(global);
                    if (selected == toggle)
                        item->setExpanded(!item->isExpanded());
                    else if (selected == rename)
                        promptRenameChannelGroup(group);
                    else if (selected == remove)
                        removeChannelGroup(group);
                    return;
                }
                const QString channel = ChannelGroupTree::channelOf(item);
                const QString currentGroup =
                    ChannelGroups::renderedGroupOf(m_chatSidebarRows, channel);
                QMenu *moveMenu = menu.addMenu(QStringLiteral("Move to group"));
                QHash<QAction *, QString> targets;
                const QStringList groups =
                    ChannelGroups::renderedGroupNames(m_chatSidebarRows);
                for (const QString &group : groups) {
                    QAction *action = moveMenu->addAction(group);
                    action->setCheckable(true);
                    action->setChecked(group == currentGroup);
                    targets.insert(action, group);
                }
                if (!groups.isEmpty())
                    moveMenu->addSeparator();
                QAction *newGroup =
                    moveMenu->addAction(QStringLiteral("New group…"));
                QAction *ungroup =
                    currentGroup.isEmpty()
                        ? nullptr
                        : moveMenu->addAction(
                              QStringLiteral("Remove from group"));
                addChatRetentionMenu(&menu, channel);
                menu.addSeparator();
                QAction *open = nullptr;
                QAction *refresh = nullptr;
                QAction *settings = nullptr;
                QAction *del = nullptr;
                if (isDiscordConversation(channel)) {
                    open = menu.addAction(QStringLiteral("Open in Discord"));
                    refresh = menu.addAction(QStringLiteral("Refresh history"));
                    menu.addSeparator();
                    settings = menu.addAction(
                        QStringLiteral("Discord connection settings…"));
                } else {
                    settings =
                        menu.addAction(QStringLiteral("Channel settings…"));
                    del = menu.addAction(QStringLiteral("Delete room"));
                }
                QAction *selected = menu.exec(global);
                if (!selected)
                    return;
                if (targets.contains(selected)) {
                    moveChannelToGroup(channel, targets.value(selected));
                } else if (selected == newGroup) {
                    const QString name = promptChannelGroupName(
                        QStringLiteral("New group"), QString());
                    if (!name.isEmpty())
                        moveChannelToGroup(channel, name);
                } else if (ungroup && selected == ungroup) {
                    moveChannelToGroup(channel, QString());
                } else if (open && selected == open && m_discordClient) {
                    QDesktopServices::openUrl(
                        m_discordClient->browserUrl(channel));
                } else if (refresh && selected == refresh && m_discordClient) {
                    m_discordClient->openConversation(channel, true);
                } else if (settings && selected == settings) {
                    openChannelSettingsFor(channel);
                } else if (del && selected == del) {
                    promptDeleteRoom(channel);
                }
            });
    connect(m_dmList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item, QListWidgetItem *) {
                if (item)
                    switchConversation(item->data(Qt::UserRole).toString());
            });
    m_dmList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_dmList, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &pos) {
                QListWidgetItem *item = m_dmList->itemAt(pos);
                const QString conversation =
                    item ? item->data(Qt::UserRole).toString() : QString();
                if (conversation.isEmpty())
                    return;
                QMenu menu(m_dmList);
                addChatRetentionMenu(&menu, conversation);
                menu.exec(m_dmList->viewport()->mapToGlobal(pos));
            });
    connect(addChannelButton, &QPushButton::clicked, this, [this, addChannelButton] {
        QMenu menu(this);
        connect(menu.addAction(QStringLiteral("New channel (public)")),
                &QAction::triggered, this, &MainWindow::promptAddChannel);
        connect(menu.addAction(QString::fromUtf8(
                    "New private room (only people you invite)\xE2\x80\xA6")),
                &QAction::triggered, this, &MainWindow::promptAddPrivateChannel);
        connect(menu.addAction(QString::fromUtf8("New message\xE2\x80\xA6")),
                &QAction::triggered, this, &MainWindow::showNewMessageDialog);
        menu.addSeparator();
        connect(menu.addAction(QStringLiteral("New group…")),
                &QAction::triggered, this, &MainWindow::promptAddChannelGroup);
        menu.exec(addChannelButton->mapToGlobal(
            QPoint(0, addChannelButton->height())));
    });
    connect(m_messageInput, &QLineEdit::textEdited, this, &MainWindow::onComposerEdited);
    connect(m_messageInput, &QLineEdit::cursorPositionChanged, this,
            [this] { updateMentionPopup(); });
    connect(m_messageInput, &QLineEdit::returnPressed, this, &MainWindow::sendCurrentMessage);
    connect(sendButton, &QPushButton::clicked, this, &MainWindow::sendCurrentMessage);
}

void MainWindow::updateHomeStats()
{
    if (m_connectedAtMs > 0) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - m_uptimePersistedAtMs >= 5 * 60 * 1000) {
            m_uptimePersistedAtMs = now;
            const qint64 totalMs = m_totalConnectionMs + (now - m_connectedAtMs);
            QSettings().setValue(kConnectionTotalSetting, totalMs);
        }
    }
}

QString MainWindow::selfNodeStats() const
{
    int mirrored = 0;
    int online = 0;
    for (const RepositoryRecord &repo : m_repositories) {
        if (repo.previewOnly)
            continue;
        if (repo.lastSyncMs > 0 ||
            (!repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists()))
            ++mirrored;
        if (repo.publishToNetwork)
            ++online;
    }
    const qint64 sessionMs =
        m_connectedAtMs > 0 ? QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs : 0;
    const qint64 totalMs = m_totalConnectionMs + sessionMs;
    const QString key = m_profileIdentity.shortPublicKey();
    const int permanentRepoCount =
        int(std::count_if(m_repositories.cbegin(), m_repositories.cend(),
                          [](const RepositoryRecord &repo) {
                              return !repo.previewOnly;
                          }));
    return QString::fromUtf8(
               "%1 repos \xC2\xB7 %2 mirrored \xC2\xB7 %3 online \xC2\xB7 %4 chats")
               .arg(permanentRepoCount)
               .arg(mirrored)
               .arg(online)
               .arg(m_channels.size()) +
           "\nuptime " + formatDuration(sessionMs) + " \xC2\xB7 total " +
           formatDuration(totalMs) +
           (key.isEmpty() ? QString() : " \xC2\xB7 key " + key);
}
