
#include "Tooltips.h"
#include "CatalogAdminDelete.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "SafeDelete.h"
#include "RepoStatsStore.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "KebabHeaderView.h"
#include "LogTimelineChart.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PrivateMirrorStore.h"
#include "PromptDraftStore.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "RepoSecurity.h"
#include "RewardPoolSigner.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"
#include "FontSizing.h"
#include "DesignTokens.h"
#include "UiKit.h"

#include <QBrush>
#include <QGridLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QMenu>
#include <QScrollArea>
#include <QButtonGroup>
#include <QTabBar>
#include <QRegularExpression>
#include <QTabWidget>

#include <algorithm>
#include <cmath>

#if defined(Q_OS_UNIX)
#include <signal.h>
#endif

using namespace forkmesh::ui;

void MainWindow::refreshNetworkTab(int tabIndex)
{
    if (tabIndex >= 0 && tabIndex == m_networkWebRequestsTabIndex) {
        refreshNetworkWebRequests();
        return;
    }
    switch (tabIndex) {
    case kNetworkNodesTab:
        refreshRelayNodes();
        refreshRunners();
        refreshNodesTable();
        loadMirrorNodesPanel();
        break;
    case kNetworkHostsTab:
        refreshHostsTable();
        refreshRunners();
        break;
    default:
        refreshFirewallTables();
        refreshNetworkDiagnostics();
        break;
    }
}

namespace {
// Repos table columns. One column per published field grew into a strip too
// wide to read, so the facts are stacked instead: every repository is a
// four-line row -- who and what it is, what it contains, and the machine
// serving it -- with each line built from the same fields in the same order,
// so the same fact sits on the same line in every row. The buttons keep their
// own column so they stay reachable without scrolling, and the values you only
// want when you are copying them (URLs, digests, node ids, exact timestamps)
// hang off the cell tooltips. Two things are still deliberately left out: the
// description (prose for the repository's own page, not a grid cell) and raw
// signatures / private-archive locators (proof material rather than repository
// facts — their digests are shown instead).

// Rows reserve room for this many lines so every row is the same height and
// the columns read as a grid rather than as ragged paragraphs.
constexpr int kRepoCellLines = 6;
constexpr int kRepoCellPadX = 8;
constexpr int kRepoCellPadY = 6;
constexpr int kRepoSectionLabelWidth = 88;
constexpr int kRepoActionSlotWidth = 96;
constexpr int kRepoActionSlotCount = 5;
constexpr int kRepoActionColumns = 3;
constexpr int kRepoActionSlotSpacing = 6;
constexpr int kRepoMirrorsColumnWidth = 88;
constexpr int kRepoNextCellPadX = 12;
constexpr int kRepoNextCellPadY = 10;
constexpr int kRepoNextLineGap = 3;
constexpr int kRepoNextActionSlotWidth = 80;
constexpr int kRepoNextActionSlotSpacing = 4;
constexpr int kRepoNextCompactRowHeight = 46;
constexpr int kRepoNextLabelWidth = 92;

inline int repoCellPadX()
{
    return Theme::isNext() ? kRepoNextCellPadX : kRepoCellPadX;
}

inline int repoCellPadY()
{
    return Theme::isNext() ? kRepoNextCellPadY : kRepoCellPadY;
}

inline int repoActionSlotWidth()
{
    return Theme::isNext() ? kRepoNextActionSlotWidth : kRepoActionSlotWidth;
}

inline int repoActionSlotSpacing()
{
    return Theme::isNext() ? kRepoNextActionSlotSpacing : kRepoActionSlotSpacing;
}

inline int repoActionsColumnWidth()
{
    return 2 * repoCellPadX() + 2 * 4 +
           kRepoActionColumns * repoActionSlotWidth() +
           (kRepoActionColumns - 1) * repoActionSlotSpacing();
}

inline QFont repoCellLeadFont(const QFont &base, bool bold)
{
    QFont font(base);
    if (Theme::isNext()) {
        if (bold)
            font.setFamily(Theme::monoFontFamily());
        font.setPixelSize(13);
        font.setWeight(bold ? QFont::Medium : QFont::Normal);
        return font;
    }
    font.setBold(bold);
    return font;
}

inline QFont repoCellBodyFont(const QFont &base)
{
    QFont font(base);
    if (Theme::isNext()) {
        font.setPixelSize(12);
        font.setWeight(QFont::Normal);
        return font;
    }
    // The style sheet sizes this table in pixels, so honour whichever unit the
    // inherited font actually carries.
    if (font.pixelSize() > 0)
        font.setPixelSize(qMax(9, font.pixelSize() - 1));
    else
        font.setPointSizeF(qMax(7.5, font.pointSizeF() - 1.0));
    return font;
}

inline int networkRepoRowHeight(const QFont &base)
{
    if (Theme::isNext())
        return 2 * kRepoNextCellPadY +
               QFontMetrics(repoCellLeadFont(base, true)).height() +
               (kRepoCellLines - 1) *
                   (QFontMetrics(repoCellBodyFont(base)).height() +
                    kRepoNextLineGap);
    return 2 * kRepoCellPadY +
           QFontMetrics(repoCellLeadFont(base, true)).height() +
           (kRepoCellLines - 1) * QFontMetrics(repoCellBodyFont(base)).height();
}

// A stacked cell must never widen the grid the way one long commit subject
// would, so ResizeToContents stops asking for room past this.
constexpr int kRepoCellMaxWidth = 460;

// Draws the stacked cells: the first line in the row's own colour, the rest a
// step smaller and muted, and every line elided on its own so a long subject
// never pushes the lines beneath it out of the cell.
class NetworkRepoRowDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), networkRepoRowHeight(option.font)));
        const QString text = index.data(Qt::DisplayRole).toString();
        if (Theme::isNext()) {
            if (option.widget &&
                option.widget->property("compactRepoList").toBool())
                size.setHeight(kRepoNextCompactRowHeight);
            if (!text.contains(QLatin1Char('\n')))
                return size;
            const QFontMetrics lead(repoCellLeadFont(
                option.font, index.data(kRepoLeadBoldRole).toBool()));
            const QFontMetrics body(repoCellBodyFont(option.font));
            const QStringList lines = text.split(QLatin1Char('\n'));
            int width = 0;
            for (int i = 0; i < lines.size(); ++i)
                width = qMax(width, i == 0 ? lead.horizontalAdvance(lines.at(i))
                                           : kRepoNextLabelWidth +
                                                 body.horizontalAdvance(lines.at(i)));
            size.setWidth(qMin(width, kRepoCellMaxWidth) + 2 * kRepoNextCellPadX);
            return size;
        }
        if (!text.contains(QLatin1Char('\n')))
            return size;
        const QFontMetrics lead(repoCellLeadFont(
            option.font, index.data(kRepoLeadBoldRole).toBool()));
        const QFontMetrics body(repoCellBodyFont(option.font));
        const QStringList lines = text.split(QLatin1Char('\n'));
        int width = 0;
        for (int i = 0; i < lines.size(); ++i)
            width = qMax(width,
                         (i == 0 ? lead : body).horizontalAdvance(lines.at(i)));
        size.setWidth(qMin(width, kRepoCellMaxWidth) + 2 * kRepoCellPadX);
        return size;
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (Theme::isNext()) {
            paintNext(painter, option, index);
            return;
        }
        // Single-line cells go through here too: the mirror count has to start
        // on the same line as every other cell's first line.
        const QString text = index.data(Qt::DisplayRole).toString();
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        opt.text.clear();
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

        // Matching the #issueTable::item rules: the style sheet colours items
        // itself, and drawControl below only resolves that for the text it
        // draws — which is none, because every line is drawn here.
        const bool dark = currentThemeIsDark();
        const bool selected = !Theme::isNext() &&
                              option.state.testFlag(QStyle::State_Selected);
        QColor lead(selected
                        ? QStringLiteral("#ffffff")
                        : Theme::nextOrHex(Theme::Role::FgSecondary,
                                           QString::fromLatin1(dark ? "#c9d1d9"
                                                                    : "#1f2328")));
        const QVariant foreground = index.data(Qt::ForegroundRole);
        if (!selected && foreground.canConvert<QBrush>())
            lead = qvariant_cast<QBrush>(foreground).color();
        const QColor muted(selected
                               ? QStringLiteral("#c8e1ff")
                               : Theme::nextOrHex(Theme::Role::FgMuted,
                                                  QString::fromLatin1(dark ? "#8b949e"
                                                                           : "#656d76")));

        Qt::Alignment horizontal = Qt::AlignLeft;
        const Qt::Alignment stored =
            Qt::Alignment(index.data(Qt::TextAlignmentRole).toInt()) &
            Qt::AlignHorizontal_Mask;
        if (stored != Qt::Alignment())
            horizontal = stored;

        const QFont leadFont = repoCellLeadFont(
            option.font, index.data(kRepoLeadBoldRole).toBool());
        const QFont bodyFont = repoCellBodyFont(option.font);
        const QRect area = option.rect.adjusted(kRepoCellPadX, kRepoCellPadY,
                                                -kRepoCellPadX, -kRepoCellPadY);
        const QStringList lines = text.split(QLatin1Char('\n'));
        painter->save();
        int y = area.top();
        for (int i = 0; i < lines.size(); ++i) {
            const QFont &font = i == 0 ? leadFont : bodyFont;
            const QFontMetrics metrics(font);
            const int height = metrics.height();
            if (y + height > area.bottom() + 1)
                break;
            const int divider = lines.at(i).indexOf(QLatin1Char(':'));
            if (divider > 0 && horizontal == Qt::AlignLeft) {
                QFont labelFont(bodyFont);
                labelFont.setBold(true);
                const QString label = lines.at(i).left(divider + 1);
                const QString value = lines.at(i).mid(divider + 1).trimmed();
                const QFontMetrics labelMetrics(labelFont);
                const int labelWidth = qMin(area.width(),
                                            kRepoSectionLabelWidth);
                painter->setFont(labelFont);
                painter->setPen(lead);
                painter->drawText(
                    QRect(area.left(), y, labelWidth, height),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    labelMetrics.elidedText(label, Qt::ElideRight, labelWidth));
                painter->setFont(bodyFont);
                painter->setPen(muted);
                painter->drawText(
                    QRect(area.left() + labelWidth, y,
                          qMax(0, area.width() - labelWidth), height),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    metrics.elidedText(value, Qt::ElideRight,
                                       qMax(0, area.width() - labelWidth)));
            } else {
                painter->setFont(font);
                painter->setPen(i == 0 ? lead : muted);
                painter->drawText(
                    QRect(area.left(), y, area.width(), height),
                    horizontal | Qt::AlignVCenter,
                    metrics.elidedText(lines.at(i), Qt::ElideRight,
                                       area.width()));
            }
            y += height;
        }
        painter->restore();
    }

private:
    struct NextBadge {
        QString text;
        Theme::Role fill;
        Theme::Role ink;
        bool outline;
    };

    static NextBadge nextBadgeFor(const QString &part)
    {
        if (part.startsWith(QLatin1String("Flagged")))
            return {part, Theme::Role::DangerSubtle, Theme::Role::Danger, false};
        if (part == QLatin1String("Live host"))
            return {part, Theme::Role::AccentSubtle, Theme::Role::AccentFg, false};
        if (part.startsWith(QLatin1String("Public")) ||
            part.startsWith(QLatin1String("Private")))
            return {part, Theme::Role::BorderStrong, Theme::Role::FgMuted, true};
        return {part, Theme::Role::SurfaceActive, Theme::Role::FgMuted, false};
    }

    static QStringList nextAccessParts(const QString &line)
    {
        const QString prefix = QStringLiteral("Access:");
        QStringList parts;
        if (!line.startsWith(prefix))
            return parts;
        const QString dash = QString::fromUtf8("\xE2\x80\x94");
        const QStringList pieces = line.mid(prefix.size())
                                       .split(QString::fromUtf8(" \xC2\xB7 "),
                                              Qt::SkipEmptyParts);
        for (const QString &piece : pieces) {
            const QString trimmed = piece.trimmed();
            if (!trimmed.isEmpty() && trimmed != dash)
                parts << trimmed;
        }
        return parts;
    }

    static QColor nextForeground(const QModelIndex &index, const QColor &fallback)
    {
        const QVariant foreground = index.data(Qt::ForegroundRole);
        if (!foreground.canConvert<QBrush>())
            return fallback;
        const QBrush brush = qvariant_cast<QBrush>(foreground);
        return brush.style() == Qt::NoBrush ? fallback : brush.color();
    }

    void paintNext(QPainter *painter, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const
    {
        const QWidget *widget = option.widget;
        const QRect cell = option.rect;
        painter->save();
        const QVariant hover =
            widget ? widget->property("reposNextHoverRow") : QVariant();
        if (option.state.testFlag(QStyle::State_Selected))
            painter->fillRect(cell, Theme::color(Theme::Role::SurfaceActive));
        else if (hover.isValid() && hover.toInt() == index.row())
            painter->fillRect(cell, Theme::color(Theme::Role::SurfaceRaised));
        if (index.model() && index.row() < index.model()->rowCount() - 1)
            painter->fillRect(QRect(cell.left(), cell.bottom(), cell.width(), 1),
                              Theme::color(Theme::Role::Border));

        const QString text = index.data(Qt::DisplayRole).toString();
        if (text.isEmpty()) {
            painter->restore();
            return;
        }
        painter->setRenderHint(QPainter::Antialiasing, true);
        const bool title = index.data(kRepoLeadBoldRole).toBool();
        const bool compact =
            widget && widget->property("compactRepoList").toBool();
        const QFont leadFont = repoCellLeadFont(option.font, title);
        const QFont bodyFont = repoCellBodyFont(option.font);
        const QFontMetrics leadMetrics(leadFont);
        const QFontMetrics bodyMetrics(bodyFont);
        const QColor fg = Theme::color(Theme::Role::Fg);
        const QColor secondary = Theme::color(Theme::Role::FgSecondary);
        const QColor muted = Theme::color(Theme::Role::FgMuted);
        const QColor subtle = Theme::color(Theme::Role::FgSubtle);
        const QString dash = QString::fromUtf8("\xE2\x80\x94");
        const QString dot = QString::fromUtf8(" \xC2\xB7 ");

        Qt::Alignment horizontal = Qt::AlignLeft;
        const Qt::Alignment stored =
            Qt::Alignment(index.data(Qt::TextAlignmentRole).toInt()) &
            Qt::AlignHorizontal_Mask;
        if (stored != Qt::Alignment())
            horizontal = stored;

        if (compact && !text.contains(QLatin1Char('\n'))) {
            const QRect area =
                cell.adjusted(kRepoNextCellPadX, 0, -kRepoNextCellPadX, 0);
            QStringList parts = text.split(QStringLiteral(" | "));
            const QString head = parts.takeFirst();
            const int headRoom =
                parts.isEmpty()
                    ? area.width()
                    : qMin(area.width(),
                           qMax(area.width() / 2,
                                leadMetrics.horizontalAdvance(head)));
            const QString shownHead =
                leadMetrics.elidedText(head, Qt::ElideRight, headRoom);
            painter->setFont(leadFont);
            painter->setPen(title ? fg : nextForeground(index, secondary));
            painter->drawText(QRect(area.left(), area.top(), headRoom, area.height()),
                              horizontal | Qt::AlignVCenter, shownHead);
            if (!parts.isEmpty()) {
                const int left =
                    area.left() + leadMetrics.horizontalAdvance(shownHead) + 14;
                const QRect rest(left, area.top(),
                                 qMax(0, area.right() + 1 - left), area.height());
                painter->setFont(bodyFont);
                painter->setPen(muted);
                painter->drawText(rest, Qt::AlignLeft | Qt::AlignVCenter,
                                  bodyMetrics.elidedText(parts.join(dot),
                                                         Qt::ElideRight,
                                                         rest.width()));
            }
            painter->restore();
            return;
        }

        const QRect area = cell.adjusted(kRepoNextCellPadX, kRepoNextCellPadY,
                                         -kRepoNextCellPadX, -kRepoNextCellPadY);
        const QStringList lines = text.split(QLatin1Char('\n'));
        int y = area.top();
        int first = 0;
        if (title) {
            const bool hasAccess =
                lines.size() > 1 &&
                lines.at(1).startsWith(QLatin1String("Access:"));
            QList<NextBadge> badges;
            if (hasAccess)
                for (const QString &part : nextAccessParts(lines.at(1)))
                    badges << nextBadgeFor(part);
            QFont badgeFont = bodyFont;
            badgeFont.setPixelSize(11);
            badgeFont.setWeight(QFont::Medium);
            const QFontMetrics badgeMetrics(badgeFont);
            int badgesWidth = 0;
            for (const NextBadge &badge : std::as_const(badges))
                badgesWidth += 6 + badgeMetrics.horizontalAdvance(badge.text) + 14;
            const int lineHeight = leadMetrics.height();
            const int nameRoom = qMin(area.width(),
                                      qMax(area.width() / 2,
                                           area.width() - badgesWidth));
            const QString name =
                leadMetrics.elidedText(lines.at(0), Qt::ElideRight, nameRoom);
            painter->setFont(leadFont);
            painter->setPen(fg);
            painter->drawText(QRect(area.left(), y, nameRoom, lineHeight),
                              Qt::AlignLeft | Qt::AlignVCenter, name);
            int x = area.left() + leadMetrics.horizontalAdvance(name);
            const int centerY = y + lineHeight / 2;
            painter->setFont(badgeFont);
            for (const NextBadge &badge : std::as_const(badges)) {
                x += 6;
                const int width = badgeMetrics.horizontalAdvance(badge.text) + 14;
                if (x + width > area.right() + 1)
                    break;
                const QRectF pill(x + 0.5, centerY - 8.5, width - 1, 17);
                if (badge.outline) {
                    painter->setPen(QPen(Theme::color(badge.fill), 1));
                    painter->setBrush(Qt::NoBrush);
                    painter->drawRoundedRect(pill, 8.5, 8.5);
                } else {
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(Theme::color(badge.fill));
                    painter->drawRoundedRect(pill, 5, 5);
                }
                painter->setPen(Theme::color(badge.ink));
                painter->drawText(pill, Qt::AlignCenter, badge.text);
                x += width;
            }
            y += lineHeight + kRepoNextLineGap + 1;
            first = hasAccess ? 2 : 1;
        }

        for (int i = first; i < lines.size(); ++i) {
            const bool leadLine = i == 0;
            const QFont &font = leadLine ? leadFont : bodyFont;
            const QFontMetrics metrics(font);
            const int height = metrics.height();
            if (y + height > area.bottom() + 1)
                break;
            const QString &line = lines.at(i);
            const int divider = line.indexOf(QLatin1Char(':'));
            if (divider > 0 && horizontal == Qt::AlignLeft) {
                const int labelWidth = qMin(area.width(), kRepoNextLabelWidth);
                const QString value = line.mid(divider + 1).trimmed();
                painter->setFont(bodyFont);
                painter->setPen(muted);
                painter->drawText(QRect(area.left(), y, labelWidth, height),
                                  Qt::AlignLeft | Qt::AlignVCenter,
                                  bodyMetrics.elidedText(line.left(divider),
                                                         Qt::ElideRight,
                                                         qMax(0, labelWidth - 6)));
                const int valueWidth = qMax(0, area.width() - labelWidth);
                painter->setPen(value == dash ? subtle : secondary);
                painter->drawText(QRect(area.left() + labelWidth, y, valueWidth, height),
                                  Qt::AlignLeft | Qt::AlignVCenter,
                                  bodyMetrics.elidedText(value, Qt::ElideRight,
                                                         valueWidth));
            } else {
                painter->setFont(font);
                painter->setPen(leadLine ? nextForeground(index, secondary) : muted);
                painter->drawText(QRect(area.left(), y, area.width(), height),
                                  horizontal | Qt::AlignVCenter,
                                  metrics.elidedText(line, Qt::ElideRight,
                                                     area.width()));
            }
            y += height + kRepoNextLineGap;
        }
        painter->restore();
    }
};

void reposNextPaintBars(QPainter &painter, const QRectF &area,
                        const QVector<qint64> &weeks)
{
    const int count = weeks.size();
    if (count <= 0 || area.width() <= 1.0)
        return;
    qint64 maximum = 1;
    for (const qint64 value : weeks)
        maximum = qMax(maximum, value);
    const QColor zero = Theme::color(Theme::Role::SurfaceActive);
    const QColor recent = Theme::color(Theme::Role::Accent);
    const QColor bar = Theme::color(Theme::Role::Fg, 89);
    painter.setPen(Qt::NoPen);
    const qreal step = area.width() / count;
    const qreal barWidth = qMax(1.0, step - 1.0);
    for (int index = 0; index < count; ++index) {
        const qint64 value = weeks.at(index);
        const qreal height = value > 0
            ? qMax(area.height() * 0.1, area.height() * value / maximum)
            : 2.0;
        painter.setBrush(value <= 0 ? zero : (index >= count - 4 ? recent : bar));
        painter.drawRect(QRectF(area.left() + index * step, area.bottom() - height,
                                barWidth, height));
    }
}

class NetworkRepoActivityChart final : public QWidget
{
public:
    NetworkRepoActivityChart(const QVector<qint64> &weeks,
                             qint64 totalCount,
                             const QString &singular,
                             const QString &plural,
                             const QColor &activeColor,
                             const QString &objectName,
                             QWidget *parent = nullptr)
        : QWidget(parent), m_weeks(weeks), m_totalCount(totalCount),
          m_singular(singular), m_plural(plural), m_activeColor(activeColor)
    {
        setObjectName(objectName);
        setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        setMinimumSize(112, 54);
        setMaximumHeight(68);
        setAttribute(Qt::WA_TransparentForMouseEvents);

        qint64 activityTotal = 0;
        QStringList values;
        for (const qint64 value : m_weeks) {
            activityTotal += value;
            values << QString::number(value);
        }
        m_activityTotal = activityTotal;
        const auto countLabel = [this](qint64 count) {
            return QStringLiteral("%1 %2")
                .arg(formatCount(count), count == 1 ? m_singular : m_plural);
        };
        m_summary = m_totalCount >= 0
            ? (activityTotal && activityTotal != m_totalCount
                   ? QStringLiteral("%1 total; %2 in the past 52 weeks")
                         .arg(countLabel(m_totalCount),
                              countLabel(activityTotal))
                   : countLabel(m_totalCount))
            : QStringLiteral("%1 in the past 52 weeks")
                  .arg(countLabel(activityTotal));
        setAccessibleName(m_summary);
        const QString activityName =
            m_plural.left(1).toUpper() + m_plural.mid(1);
        setToolTip(QStringLiteral("%1\n%2 per week (oldest first):\n%3")
                       .arg(m_summary, activityName,
                            values.join(QLatin1Char(' '))));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (Theme::isNext()) {
            paintNext(painter);
            return;
        }

        const QString totalLabel = m_totalCount >= 0
            ? QStringLiteral("%1 total").arg(formatCount(m_totalCount))
            : QStringLiteral("52-week total %1")
                  .arg(formatCount(m_activityTotal));
        QFont valueFont = font();
        valueFont.setPointSizeF(qMax(7.0, valueFont.pointSizeF() - 1.0));
        const QFontMetrics valueMetrics(valueFont);
        const int footerHeight = valueMetrics.height() + 3;
        const QRectF chartRect = QRectF(rect()).adjusted(
            1.0, 3.0, -1.0, -footerHeight - 3.0);
        const QRectF labelRect(1.0, chartRect.bottom() + 3.0,
                               qMax(0, width() - 2), footerHeight);

        QColor track = palette().color(QPalette::WindowText);
        track.setAlpha(30);
        QColor emptyBar = palette().color(QPalette::WindowText);
        emptyBar.setAlpha(45);
        painter.setPen(Qt::NoPen);
        painter.setBrush(track);
        painter.drawRoundedRect(chartRect, 3.0, 3.0);

        qint64 maximum = 1;
        for (const qint64 value : m_weeks)
            maximum = qMax(maximum, value);
        const int count = m_weeks.size();
        if (count > 0 && chartRect.width() > 1.0) {
            const qreal step = chartRect.width() / count;
            const qreal barWidth = qMax(1.0, step - 0.65);
            for (int index = 0; index < count; ++index) {
                const qint64 value = m_weeks.at(index);
                const qreal height = value > 0
                    ? qMax(2.0, (chartRect.height() - 3.0) * value / maximum)
                    : 1.0;
                const QRectF bar(chartRect.left() + index * step + (step - barWidth) / 2.0,
                                 chartRect.bottom() - height - 1.0, barWidth, height);
                painter.setBrush(value > 0 ? m_activeColor : emptyBar);
                painter.drawRoundedRect(bar, 1.0, 1.0);
            }
        }

        painter.setFont(valueFont);
        QColor label = palette().color(QPalette::WindowText);
        label.setAlpha(180);
        painter.setPen(label);
        painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                         totalLabel);
        painter.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("52 weeks"));
    }

private:
    void paintNext(QPainter &painter)
    {
        const qint64 shownTotal =
            m_totalCount >= 0 ? m_totalCount : m_activityTotal;
        QFont valueFont = font();
        valueFont.setPixelSize(12);
        valueFont.setWeight(QFont::Normal);
        const QFontMetrics valueMetrics(valueFont);
        const int footerHeight = valueMetrics.height();
        const QRectF area = QRectF(rect()).adjusted(
            kRepoNextCellPadX, kRepoNextCellPadY, -kRepoNextCellPadX, 0);
        const qreal barsHeight =
            qBound(8.0, area.height() - footerHeight - 6.0, 32.0);
        const QRectF chartRect(area.left(), area.top(), area.width(), barsHeight);
        const QRectF labelRect(area.left(), chartRect.bottom() + 6.0,
                               area.width(), footerHeight);

        reposNextPaintBars(painter, chartRect, m_weeks);

        painter.setFont(valueFont);
        painter.setPen(Theme::color(Theme::Role::FgMuted));
        painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                         m_totalCount >= 0
                             ? QStringLiteral("%1 total").arg(formatCount(shownTotal))
                             : QStringLiteral("%1 in 52 weeks")
                                   .arg(formatCount(shownTotal)));
        painter.setPen(Theme::color(Theme::Role::FgSubtle));
        painter.drawText(labelRect, Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("52 wk"));
    }

    QVector<qint64> m_weeks;
    qint64 m_totalCount = -1;
    qint64 m_activityTotal = 0;
    QString m_singular;
    QString m_plural;
    QColor m_activeColor;
    QString m_summary;
};

enum NetworkRepoCol {
    kRepoColName = 0,
    kRepoColContents,
    kRepoColActivity,
    kRepoColMergeActivity,
    kRepoColHost,
    kRepoColMirrors,
    kRepoColActions,
    kRepoColCount,
};

constexpr int kRepoDetailedTextRole = Qt::UserRole + 40;
constexpr int kRepoCompactTextRole = Qt::UserRole + 41;
constexpr int kRepoNextRecordRole = Qt::UserRole + 46;
constexpr int kRepoNextCurrentRole = Qt::UserRole + 47;

QStringList networkRepoHeaders()
{
    return {QStringLiteral("Repository"), QStringLiteral("Contents"),
            QStringLiteral("Commit activity"), QStringLiteral("Merge activity"),
            QStringLiteral("Host"), QStringLiteral("Mirrors"),
            QStringLiteral("Actions")};
}

// Keep the page a real grid instead of letting whichever repository happens to
// contain the longest string decide the shape of every column. The four data
// sections share the available width equally; the compact mirror count and the
// five aligned action slots keep stable widths of their own.
void configureNetworkRepoGrid(QTableWidget *table)
{
    if (!table)
        return;
    table->setProperty("repositoryGrid", true);
    table->setShowGrid(true);
    table->setGridStyle(Qt::SolidLine);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);

    QHeaderView *horizontal = table->horizontalHeader();
    horizontal->setStretchLastSection(false);
    horizontal->setMinimumSectionSize(64);
    for (const int column : {kRepoColName, kRepoColContents,
                             kRepoColActivity, kRepoColMergeActivity,
                             kRepoColHost}) {
        horizontal->setSectionResizeMode(column, QHeaderView::Stretch);
    }
    horizontal->setSectionResizeMode(kRepoColMirrors, QHeaderView::Fixed);
    horizontal->resizeSection(kRepoColMirrors, kRepoMirrorsColumnWidth);
    horizontal->setSectionResizeMode(kRepoColActions, QHeaderView::Fixed);
    horizontal->resizeSection(kRepoColActions, repoActionsColumnWidth());

    QHeaderView *vertical = table->verticalHeader();
    vertical->setSectionResizeMode(QHeaderView::Fixed);
    vertical->setDefaultSectionSize(networkRepoRowHeight(table->font()));
    vertical->setMinimumSectionSize(networkRepoRowHeight(table->font()));
}

QGridLayout *networkRepoActionGrid(QWidget *cell)
{
    auto *column = new QVBoxLayout(cell);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    auto *grid = new QGridLayout;
    grid->setContentsMargins(4, repoCellPadY(), 4, 2);
    grid->setHorizontalSpacing(repoActionSlotSpacing());
    grid->setVerticalSpacing(Theme::isNext() ? 6 : 5);
    if (Theme::isNext())
        grid->setContentsMargins(kRepoNextCellPadX + 4, kRepoNextCellPadY,
                                 kRepoNextCellPadX - 4, 2);
    for (int columnIndex = 0; columnIndex < kRepoActionColumns;
         ++columnIndex)
        grid->setColumnMinimumWidth(columnIndex, repoActionSlotWidth());
    column->addLayout(grid);
    column->addStretch();
    return grid;
}

void addNetworkRepoAction(QGridLayout *grid, QPushButton *button, int slot)
{
    if (!grid || !button)
        return;
    button->setFixedWidth(repoActionSlotWidth());
    // First three controls use the top row. Web starts the second row and the
    // destructive action stays pinned to its right edge, leaving a deliberate
    // gap between navigation and deletion.
    const int row = slot < kRepoActionColumns ? 0 : 1;
    const int column = slot < kRepoActionColumns ? slot :
                       (slot == kRepoActionSlotCount - 1
                            ? kRepoActionColumns - 1
                            : slot - kRepoActionColumns);
    grid->addWidget(button, row, column);
}

class ReposNextTableChrome final : public QObject
{
public:
    explicit ReposNextTableChrome(QTableWidget *table)
        : QObject(table), m_table(table)
    {
        setObjectName(QStringLiteral("reposNextChrome"));
        table->setProperty("reposNextHoverRow", -1);
        table->viewport()->setMouseTracking(true);
        table->viewport()->installEventFilter(this);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_table)
            return false;
        QWidget *viewport = m_table->viewport();
        if (watched == viewport) {
            if (event->type() == QEvent::MouseMove)
                setHovered(m_table->rowAt(
                    static_cast<QMouseEvent *>(event)->position().toPoint().y()));
            else if (event->type() == QEvent::Leave)
                setHovered(-1);
        } else if (event->type() == QEvent::Enter) {
            auto *cell = qobject_cast<QWidget *>(watched);
            if (cell && cell->parentWidget() == viewport)
                setHovered(m_table->rowAt(cell->geometry().center().y()));
        }
        return false;
    }

private:
    void setHovered(int row)
    {
        const int before = m_table->property("reposNextHoverRow").toInt();
        if (before == row)
            return;
        m_table->setProperty("reposNextHoverRow", row);
        updateRow(before);
        updateRow(row);
    }

    void updateRow(int row)
    {
        if (row < 0 || row >= m_table->rowCount())
            return;
        m_table->viewport()->update(QRect(0, m_table->rowViewportPosition(row),
                                          m_table->viewport()->width(),
                                          m_table->rowHeight(row)));
    }

    QPointer<QTableWidget> m_table;
};

void reposNextStyleTable(QTableWidget *table)
{
    if (!table || !Theme::isNext())
        return;
    table->setProperty("reposNext", true);
    table->setAlternatingRowColors(false);
    table->setShowGrid(false);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft |
                                                   Qt::AlignVCenter);
    new ReposNextTableChrome(table);
}

void reposNextTrackCell(QTableWidget *table, QWidget *cell)
{
    if (!table || !cell || !Theme::isNext())
        return;
    if (QObject *chrome = table->findChild<QObject *>(
            QStringLiteral("reposNextChrome"), Qt::FindDirectChildrenOnly))
        cell->installEventFilter(chrome);
}

QLineEdit *reposNextFilterField(const QString &objectName, QWidget *parent)
{
    auto *field = new QLineEdit(parent);
    field->setObjectName(objectName);
    field->setPlaceholderText(QStringLiteral("Filter repositories"));
    field->setToolTip(QStringLiteral(
        "Show only repositories whose name, branch, commit message or author "
        "contains this text"));
    field->setClearButtonEnabled(true);
    field->setFixedWidth(288);
    field->addAction(QIcon(kit::tintedIcon(QStringLiteral("search"),
                                           Theme::color(Theme::Role::FgSubtle),
                                           14, field->devicePixelRatioF())),
                     QLineEdit::LeadingPosition);
    return field;
}

QPushButton *reposNextHelpButton(const QString &accessibleName,
                                 const QString &helpText)
{
    auto *button = new QPushButton;
    button->setObjectName(QStringLiteral("inlineHelpButton"));
    button->setAccessibleName(accessibleName);
    button->setAccessibleDescription(helpText);
    button->setToolTip(
        QStringLiteral("<div style='width: 420px; white-space: normal;'>%1</div>")
            .arg(helpText.toHtmlEscaped()));
    button->setCursor(Qt::PointingHandCursor);
    button->setFlat(true);
    button->setFixedSize(24, 24);
    button->setProperty("nextIconRole", int(Theme::Role::FgMuted));
    setOcticon(button, "info", 14);
    return button;
}

void reposNextButtonRole(QPushButton *button, const char *role,
                         Theme::Role icon)
{
    if (!button)
        return;
    button->setProperty("reposRole", QString::fromLatin1(role));
    button->setProperty("nextIconRole", int(icon));
    applyStoredOcticon(button);
}

void reposNextStyleRowActions(QWidget *actions)
{
    if (!actions || !Theme::isNext())
        return;
    actions->setProperty("reposNext", true);
    const auto buttons =
        actions->findChildren<QPushButton *>(QString(), Qt::FindDirectChildrenOnly);
    for (QPushButton *button : buttons) {
        const QString name = button->objectName();
        if (name == QLatin1String("primaryButton"))
            reposNextButtonRole(button, "outline", Theme::Role::FgSecondary);
        else if (name == QLatin1String("dangerButton"))
            reposNextButtonRole(button, "danger", Theme::Role::Danger);
        else
            reposNextButtonRole(button, "ghost", Theme::Role::FgMuted);
    }
}

class ReposNextElidedLabel final : public QLabel
{
public:
    explicit ReposNextElidedLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(text, parent)
    {
        setTextFormat(Qt::PlainText);
        setToolTip(text);
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, QLabel::minimumSizeHint().height());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QRect area = contentsRect();
        const QString shown =
            fontMetrics().elidedText(text(), Qt::ElideRight, area.width());
        style()->drawItemText(&painter, area, int(alignment()), palette(),
                              isEnabled(), shown, foregroundRole());
    }
};

class ReposNextSparkCell final : public QWidget
{
public:
    ReposNextSparkCell(const QString &title, const QVector<qint64> &weeks,
                       qint64 total, QWidget *parent = nullptr)
        : QWidget(parent), m_title(title), m_weeks(weeks), m_total(total)
    {
        setObjectName(QStringLiteral("reposNextCardSpark"));
        setMinimumSize(120, 72);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QFont head = font();
        head.setPixelSize(13);
        head.setWeight(QFont::Normal);
        QFont small = head;
        small.setPixelSize(12);
        const QFontMetrics headMetrics(head);
        const QRectF top(0, 0, width(), headMetrics.height());
        painter.setFont(head);
        painter.setPen(Theme::color(Theme::Role::FgMuted));
        painter.drawText(top, Qt::AlignLeft | Qt::AlignVCenter, m_title);
        painter.setFont(small);
        painter.setPen(Theme::color(Theme::Role::FgSubtle));
        painter.drawText(top, Qt::AlignRight | Qt::AlignVCenter,
                         m_total >= 0
                             ? QString::fromUtf8("%1 \xC2\xB7 52 wk")
                                   .arg(formatCount(m_total))
                             : QString::fromUtf8("\xE2\x80\x94"));
        const qreal barsTop = top.bottom() + 12.0;
        const QRectF bars(0, barsTop, width(),
                          qBound(8.0, height() - barsTop, 40.0));
        if (m_weeks.isEmpty()) {
            painter.setFont(head);
            painter.setPen(Theme::color(Theme::Role::FgSubtle));
            painter.drawText(bars, Qt::AlignLeft | Qt::AlignTop,
                             QStringLiteral("Activity not published"));
            return;
        }
        reposNextPaintBars(painter, bars, m_weeks);
    }

private:
    QString m_title;
    QVector<qint64> m_weeks;
    qint64 m_total = -1;
};

QVariant reposNextNumber(const QJsonObject &record, const QString &key)
{
    const QJsonValue value = record.value(key);
    if (value.isDouble())
        return QVariant(qint64(value.toDouble()));
    bool ok = false;
    const qint64 parsed = value.toString().trimmed().toLongLong(&ok);
    return ok ? QVariant(parsed) : QVariant();
}

QString reposNextText(const QJsonObject &record, const QString &key)
{
    return record.value(key).toString().trimmed();
}

QString reposNextDash()
{
    return QString::fromUtf8("\xE2\x80\x94");
}

QLabel *reposNextLabel(const QString &text, const char *objectName,
                       QWidget *parent = nullptr)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel *reposNextIconLabel(const QString &octicon, Theme::Role role, int px,
                           QWidget *parent = nullptr)
{
    auto *label = new QLabel(parent);
    label->setObjectName(QStringLiteral("reposNextCardIcon"));
    label->setPixmap(kit::tintedIcon(octicon, Theme::color(role), px,
                                     label->devicePixelRatioF()));
    label->setFixedSize(px, px);
    return label;
}

QLabel *reposNextPill(const QString &text, const char *tone,
                      QWidget *parent = nullptr)
{
    auto *pill = reposNextLabel(text, "reposNextPill", parent);
    pill->setProperty("tone", QString::fromLatin1(tone));
    pill->setAlignment(Qt::AlignCenter);
    return pill;
}

const char *reposNextPillTone(const QString &part)
{
    if (part.startsWith(QLatin1String("Flagged")))
        return "danger";
    if (part == QLatin1String("Live host"))
        return "accent";
    if (part.startsWith(QLatin1String("Public")) ||
        part.startsWith(QLatin1String("Private")))
        return "outline";
    return "neutral";
}

QPixmap reposNextInitialsPixmap(const QString &name, qreal dpr)
{
    constexpr int side = 18;
    QPixmap pixmap(qRound(side * dpr), qRound(side * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QString initials;
    const QStringList parts = name.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        initials += part.at(0).toUpper();
        if (initials.size() == 2)
            break;
    }
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Theme::color(Theme::Role::SurfaceActive));
    painter.drawEllipse(QRectF(0, 0, side, side));
    QFont font = painter.font();
    font.setPixelSize(8);
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(Theme::color(Theme::Role::FgMuted));
    painter.drawText(QRectF(0, 0, side, side), Qt::AlignCenter, initials);
    return pixmap;
}

QPixmap reposNextGridPixmap(const QColor &color, qreal dpr)
{
    constexpr int side = 14;
    QPixmap pixmap(qRound(side * dpr), qRound(side * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, 1.3));
    painter.setBrush(Qt::NoBrush);
    for (const QPointF &corner : {QPointF(1.5, 1.5), QPointF(8.0, 1.5),
                                  QPointF(1.5, 8.0), QPointF(8.0, 8.0)})
        painter.drawRoundedRect(QRectF(corner, QSizeF(4.5, 4.5)), 1.0, 1.0);
    return pixmap;
}

QString reposNextMirrorsText(const QTableWidgetItem *item)
{
    if (!item)
        return reposNextDash();
    bool ok = false;
    const int count = item->text().toInt(&ok);
    if (!ok)
        return item->text();
    return count == 1 ? QStringLiteral("1 mirror")
                      : QStringLiteral("%1 mirrors").arg(formatCount(count));
}

QWidget *reposNextCardList(QTableWidget *table)
{
    if (!table || !table->parentWidget())
        return nullptr;
    return table->parentWidget()->findChild<QWidget *>(
        QStringLiteral("reposNextCardList"));
}

QList<QFrame *> reposNextCards(QWidget *list)
{
    return list ? list->findChildren<QFrame *>(QStringLiteral("reposNextCard"),
                                                Qt::FindDirectChildrenOnly)
                : QList<QFrame *>();
}

QPushButton *reposNextCardButton(QPushButton *source, const QString &text,
                                 const char *role, Theme::Role iconRole)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QStringLiteral("reposNextCardAction"));
    button->setProperty("reposRole", QString::fromLatin1(role));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(source->toolTip());
    button->setEnabled(source->isEnabled());
    const QString icon = source->property("forkmeshOcticon").toString();
    if (!icon.isEmpty()) {
        button->setIcon(QIcon(kit::tintedIcon(icon, Theme::color(iconRole), 14,
                                              button->devicePixelRatioF())));
        button->setIconSize(QSize(14, 14));
    }
    QPointer<QPushButton> target(source);
    QObject::connect(button, &QPushButton::clicked, button, [target] {
        if (target)
            target->click();
    });
    return button;
}

QPushButton *reposNextMoreButton(const QList<QPushButton *> &sources)
{
    auto *button = new QPushButton;
    button->setObjectName(QStringLiteral("reposNextCardAction"));
    button->setProperty("reposRole", QStringLiteral("more"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(QStringLiteral("More actions"));
    button->setAccessibleName(QStringLiteral("More actions"));
    button->setIcon(QIcon(kit::tintedIcon(QStringLiteral("kebab-horizontal"),
                                          Theme::color(Theme::Role::FgMuted), 14,
                                          button->devicePixelRatioF())));
    button->setIconSize(QSize(14, 14));
    QList<QPointer<QPushButton>> targets;
    for (QPushButton *source : sources)
        targets.append(QPointer<QPushButton>(source));
    QPointer<QPushButton> self(button);
    QObject::connect(button, &QPushButton::clicked, button, [self, targets] {
        if (!self)
            return;
        QMenu menu(self);
        for (const QPointer<QPushButton> &target : targets) {
            if (!target)
                continue;
            const bool danger =
                target->objectName() == QLatin1String("dangerButton");
            QAction *action = menu.addAction(target->text());
            const QString icon = target->property("forkmeshOcticon").toString();
            if (!icon.isEmpty())
                action->setIcon(QIcon(kit::tintedIcon(
                    icon,
                    Theme::color(danger ? Theme::Role::Danger
                                        : Theme::Role::FgMuted),
                    14, self->devicePixelRatioF())));
            action->setToolTip(target->toolTip());
            action->setEnabled(target->isEnabled());
            QObject::connect(action, &QAction::triggered, target.data(),
                             [target] {
                                 QTimer::singleShot(0, target.data(), [target] {
                                     if (target)
                                         target->click();
                                 });
                             });
        }
        menu.setToolTipsVisible(true);
        menu.exec(self->mapToGlobal(QPoint(0, self->height() + 4)));
    });
    return button;
}

QFrame *reposNextCell(bool first)
{
    auto *cell = new QFrame;
    cell->setObjectName(QStringLiteral("reposNextCardCell"));
    cell->setProperty("reposCellFirst", first);
    return cell;
}

QWidget *reposNextStatsCell(const QJsonObject &record, const QString &tip)
{
    QFrame *cell = reposNextCell(true);
    cell->setToolTip(tip);
    auto *grid = new QGridLayout(cell);
    grid->setContentsMargins(20, 16, 20, 16);
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(14);
    const auto count = [&record](const QString &key) {
        const QVariant value = reposNextNumber(record, key);
        return value.isValid() ? formatCount(value.toLongLong()) : reposNextDash();
    };
    QString copies = reposNextDash();
    const QVariant worktrees = reposNextNumber(record, QStringLiteral("worktreeCount"));
    const QVariant artifacts = reposNextNumber(record, QStringLiteral("artifactCount"));
    if (worktrees.isValid() || artifacts.isValid())
        copies = formatCount(worktrees.toLongLong() + artifacts.toLongLong());
    const QVariant size = reposNextNumber(record, QStringLiteral("sizeBytes"));
    const QString storage = size.isValid() && size.toLongLong() > 0
                                ? SystemStats::formatBytes(size.toLongLong())
                                : reposNextDash();
    struct Stat {
        const char *icon;
        const char *key;
        QString value;
    };
    const QList<Stat> stats = {
        {"issue-opened", "Issues", count(QStringLiteral("issueCount"))},
        {"comment", "Discussions", count(QStringLiteral("discussionCount"))},
        {"copy", "Copies", copies},
        {"database", "Storage", storage},
    };
    for (int index = 0; index < stats.size(); ++index) {
        const int row = index / 2;
        const int column = (index % 2) * 4;
        grid->addWidget(reposNextIconLabel(QString::fromLatin1(stats.at(index).icon),
                                           Theme::Role::FgSubtle, 14),
                        row, column, Qt::AlignVCenter);
        grid->addWidget(reposNextLabel(QString::fromLatin1(stats.at(index).key),
                                       "reposNextCardKey"),
                        row, column + 1);
        QLabel *value = reposNextLabel(stats.at(index).value, "reposNextCardValue");
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        grid->addWidget(value, row, column + 2);
    }
    grid->setColumnMinimumWidth(3, 12);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(5, 1);
    return cell;
}

QWidget *reposNextSparkCellFor(const QJsonObject &record, const QString &title,
                               const QString &seriesField,
                               const QString &totalField, const QString &tip)
{
    QFrame *cell = reposNextCell(false);
    cell->setToolTip(tip);
    auto *layout = new QVBoxLayout(cell);
    layout->setContentsMargins(20, 16, 20, 16);
    QVector<qint64> weeks;
    for (const QJsonValue &week : record.value(seriesField).toArray()) {
        const qint64 value = week.isDouble()
                                 ? qint64(week.toDouble())
                                 : week.toString().trimmed().toLongLong();
        weeks.append(qMax<qint64>(0, value));
    }
    qint64 total = -1;
    const QVariant counted = reposNextNumber(record, totalField);
    if (counted.isValid()) {
        total = counted.toLongLong();
    } else if (!weeks.isEmpty()) {
        total = 0;
        for (const qint64 value : std::as_const(weeks))
            total += value;
    }
    layout->addWidget(new ReposNextSparkCell(title, weeks, total));
    return cell;
}

QWidget *reposNextHostCell(const QJsonObject &record, const QTableWidgetItem *mirrors,
                           const QString &tip)
{
    QFrame *cell = reposNextCell(false);
    cell->setToolTip(tip);
    auto *layout = new QVBoxLayout(cell);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(6);
    const auto join = [](const QStringList &parts) {
        QStringList kept;
        for (const QString &part : parts)
            if (!part.trimmed().isEmpty())
                kept << part.trimmed();
        return kept.isEmpty() ? reposNextDash()
                              : kept.join(QString::fromUtf8(" \xC2\xB7 "));
    };
    const auto row = [layout](QWidget *left, QWidget *right) {
        auto *line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        line->addWidget(left, 1);
        if (right)
            line->addWidget(right);
        layout->addLayout(line);
    };

    QString machine = reposNextText(record, QStringLiteral("machineName"));
    if (machine.isEmpty()) {
        const QJsonArray members = record.value(QStringLiteral("_memberOwners")).toArray();
        if (!members.isEmpty())
            machine = members.first().toString().trimmed();
    }
    auto *head = new QWidget;
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(6);
    headLayout->addWidget(reposNextIconLabel(QStringLiteral("server"),
                                             Theme::Role::FgMuted, 14));
    auto *machineLabel =
        new ReposNextElidedLabel(machine.isEmpty() ? reposNextDash() : machine);
    machineLabel->setObjectName(QStringLiteral("reposNextCardHostName"));
    headLayout->addWidget(machineLabel, 1);
    QLabel *mirrorCount = reposNextLabel(reposNextMirrorsText(mirrors),
                                         "reposNextCardMirrors");
    mirrorCount->setToolTip(mirrors ? mirrors->toolTip() : QString());
    row(head, mirrorCount);

    const QString version = reposNextText(record, QStringLiteral("version"));
    QLabel *versionLabel = reposNextLabel(
        version.isEmpty() ? reposNextDash()
                          : (version.startsWith(QLatin1Char('v'))
                                 ? version
                                 : QStringLiteral("v") + version),
        "reposNextCardMeta");
    auto *system = new ReposNextElidedLabel(
        join({reposNextText(record, QStringLiteral("platform")),
              reposNextText(record, QStringLiteral("runtimeMode"))}));
    system->setObjectName(QStringLiteral("reposNextCardMeta"));
    row(system, versionLabel);

    const QVariant cpu = reposNextNumber(record, QStringLiteral("cpuPercent"));
    const QVariant diskUsed = reposNextNumber(record, QStringLiteral("diskUsedBytes"));
    const QVariant diskTotal = reposNextNumber(record, QStringLiteral("diskTotalBytes"));
    QLabel *cpuLabel = reposNextLabel(
        cpu.isValid() ? QStringLiteral("CPU %1%").arg(cpu.toLongLong())
                      : QStringLiteral("CPU ") + reposNextDash(),
        "reposNextCardMeta");
    QLabel *diskLabel = reposNextLabel(
        diskUsed.isValid() && diskTotal.isValid() && diskTotal.toLongLong() > 0
            ? QStringLiteral("Disk %1%").arg(
                  qRound(100.0 * diskUsed.toLongLong() / diskTotal.toLongLong()))
            : QStringLiteral("Disk ") + reposNextDash(),
        "reposNextCardMeta");
    diskLabel->setToolTip(
        diskUsed.isValid() && diskTotal.isValid()
            ? QStringLiteral("%1 of %2").arg(
                  SystemStats::formatBytes(diskUsed.toLongLong()),
                  SystemStats::formatBytes(diskTotal.toLongLong()))
            : QString());
    row(cpuLabel, diskLabel);

    const QString source = reposNextText(record, QStringLiteral("source"));
    const QVariant synced = reposNextNumber(record, QStringLiteral("lastSync"));
    QStringList syncParts;
    if (!source.isEmpty())
        syncParts << QStringLiteral("Sync via %1").arg(source);
    if (synced.isValid() && synced.toLongLong() > 0)
        syncParts << QStringLiteral("synced %1")
                         .arg(formatIssueRelativeTime(synced.toLongLong()));
    auto *sync = new ReposNextElidedLabel(join(syncParts));
    sync->setObjectName(QStringLiteral("reposNextCardMeta"));
    row(sync, nullptr);
    layout->addStretch(1);
    return cell;
}

QFrame *reposNextBuildCard(QTableWidget *table, int row, bool local)
{
    QTableWidgetItem *nameItem = table->item(row, kRepoColName);
    if (!nameItem)
        return nullptr;
    const QJsonObject record = nameItem->data(kRepoNextRecordRole).toJsonObject();
    const bool current = nameItem->data(kRepoNextCurrentRole).toBool();
    const auto cellTip = [table, row](int column) {
        const QTableWidgetItem *item = table->item(row, column);
        if (!item)
            return QString();
        return item->toolTip().isEmpty() ? item->text() : item->toolTip();
    };

    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("reposNextCard"));
    card->setProperty("reposRow", row);
    card->setAttribute(Qt::WA_Hover);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    auto *top = new QWidget;
    top->setObjectName(QStringLiteral("reposNextCardTop"));
    auto *topLayout = new QHBoxLayout(top);
    topLayout->setContentsMargins(20, 18, 16, 16);
    topLayout->setSpacing(14);

    const bool isPrivate = record.value(QStringLiteral("private")).toBool(false) ||
                           record.value(QStringLiteral("isPrivate")).toBool(false);
    auto *tile = new QLabel;
    tile->setObjectName(QStringLiteral("reposNextCardTile"));
    tile->setFixedSize(36, 36);
    tile->setAlignment(Qt::AlignCenter);
    tile->setPixmap(kit::tintedIcon(local && isPrivate ? QStringLiteral("lock")
                                                       : QStringLiteral("git-branch"),
                                    Theme::color(Theme::Role::FgMuted), 16,
                                    tile->devicePixelRatioF()));
    topLayout->addWidget(tile, 0, Qt::AlignTop);

    auto *info = new QVBoxLayout;
    info->setContentsMargins(0, 0, 0, 0);
    info->setSpacing(4);
    auto *nameRow = new QHBoxLayout;
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(8);
    auto *name = new ReposNextElidedLabel(
        reposNextText(record, QStringLiteral("owner")) + QLatin1Char('/') +
        reposNextText(record, QStringLiteral("name")));
    name->setObjectName(QStringLiteral("reposNextCardName"));
    nameRow->addWidget(name);
    const QStringList lines =
        nameItem->data(kRepoDetailedTextRole).toString().split(QLatin1Char('\n'));
    if (lines.size() > 1 && lines.at(1).startsWith(QLatin1String("Access:"))) {
        const QStringList parts =
            lines.at(1).mid(7).split(QString::fromUtf8(" \xC2\xB7 "), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            const QString trimmed = part.trimmed();
            if (!trimmed.isEmpty() && trimmed != reposNextDash())
                nameRow->addWidget(reposNextPill(trimmed, reposNextPillTone(trimmed)));
        }
    }
    if (local) {
        QStringList compact = nameItem->data(kRepoCompactTextRole)
                                  .toString()
                                  .split(QStringLiteral(" | "));
        const QString state = compact.size() > 1 ? compact.at(1) : QString();
        QLabel *dirty = reposNextPill(state, state.startsWith(QLatin1String("Dirty"))
                                                 ? "warning"
                                                 : "neutral");
        dirty->setObjectName(QStringLiteral("reposNextCardDirty"));
        dirty->setVisible(!state.isEmpty());
        nameRow->addWidget(dirty);
    }
    if (current)
        nameRow->addWidget(reposNextPill(QStringLiteral("Current"), "accent"));
    nameRow->addStretch(1);
    info->addLayout(nameRow);

    const QString description = reposNextText(record, QStringLiteral("description"));
    if (!description.isEmpty()) {
        auto *label = new ReposNextElidedLabel(description);
        label->setObjectName(QStringLiteral("reposNextCardDesc"));
        info->addWidget(label);
    }
    const QString path = reposNextText(record, QStringLiteral("_localForkPath"));
    if (local && !path.isEmpty()) {
        auto *label = new ReposNextElidedLabel(path);
        label->setObjectName(QStringLiteral("reposNextCardPath"));
        label->setFont(Theme::monospaceFont(label->font()));
        info->addWidget(label);
    }
    const QString subject = reposNextText(record, QStringLiteral("commitSubject"));
    if (!subject.isEmpty()) {
        auto *commitRow = new QHBoxLayout;
        commitRow->setContentsMargins(0, 4, 0, 0);
        commitRow->setSpacing(6);
        const QString author = reposNextText(record, QStringLiteral("commitAuthorName"));
        if (!author.isEmpty()) {
            auto *avatar = new QLabel;
            avatar->setObjectName(QStringLiteral("reposNextCardAvatar"));
            avatar->setFixedSize(18, 18);
            avatar->setPixmap(reposNextInitialsPixmap(author, avatar->devicePixelRatioF()));
            avatar->setToolTip(author);
            commitRow->addWidget(avatar);
        }
        auto *message = new ReposNextElidedLabel(subject);
        message->setObjectName(QStringLiteral("reposNextCardCommit"));
        commitRow->addWidget(message);
        const QVariant at = reposNextNumber(record, QStringLiteral("commitAt"));
        if (at.isValid() && at.toLongLong() > 0) {
            QLabel *age = reposNextLabel(
                QString::fromUtf8("\xC2\xB7 %1").arg(formatIssueRelativeTime(at.toLongLong())),
                "reposNextCardAge");
            age->setToolTip(formatRepoDate(at.toLongLong()));
            commitRow->addWidget(age);
        }
        commitRow->addStretch(1);
        info->addLayout(commitRow);
    }
    topLayout->addLayout(info, 1);

    auto *actionsRow = new QHBoxLayout;
    actionsRow->setContentsMargins(0, 0, 0, 0);
    actionsRow->setSpacing(4);
    QWidget *actionsCell = table->cellWidget(row, kRepoColActions);
    const QList<QPushButton *> sources =
        actionsCell ? actionsCell->findChildren<QPushButton *>(
                          QString(), Qt::FindDirectChildrenOnly)
                    : QList<QPushButton *>();
    QList<QPushButton *> overflow;
    for (int index = 0; index < sources.size(); ++index) {
        QPushButton *source = sources.at(index);
        if (index == 0) {
            actionsRow->addWidget(reposNextCardButton(
                source, current ? QStringLiteral("Open") : source->text(),
                current ? "outline" : "inverse",
                current ? Theme::Role::Fg : Theme::Role::Bg));
        } else if (index <= 2 &&
                   source->objectName() != QLatin1String("dangerButton")) {
            actionsRow->addWidget(reposNextCardButton(source, source->text(), "ghost",
                                                      Theme::Role::FgMuted));
        } else {
            overflow.append(source);
        }
    }
    if (!overflow.isEmpty())
        actionsRow->addWidget(reposNextMoreButton(overflow));
    topLayout->addLayout(actionsRow);
    topLayout->setAlignment(actionsRow, Qt::AlignTop);
    cardLayout->addWidget(top);

    auto *band = new QFrame;
    band->setObjectName(QStringLiteral("reposNextCardBand"));
    auto *bandLayout = new QHBoxLayout(band);
    bandLayout->setContentsMargins(0, 0, 0, 0);
    bandLayout->setSpacing(0);
    bandLayout->addWidget(reposNextStatsCell(record, cellTip(kRepoColContents)), 11);
    const auto chartTip = [table, row](int column) {
        QWidget *chart = table->cellWidget(row, column);
        if (chart)
            return chart->toolTip();
        const QTableWidgetItem *item = table->item(row, column);
        return item ? item->toolTip() : QString();
    };
    bandLayout->addWidget(reposNextSparkCellFor(record, QStringLiteral("Commits"),
                                                QStringLiteral("activityWeeks"),
                                                QStringLiteral("commitCount"),
                                                chartTip(kRepoColActivity)),
                          10);
    bandLayout->addWidget(reposNextSparkCellFor(record, QStringLiteral("Merges"),
                                                QStringLiteral("mergeActivityWeeks"),
                                                QStringLiteral("mergeCount"),
                                                chartTip(kRepoColMergeActivity)),
                          10);
    bandLayout->addWidget(reposNextHostCell(record, table->item(row, kRepoColMirrors),
                                            cellTip(kRepoColHost)),
                          11);
    cardLayout->addWidget(band);

    return card;
}

void reposNextRebuildCards(QTableWidget *table, bool local)
{
    if (!Theme::isNext())
        return;
    QWidget *list = reposNextCardList(table);
    auto *layout = list ? qobject_cast<QVBoxLayout *>(list->layout()) : nullptr;
    if (!layout)
        return;
    for (QFrame *card : reposNextCards(list)) {
        layout->removeWidget(card);
        card->hide();
        card->deleteLater();
    }
    for (int row = 0; row < table->rowCount(); ++row) {
        if (QFrame *card = reposNextBuildCard(table, row, local))
            layout->insertWidget(layout->count() - 1, card);
    }
}

void reposNextSyncCardMirrors(QTableWidget *table, int row)
{
    if (!Theme::isNext() || !table)
        return;
    const QTableWidgetItem *item = table->item(row, kRepoColMirrors);
    for (QFrame *card : reposNextCards(reposNextCardList(table))) {
        if (card->property("reposRow").toInt() != row)
            continue;
        if (auto *label = card->findChild<QLabel *>(QStringLiteral("reposNextCardMirrors"))) {
            label->setText(reposNextMirrorsText(item));
            label->setToolTip(item ? item->toolTip() : QString());
        }
    }
}

void reposNextSyncLocalDirty(QTableWidget *table)
{
    if (!Theme::isNext() || !table)
        return;
    for (QFrame *card : reposNextCards(reposNextCardList(table))) {
        const QTableWidgetItem *item =
            table->item(card->property("reposRow").toInt(), kRepoColName);
        auto *label = card->findChild<QLabel *>(QStringLiteral("reposNextCardDirty"));
        if (!item || !label)
            continue;
        const QStringList compact =
            item->data(kRepoCompactTextRole).toString().split(QStringLiteral(" | "));
        const QString state = compact.size() > 1 ? compact.at(1) : QString();
        label->setText(state);
        label->setVisible(!state.isEmpty());
        label->setProperty("tone", state.startsWith(QLatin1String("Dirty"))
                                       ? QStringLiteral("warning")
                                       : QStringLiteral("neutral"));
        label->style()->unpolish(label);
        label->style()->polish(label);
    }
}

void reposNextApplyFilter(QTableWidget *table)
{
    if (!table || !Theme::isNext())
        return;
    const QString needle =
        table->property("reposNextFilterText").toString().trimmed();
    for (int row = 0; row < table->rowCount(); ++row) {
        bool match = needle.isEmpty();
        if (!match) {
            if (const QTableWidgetItem *item = table->item(row, kRepoColName)) {
                QString haystack = item->data(kRepoDetailedTextRole).toString();
                if (haystack.isEmpty())
                    haystack = item->text();
                match = haystack.contains(needle, Qt::CaseInsensitive) ||
                        item->data(Qt::UserRole).toString().contains(
                            needle, Qt::CaseInsensitive);
            }
        }
        table->setRowHidden(row, !match);
    }
    QWidget *list = reposNextCardList(table);
    if (!list)
        return;
    int shown = 0;
    for (QFrame *card : reposNextCards(list)) {
        const int row = card->property("reposRow").toInt();
        const bool visible = row >= 0 && row < table->rowCount() &&
                             !table->isRowHidden(row);
        card->setVisible(visible);
        if (visible)
            ++shown;
    }
    if (auto *empty = list->findChild<QLabel *>(QStringLiteral("reposNextCardEmpty"))) {
        empty->setText(table->rowCount() == 0
                           ? QStringLiteral("No repositories to show.")
                           : QStringLiteral("No repositories match the filter."));
        empty->setVisible(shown == 0);
    }
}

void reposNextSetTabCount(QTableWidget *table, int count)
{
    if (!Theme::isNext() || !table)
        return;
    QWidget *tabPage = table->parentWidget();
    QTabWidget *tabs = nullptr;
    for (QWidget *widget = tabPage ? tabPage->parentWidget() : nullptr; widget;
         widget = widget->parentWidget()) {
        if ((tabs = qobject_cast<QTabWidget *>(widget)))
            break;
    }
    if (!tabs)
        return;
    const int index = tabs->indexOf(tabPage);
    if (index < 0)
        return;
    if (auto *pill = qobject_cast<QLabel *>(
            tabs->tabBar()->tabButton(index, QTabBar::RightSide))) {
        pill->setText(formatCount(count));
        pill->setVisible(true);
    }
}

QScrollArea *reposNextCardArea(const QString &objectName, QWidget *parent)
{
    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName(objectName);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *list = new QWidget;
    list->setObjectName(QStringLiteral("reposNextCardList"));
    auto *layout = new QVBoxLayout(list);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    QLabel *empty = reposNextLabel(QStringLiteral("No repositories to show."),
                                   "reposNextCardEmpty", list);
    empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(empty);
    layout->addStretch(1);
    scroll->setWidget(list);
    return scroll;
}

void reposNextApplyMode(QWidget *page, bool cards)
{
    if (!page)
        return;
    page->setProperty("reposNextCards", cards);
    const auto tables = page->findChildren<QTableWidget *>();
    for (QTableWidget *table : tables) {
        if (!table->property("reposNext").toBool())
            continue;
        table->setMinimumHeight(0);
        table->setMaximumHeight(cards ? 0 : QWIDGETSIZE_MAX);
        QWidget *tabPage = table->parentWidget();
        if (!tabPage)
            continue;
        const auto areas = tabPage->findChildren<QScrollArea *>(
            QString(), Qt::FindDirectChildrenOnly);
        for (QScrollArea *area : areas)
            if (area->objectName().startsWith(QLatin1String("reposNextCards")))
                area->setVisible(cards);
    }
}
} // namespace

QWidget *MainWindow::buildNetworkReposSection()
{
    const bool next = Theme::isNext();
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);
    if (next) {
        page->setProperty("reposNextPage", true);
        outer->setContentsMargins(24, 16, 24, 16);
        outer->setSpacing(14);
    }

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(10);
    QHBoxLayout *titleRow = header;
    QVBoxLayout *titleColumn = nullptr;
    if (next) {
        header->setSpacing(6);
        titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        titleRow = new QHBoxLayout;
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(6);
        titleColumn->addLayout(titleRow);
        header->addLayout(titleColumn, 1);
    }

    auto *title = new QLabel(next ? QStringLiteral("Repositories")
                                  : QStringLiteral("Repos"));
    title->setObjectName("sectionTitle");
    QFont titleFont = title->font();
    titleFont = forkmesh::resizedFont(titleFont, 4);
    titleFont.setBold(true);
    if (next) {
        titleFont = title->font();
        titleFont.setPixelSize(15);
        titleFont.setWeight(QFont::DemiBold);
    }
    title->setFont(titleFont);
    titleRow->addWidget(title);

    // QStringLiteral would widen these UTF-8 bytes one by one into mojibake.
    const QString pageDescription = QString::fromUtf8(
        "Repositories grouped by user or organization across the active relay. "
        "Mirror hosts are combined into one repository row \xE2\x80\x94 who and "
        "what it is, what it contains, and the machine serving it, each on its "
        "own line. Hover a cell for the URLs, digests and exact timestamps "
        "behind it, or use a column's \xE2\x8B\xAF menu to hide or move it.");
    if (next) {
        titleRow->addWidget(reposNextHelpButton(
            QStringLiteral("About repositories"), pageDescription));
        titleRow->addStretch(1);
        auto *summary = new QLabel(QStringLiteral(
            "Grouped by owner across the active relay. Mirror hosts fold into "
            "one row per repository."));
        summary->setObjectName("mutedLabel");
        summary->setProperty("reposRole", QStringLiteral("summary"));
        summary->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        titleColumn->addWidget(summary);
    } else {
        header->addStretch();
    }

    m_networkReposStatus = new QLabel(QStringLiteral("Not loaded"));
    m_networkReposStatus->setObjectName("mutedLabel");
    if (!next)
        header->addWidget(m_networkReposStatus);

    // fromUtf8, not QStringLiteral: the ellipsis was rendering as mojibake.
    auto *newRepoButton =
        new QPushButton(QString::fromUtf8("New repository\xE2\x80\xA6"));
    newRepoButton->setObjectName("primaryButton");
    newRepoButton->setCursor(Qt::PointingHandCursor);
    newRepoButton->setToolTip(QStringLiteral("Create a brand-new repository"));
    setOcticon(newRepoButton, "repo", 14);
    connect(newRepoButton, &QPushButton::clicked, this,
            &MainWindow::createNewRepository);
    if (!next)
        header->addWidget(newRepoButton);

    auto *importLocalButton = new QPushButton(
        QString::fromUtf8("Import folder\xE2\x80\xA6"));
    importLocalButton->setObjectName("ghostButton");
    importLocalButton->setCursor(Qt::PointingHandCursor);
    importLocalButton->setToolTip(QStringLiteral(
        "Add one Git checkout or every Git repository directly inside a folder"));
    setOcticon(importLocalButton, "file-directory", 14);
    connect(importLocalButton, &QPushButton::clicked, this,
            &MainWindow::promptAddRepository);
    if (!next)
        header->addWidget(importLocalButton);

    auto *viewButton = new QPushButton(QStringLiteral("Simple list"));
    viewButton->setObjectName("ghostButton");
    viewButton->setCheckable(true);
    viewButton->setCursor(Qt::PointingHandCursor);
    viewButton->setToolTip(QStringLiteral(
        "Switch between a one-line repository list and the full information grid"));
    setOcticon(viewButton, "list-unordered", 14);
    if (!next)
        header->addWidget(viewButton);

    m_networkReposRefreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_networkReposRefreshButton->setObjectName("ghostButton");
    m_networkReposRefreshButton->setCursor(Qt::PointingHandCursor);
    m_networkReposRefreshButton->setToolTip(QStringLiteral("Refresh network repositories"));
    setOcticon(m_networkReposRefreshButton, "sync", 14);
    connect(m_networkReposRefreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshNetworkReposPage);
    QPushButton *cardsViewButton = nullptr;
    if (next) {
        auto *viewToggle = new QFrame;
        viewToggle->setObjectName(QStringLiteral("reposNextViewToggle"));
        auto *viewToggleLayout = new QHBoxLayout(viewToggle);
        viewToggleLayout->setContentsMargins(2, 2, 2, 2);
        viewToggleLayout->setSpacing(2);
        auto *viewGroup = new QButtonGroup(viewToggle);
        viewGroup->setExclusive(true);
        const qreal ratio = page->devicePixelRatioF();
        for (int index = 0; index < 2; ++index) {
            auto *button = new QPushButton;
            button->setObjectName(QStringLiteral("reposNextViewButton"));
            button->setCheckable(true);
            button->setCursor(Qt::PointingHandCursor);
            button->setFixedSize(28, 28);
            QIcon icon;
            if (index == 0) {
                icon.addPixmap(reposNextGridPixmap(Theme::color(Theme::Role::FgMuted), ratio),
                               QIcon::Normal, QIcon::Off);
                icon.addPixmap(reposNextGridPixmap(Theme::color(Theme::Role::Fg), ratio),
                               QIcon::Normal, QIcon::On);
                button->setToolTip(QStringLiteral("Cards"));
                button->setAccessibleName(QStringLiteral("Cards"));
                cardsViewButton = button;
            } else {
                icon.addPixmap(kit::tintedIcon(QStringLiteral("list-unordered"),
                                               Theme::color(Theme::Role::FgMuted), 14, ratio),
                               QIcon::Normal, QIcon::Off);
                icon.addPixmap(kit::tintedIcon(QStringLiteral("list-unordered"),
                                               Theme::color(Theme::Role::Fg), 14, ratio),
                               QIcon::Normal, QIcon::On);
                button->setToolTip(QStringLiteral(
                    "Table: every column, with the column menu and Simple list"));
                button->setAccessibleName(QStringLiteral("Table"));
            }
            button->setIcon(icon);
            button->setIconSize(QSize(14, 14));
            viewGroup->addButton(button);
            viewToggleLayout->addWidget(button);
        }
        cardsViewButton->setChecked(true);
        header->addWidget(viewToggle);
        header->addSpacing(4);
        for (QPushButton *button :
             {viewButton, m_networkReposRefreshButton, importLocalButton}) {
            reposNextButtonRole(button, "ghost", Theme::Role::FgMuted);
            header->addWidget(button);
        }
        newRepoButton->setProperty("reposRole", QStringLiteral("inverse"));
        newRepoButton->setProperty("nextIconRole", int(Theme::Role::Bg));
        setOcticon(newRepoButton, "plus", 14);
        header->addWidget(newRepoButton);
    } else {
        header->addWidget(m_networkReposRefreshButton);
    }
    outer->addLayout(header);

    if (!next) {
        auto *subtitle = new QLabel(pageDescription);
        subtitle->setObjectName("mutedLabel");
        subtitle->setWordWrap(true);
        outer->addWidget(subtitle);
    }

    auto *repoTabs = new QTabWidget;
    auto *remotePage = new QWidget;
    auto *remoteLayout = new QVBoxLayout(remotePage);
    remoteLayout->setContentsMargins(0, 8, 0, 0);
    remoteLayout->setSpacing(8);
    if (next) {
        repoTabs->setProperty("reposNext", true);
        remoteLayout->setContentsMargins(0, 16, 0, 0);
        remoteLayout->setSpacing(12);
        auto *remoteBar = new QHBoxLayout;
        remoteBar->setContentsMargins(0, 0, 0, 0);
        remoteBar->setSpacing(8);
        QLineEdit *remoteFilter =
            reposNextFilterField(QStringLiteral("reposNextFilter"), remotePage);
        connect(remoteFilter, &QLineEdit::textChanged, this,
                [this](const QString &text) {
                    if (!m_networkReposTable)
                        return;
                    m_networkReposTable->setProperty("reposNextFilterText", text);
                    reposNextApplyFilter(m_networkReposTable);
                });
        remoteBar->addWidget(remoteFilter);
        remoteBar->addStretch(1);
        m_networkReposStatus->setProperty("reposRole", QStringLiteral("status"));
        m_networkReposStatus->setTextFormat(Qt::PlainText);
        remoteBar->addWidget(m_networkReposStatus);
        remoteLayout->addLayout(remoteBar);
    } else {
        remoteLayout->addWidget(m_networkReposStatus);
    }

    m_networkReposTable = new QTableWidget(0, kRepoColCount);
    installColumnHeaderMenu(m_networkReposTable);
    m_networkReposTable->setObjectName("issueTable");
    m_networkReposTable->setHorizontalHeaderLabels(networkRepoHeaders());
    m_networkReposTable->verticalHeader()->setVisible(false);
    m_networkReposTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_networkReposTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_networkReposTable->setAlternatingRowColors(true);
    m_networkReposTable->setSortingEnabled(false);
    m_networkReposTable->setHorizontalScrollMode(
        QAbstractItemView::ScrollPerPixel);
    m_networkReposTable->setItemDelegate(
        new NetworkRepoRowDelegate(m_networkReposTable));
    configureNetworkRepoGrid(m_networkReposTable);
    reposNextStyleTable(m_networkReposTable);
    connect(m_networkReposTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int column) {
                if (column == kRepoColActions || !m_networkReposTable)
                    return;
                QTableWidgetItem *item =
                    m_networkReposTable->item(row, kRepoColName);
                if (!item)
                    return;
                const QString key = item->data(Qt::UserRole).toString();
                const QString owner =
                    item->data(Qt::UserRole + 3).toString();
                const QString name =
                    item->data(Qt::UserRole + 4).toString();
                if (owner.isEmpty() || name.isEmpty())
                    return;
                openNetworkRepo(owner, name,
                                item->data(Qt::UserRole + 1).toString(),
                                item->data(Qt::UserRole + 2).toBool());
            });
    remoteLayout->addWidget(m_networkReposTable, 1);
    if (next)
        remoteLayout->addWidget(
            reposNextCardArea(QStringLiteral("reposNextCards"), remotePage), 1);
    repoTabs->addTab(remotePage, next ? QStringLiteral("Remote")
                                      : QStringLiteral("Remote repositories"));

    // A private repository is never advertised, and a repository that was never
    // published is not in the catalog either, so neither can appear in the table
    // above no matter how long you refresh it. They are on this machine all the
    // same, so they get their own table with the same columns.
    const QString localDescription = QString::fromUtf8(
        "Repositories this machine holds that the relay does not list above "
        "\xE2\x80\x94 private ones, which are never advertised publicly, and "
        "local-only ones you have not published. Their contents are read from "
        "the copy on this disk, so there is no host or mirror line to show.");
    auto *localHeader = new QHBoxLayout;
    localHeader->setContentsMargins(0, 6, 0, 0);
    localHeader->setSpacing(10);
    auto *localPage = new QWidget;
    if (next) {
        localHeader->setContentsMargins(0, 0, 0, 0);
        localHeader->setSpacing(8);
        QLineEdit *localFilter = reposNextFilterField(
            QStringLiteral("reposNextLocalFilter"), localPage);
        connect(localFilter, &QLineEdit::textChanged, this,
                [this](const QString &text) {
                    if (!m_localReposTable)
                        return;
                    m_localReposTable->setProperty("reposNextFilterText", text);
                    reposNextApplyFilter(m_localReposTable);
                });
        localHeader->addWidget(localFilter);
    } else {
        auto *localTitle = new QLabel(QStringLiteral("Private & local"));
        localTitle->setObjectName("sectionTitle");
        QFont localTitleFont = localTitle->font();
        localTitleFont = forkmesh::resizedFont(localTitleFont, 1);
        localTitleFont.setBold(true);
        localTitle->setFont(localTitleFont);
        localHeader->addWidget(localTitle);
    }
    localHeader->addStretch();
    m_localReposStatus = new QLabel(QStringLiteral("Not loaded"));
    m_localReposStatus->setObjectName("mutedLabel");
    localHeader->addWidget(m_localReposStatus);
    if (next) {
        m_localReposStatus->setProperty("reposRole", QStringLiteral("status"));
        m_localReposStatus->setTextFormat(Qt::PlainText);
        localHeader->addWidget(reposNextHelpButton(
            QStringLiteral("About private and local repositories"),
            localDescription));
    }
    auto *localLayout = new QVBoxLayout(localPage);
    localLayout->setContentsMargins(0, 8, 0, 0);
    localLayout->setSpacing(8);
    if (next) {
        localLayout->setContentsMargins(0, 16, 0, 0);
        localLayout->setSpacing(12);
    }
    localLayout->addLayout(localHeader);

    if (!next) {
        auto *localSubtitle = new QLabel(localDescription);
        localSubtitle->setObjectName("mutedLabel");
        localSubtitle->setWordWrap(true);
        localLayout->addWidget(localSubtitle);
    }

    m_localReposTable = new QTableWidget(0, kRepoColCount);
    installColumnHeaderMenu(m_localReposTable);
    m_localReposTable->setObjectName("issueTable");
    m_localReposTable->setHorizontalHeaderLabels(networkRepoHeaders());
    m_localReposTable->verticalHeader()->setVisible(false);
    m_localReposTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_localReposTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_localReposTable->setAlternatingRowColors(true);
    m_localReposTable->setSortingEnabled(false);
    m_localReposTable->setHorizontalScrollMode(
        QAbstractItemView::ScrollPerPixel);
    m_localReposTable->setItemDelegate(
        new NetworkRepoRowDelegate(m_localReposTable));
    configureNetworkRepoGrid(m_localReposTable);
    reposNextStyleTable(m_localReposTable);
    connect(m_localReposTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int column) {
                if (column == kRepoColActions || !m_localReposTable)
                    return;
                QTableWidgetItem *item = m_localReposTable->item(row, kRepoColName);
                if (!item)
                    return;
                const int index =
                    findNetworkRepoIndex(item->data(Qt::UserRole + 3).toString(),
                                         item->data(Qt::UserRole + 4).toString(),
                                         true);
                if (index >= 0)
                    openRepoDetail(index);
            });
    localLayout->addWidget(m_localReposTable, 1);
    if (next)
        localLayout->addWidget(
            reposNextCardArea(QStringLiteral("reposNextCardsLocal"), localPage), 1);
    repoTabs->addTab(localPage, next ? QStringLiteral("Private and local")
                                     : QStringLiteral("Private / local"));
    outer->addWidget(repoTabs, 1);
    if (next) {
        for (int index = 0; index < repoTabs->count(); ++index) {
            QLabel *count = reposNextLabel(QString(), "reposNextTabCount");
            count->setAlignment(Qt::AlignCenter);
            count->setVisible(false);
            repoTabs->tabBar()->setTabButton(index, QTabBar::RightSide, count);
        }
        const auto applyMode = [page, viewButton](bool cards) {
            reposNextApplyMode(page, cards);
            viewButton->setVisible(!cards);
        };
        connect(cardsViewButton, &QPushButton::toggled, page,
                [applyMode](bool cards) { applyMode(cards); });
        applyMode(true);
    }

    connect(viewButton, &QPushButton::toggled, this,
            [this, viewButton](bool compact) {
                viewButton->setText(compact ? QStringLiteral("Detailed view")
                                            : QStringLiteral("Simple list"));
                for (QTableWidget *table : {m_networkReposTable,
                                            m_localReposTable}) {
                    if (!table)
                        continue;
                    table->setProperty("compactRepoList", compact);
                    for (int column = 1; column < kRepoColActions; ++column)
                        table->setColumnHidden(column, compact);
                    for (int row = 0; row < table->rowCount(); ++row) {
                        if (QTableWidgetItem *item =
                                table->item(row, kRepoColName)) {
                            const QString replacement = item->data(
                                compact ? kRepoCompactTextRole
                                        : kRepoDetailedTextRole).toString();
                            if (!replacement.isEmpty())
                                item->setText(replacement);
                        }
                        if (QWidget *actions = table->cellWidget(
                                row, kRepoColActions)) {
                            const auto buttons =
                                actions->findChildren<QPushButton *>();
                            for (QPushButton *button : buttons)
                                button->setVisible(!compact ||
                                                   button->text() ==
                                                       QLatin1String("Switch"));
                        }
                    }
                    const bool nextRows = Theme::isNext();
                    table->verticalHeader()->setDefaultSectionSize(
                        compact ? (nextRows ? kRepoNextCompactRowHeight
                                            : qMax(30, table->fontMetrics().height() + 12))
                                : networkRepoRowHeight(table->font()));
                    table->verticalHeader()->setMinimumSectionSize(
                        compact ? (nextRows ? kRepoNextCompactRowHeight : 24)
                                : networkRepoRowHeight(table->font()));
                    table->resizeRowsToContents();
                }
            });

    // Node -> user ownership lets renderNetworkRepos collapse machine
    // namespaces into user namespaces even when those nodes are offline.
    refreshChatUserDirectory();
    return page;
}

void MainWindow::refreshNetworkReposPage()
{
    if (!m_networkReposTable || !m_networkReposStatus)
        return;
    // The local table's git-derived columns are refreshed by the advert pass,
    // which stands down while this page is hidden; catch up now that it is up.
    if (m_mirrorAdvertStale)
        refreshMirrorAdverts();
    if (!m_networkAccess) {
        m_networkReposStatus->setText(QStringLiteral("Network is not connected."));
        return;
    }

    const int generation = ++m_networkReposLoadGen;
    m_networkReposTable->setRowCount(0);
    m_networkReposStatus->setText(QStringLiteral("Loading repositories..."));
    if (m_networkReposRefreshButton)
        m_networkReposRefreshButton->setEnabled(false);

    const auto localCatalog = [this]() -> QJsonArray {
        QJsonArray repos;
        const QString localOwner = accountOwner().trimmed();
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (repo.previewOnly || !repositoryBelongsToActiveRelay(repo) ||
                repo.owner.trimmed().isEmpty() ||
                repo.name.trimmed().isEmpty())
                continue;
            if (repo.localPath.trimmed().isEmpty() &&
                repo.mirrorPath.trimmed().isEmpty())
                continue;
            const QString owner = repo.owner.trimmed();
            const QString name = repo.name.trimmed();
            QJsonObject localRepo;
            localRepo.insert(QStringLiteral("owner"), owner);
            localRepo.insert(QStringLiteral("name"), name);
            localRepo.insert(QStringLiteral("source"), QStringLiteral("local-node"));
            localRepo.insert(QStringLiteral("servingOwner"), owner);
            const QString cloneUrl = repo.cloneUrl.trimmed();
            localRepo.insert(
                QStringLiteral("cloneUrl"),
                cloneUrl.isEmpty() ? hostedCloneUrl(owner, name) : cloneUrl);
            localRepo.insert(QStringLiteral("private"), repo.isPrivate);
            localRepo.insert(QStringLiteral("isPrivate"), repo.isPrivate);
            localRepo.insert(QStringLiteral("machineName"), localOwner);
            repos.append(localRepo);
        }
        return repos;
    };

    QNetworkRequest request(catalogListUrl());
    request.setRawHeader("accept", "application/json");
    const QString catalogOrigin = activeRelayOrigin();
    QNetworkReply *reply = m_networkAccess->get(request);
	connect(reply, &QNetworkReply::finished, this,
	        [this, reply, localCatalog, generation, catalogOrigin] {
        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError error = reply->error();
        const QString errorString = reply->errorString();
        reply->deleteLater();
        if (generation != m_networkReposLoadGen)
            return;
        if (m_networkReposRefreshButton)
            m_networkReposRefreshButton->setEnabled(true);
        if (error != QNetworkReply::NoError) {
            if (m_networkReposStatus) {
                const QJsonArray localRepos = localCatalog();
                if (!localRepos.isEmpty()) {
                    m_networkReposStatus->setText(
                        QStringLiteral("Showing local repositories (website unavailable): %1")
                            .arg(errorString));
                    renderNetworkRepos(localRepos);
                    return;
                }
                m_networkReposStatus->setText(
                    QStringLiteral("Repositories unavailable: %1")
                        .arg(errorString));
                // Nothing above stands for anything on this machine now, so the
                // local table has to be rebuilt from scratch rather than left
                // showing what the last successful load left out.
                m_networkReposCoveredLocal.clear();
                renderLocalOnlyRepos();
            }
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(body).object();
        const QJsonArray catalog = obj.value("repositories").toArray();
        // The same response is the relay's repository list; keep the
        // remembered copy current without a second request.
        if (obj.value("repositories").isArray())
            applyRelayRepositoryCatalog(catalogOrigin, catalog);

        QUrl orgsUrl = catalogApiUrl();
        orgsUrl.setPath(QStringLiteral("/api/world/organizations"));
        orgsUrl.setQuery(QString());
        QNetworkReply *orgsReply =
            m_networkAccess->get(QNetworkRequest(orgsUrl));
        connect(orgsReply, &QNetworkReply::finished, this,
                [this, orgsReply, catalog, generation] {
            const QByteArray orgBody = orgsReply->readAll();
            const bool orgsOk =
                orgsReply->error() == QNetworkReply::NoError;
            orgsReply->deleteLater();
            if (generation != m_networkReposLoadGen)
                return;
            const QJsonArray organizations =
                QJsonDocument::fromJson(orgBody)
                    .object()
                    .value(QStringLiteral("organizations"))
                    .toArray();
            if (!orgsOk || organizations.isEmpty()) {
                renderNetworkRepos(catalog);
                return;
            }

            auto combined = std::make_shared<QJsonArray>(catalog);
            auto pending = std::make_shared<int>(0);
            // First catalog row per case-insensitive owner/name, so resolving
            // each organization alias is a lookup, not a catalog rescan.
            QHash<QString, int> catalogIndex;
            for (int i = 0; i < catalog.size(); ++i) {
                const QJsonObject base = catalog.at(i).toObject();
                const QString key =
                    base.value(QStringLiteral("owner")).toString().toCaseFolded() +
                    QLatin1Char('\x1f') +
                    base.value(QStringLiteral("name")).toString().toCaseFolded();
                if (!catalogIndex.contains(key))
                    catalogIndex.insert(key, i);
            }
            for (const QJsonValue &value : organizations) {
                const QString organization =
                    value.toObject().value(QStringLiteral("name"))
                        .toString().trimmed().toLower();
                if (organization.isEmpty())
                    continue;
                ++*pending;
                QUrl reposUrl = catalogApiUrl();
                reposUrl.setPath(QStringLiteral("/api/orgs/%1/repos")
                                     .arg(organization));
                reposUrl.setQuery(QString());
                QNetworkReply *reposReply =
                    m_networkAccess->get(QNetworkRequest(reposUrl));
                connect(reposReply, &QNetworkReply::finished, this,
                        [this, reposReply, catalog, catalogIndex, combined,
                         pending, organization, generation] {
                    const QByteArray repoBody = reposReply->readAll();
                    const bool reposOk =
                        reposReply->error() == QNetworkReply::NoError;
                    reposReply->deleteLater();
                    if (generation != m_networkReposLoadGen)
                        return;
                    if (reposOk) {
                        const QJsonArray aliases =
                            QJsonDocument::fromJson(repoBody)
                                .object()
                                .value(QStringLiteral("repos"))
                                .toArray();
                        for (const QJsonValue &aliasValue : aliases) {
                            const QJsonObject alias = aliasValue.toObject();
                            const QString name =
                                alias.value(QStringLiteral("repo"))
                                    .toString().trimmed();
                            const QString node =
                                alias.value(QStringLiteral("node"))
                                    .toString().trimmed();
                            if (name.isEmpty() || node.isEmpty())
                                continue;
                            const auto match = catalogIndex.constFind(
                                node.toCaseFolded() + QLatin1Char('\x1f') +
                                name.toCaseFolded());
                            if (match == catalogIndex.constEnd())
                                continue;
                            QJsonObject publicAlias =
                                catalog.at(match.value()).toObject();
                            publicAlias.insert(QStringLiteral("owner"),
                                               organization);
                            publicAlias.insert(QStringLiteral("name"), name);
                            publicAlias.insert(
                                QStringLiteral("source"),
                                QStringLiteral("organization-alias"));
                            publicAlias.insert(
                                QStringLiteral("servingOwner"), node);
                            publicAlias.insert(
                                QStringLiteral("cloneUrl"),
                                hostedCloneUrl(organization, name));
                            combined->append(publicAlias);
                        }
                    }
                    --*pending;
                    if (*pending == 0)
                        renderNetworkRepos(*combined);
                });
            }
            if (*pending == 0)
                renderNetworkRepos(catalog);
        });
    });
}

int MainWindow::findNetworkRepoIndex(const QString &owner, const QString &name,
                                     bool includePreview) const
{
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (repo.owner == owner && repo.name == name &&
            (includePreview || !repo.previewOnly) &&
            repositoryBelongsToActiveRelay(repo))
            return i;
    }
    return -1;
}

int MainWindow::findNetworkLocalForkIndex(const QString &owner,
                                          const QString &name) const
{
    const QString localOwner = accountOwner();
    if (!localOwner.isEmpty()) {
        const int forkIndex = findNetworkRepoIndex(localOwner, name, false);
        if (forkIndex >= 0 && !m_repositories.at(forkIndex).localPath.isEmpty())
            return forkIndex;
    }

    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (repositoryBelongsToActiveRelay(repo) && !repo.previewOnly &&
            repo.owner == owner && repo.name == name && !repo.localPath.isEmpty())
            return i;
    }
    return -1;
}

namespace {

// Fold every member of one repository group into its canonical record: the
// largest commit/merge counts and the per-week maxima of the activity series.
void mergeNetworkRepoGroupStats(const QList<QJsonObject> &group,
                                QJsonObject &canonical)
{
    auto nonNegativeCount = [](const QJsonValue &value, qint64 *out) {
        if (value.isDouble()) {
            const double number = value.toDouble();
            if (!std::isfinite(number) || number < 0)
                return false;
            *out = qint64(number);
            return true;
        }
        bool ok = false;
        const qint64 number = value.toString().trimmed().toLongLong(&ok);
        if (!ok || number < 0)
            return false;
        *out = number;
        return true;
    };
    qint64 commitCount = -1;
    qint64 mergeCount = -1;
    QVector<qint64> activityWeeks(52, 0);
    QVector<qint64> mergeActivityWeeks(52, 0);
    bool hasActivityWeeks = false;
    bool hasMergeActivityWeeks = false;
    auto mergeSeries = [&](const QJsonObject &member,
                           const QString &field,
                           QVector<qint64> *combined,
                           bool *hasSeries) {
        const QJsonArray memberWeeks = member.value(field).toArray();
        if (memberWeeks.isEmpty())
            return;
        *hasSeries = true;
        const int first = qMax(0, memberWeeks.size() - combined->size());
        const int destination =
            qMax(0, combined->size() - memberWeeks.size());
        for (int index = first; index < memberWeeks.size(); ++index) {
            qint64 amount = 0;
            if (!nonNegativeCount(memberWeeks.at(index), &amount))
                continue;
            (*combined)[destination + index - first] =
                qMax(combined->at(destination + index - first), amount);
        }
    };
    for (const QJsonObject &member : group) {
        qint64 memberCommitCount = -1;
        if (nonNegativeCount(member.value(QStringLiteral("commitCount")),
                             &memberCommitCount)) {
            commitCount = qMax(commitCount, memberCommitCount);
        }
        qint64 memberMergeCount = -1;
        if (nonNegativeCount(member.value(QStringLiteral("mergeCount")),
                             &memberMergeCount)) {
            mergeCount = qMax(mergeCount, memberMergeCount);
        }
        mergeSeries(member, QStringLiteral("activityWeeks"),
                    &activityWeeks, &hasActivityWeeks);
        mergeSeries(member, QStringLiteral("mergeActivityWeeks"),
                    &mergeActivityWeeks, &hasMergeActivityWeeks);
    }
    if (commitCount >= 0)
        canonical.insert(QStringLiteral("commitCount"),
                         QString::number(commitCount));
    if (hasActivityWeeks) {
        QJsonArray mergedWeeks;
        for (const qint64 commits : activityWeeks)
            mergedWeeks.append(commits);
        canonical.insert(QStringLiteral("activityWeeks"), mergedWeeks);
    }
    if (mergeCount >= 0)
        canonical.insert(QStringLiteral("mergeCount"),
                         QString::number(mergeCount));
    if (hasMergeActivityWeeks) {
        QJsonArray mergedWeeks;
        for (const qint64 merges : mergeActivityWeeks)
            mergedWeeks.append(merges);
        canonical.insert(QStringLiteral("mergeActivityWeeks"),
                         mergedWeeks);
    }
}

} // namespace

void MainWindow::renderNetworkRepos(const QJsonArray &repos)
{
    if (!m_networkReposTable || !m_networkReposStatus)
        return;
    m_networkReposLastPayload = repos;

    const QList<QJsonObject> rows =
        canonicalNetworkRepoRows(groupNetworkRepoRecords(repos));

    m_networkReposTable->setRowCount(rows.size());
    const int generation = m_networkReposLoadGen;
    m_networkReposCoveredLocal.clear();
    for (int row = 0; row < rows.size(); ++row) {
        QJsonObject repo = rows.at(row);
        const QString owner = repo.value("owner").toString().trimmed();
        const QString name = repo.value("name").toString().trimmed();
        const QString key = owner + "/" + name;
        const QString routeOwner =
            repo.value("_routeOwner").toString(owner).trimmed();
        const QString routeName =
            repo.value("_routeName").toString(name).trimmed();
        const QString cloneUrl = repo.value("cloneUrl").toString().trimmed();
        const bool isPrivate = repo.value("private").toBool(false) ||
                               repo.value("isPrivate").toBool(false);

        int localFork = -1;
        int mirroredIndex = -1;
        for (const QJsonValue &memberOwner :
             repo.value("_memberOwners").toArray()) {
            const QString member = memberOwner.toString();
            if (localFork < 0)
                localFork = findNetworkLocalForkIndex(member, name);
            if (mirroredIndex < 0)
                mirroredIndex = findNetworkRepoIndex(member, name, false);
        }
        if (localFork < 0)
            localFork = findNetworkLocalForkIndex(routeOwner, routeName);
        if (mirroredIndex < 0)
            mirroredIndex =
                findNetworkRepoIndex(routeOwner, routeName, false);
        // Whatever this row already stands for is not a repository the local
        // table has to list a second time.
        if (localFork >= 0)
            m_networkReposCoveredLocal.insert(localFork);
        if (mirroredIndex >= 0)
            m_networkReposCoveredLocal.insert(mirroredIndex);
        // The local checkout is a badge on the identity line rather than a
        // column of its own, so hand it to the cell builder with the record.
        if (localFork >= 0) {
            const RepositoryRecord &fork = m_repositories.at(localFork);
            repo.insert(QStringLiteral("_localForkRepo"),
                        fork.owner + QLatin1Char('/') + fork.name);
            repo.insert(QStringLiteral("_localForkPath"),
                        fork.localPath.isEmpty()
                            ? QStringLiteral("No checkout path")
                            : QDir::toNativeSeparators(fork.localPath));
        }

        fillNetworkRepoDataCells(m_networkReposTable, row, repo);

        if (QTableWidgetItem *repoItem =
                m_networkReposTable->item(row, kRepoColName)) {
            repoItem->setData(Qt::UserRole, key);
            repoItem->setData(Qt::UserRole + 1, cloneUrl);
            repoItem->setData(Qt::UserRole + 2, isPrivate);
            repoItem->setData(Qt::UserRole + 3, routeOwner);
            repoItem->setData(Qt::UserRole + 4, routeName);
            if (Theme::isNext())
                repoItem->setData(kRepoNextCurrentRole,
                                  m_repoDetailIndex >= 0 &&
                                      (m_repoDetailIndex == localFork ||
                                       m_repoDetailIndex == mirroredIndex));
        }

        addNetworkRepoRowActions(row, routeOwner, routeName, cloneUrl, isPrivate,
                                 localFork, mirroredIndex);

        fetchNetworkRepoMirrors(routeOwner, routeName, row, generation);
    }

    m_networkReposStatus->setText(
        rows.isEmpty() ? QStringLiteral("No repositories advertised.")
                       : QStringLiteral("%1 repositories").arg(formatCount(rows.size())));
    m_networkRepoRowCount = rows.size();
    renderLocalOnlyRepos();
    reposNextRebuildCards(m_networkReposTable, false);
    reposNextSetTabCount(m_networkReposTable, rows.size());
    reposNextApplyFilter(m_networkReposTable);
}

// Deduplicate the advertised catalog records and group the copies of one
// repository together (by root commit, else by name).
QList<QList<QJsonObject>> MainWindow::groupNetworkRepoRecords(
    const QJsonArray &repos)
{
    QList<QJsonObject> records;
    QSet<QString> seenRecords;
    m_privateCatalogAccessIds.clear();
    static const QRegularExpression opaqueAccessIdPattern(
        QStringLiteral("^[0-9a-f]{64}$"));
    for (const QJsonValue &value : repos) {
        QJsonObject repo = value.toObject();
        const QString owner = repo.value("owner").toString().trimmed();
        const QString name = repo.value("name").toString().trimmed();
        if (owner.isEmpty() || name.isEmpty())
            continue;
        const QString recordKey =
            owner.toLower() + QLatin1Char('/') + name.toLower() +
            QLatin1Char('|') + repo.value("source").toString() +
            QLatin1Char('|') + repo.value("servingOwner").toString();
        if (seenRecords.contains(recordKey))
            continue;
        seenRecords.insert(recordKey);
        const QString key = owner + "/" + name;
        const bool isPrivate =
            repo.value("private").toBool(false) ||
            repo.value("isPrivate").toBool(false);
        if (isPrivate) {
            const QString accessId =
                repo.value(QStringLiteral("privateAccessId"))
                    .toString()
                    .trimmed()
                    .toLower();
            const QString accessPath =
                repo.value(QStringLiteral("privateAccessPath"))
                    .toString()
                    .trimmed();
            const QString expectedPath =
                QStringLiteral("/api/private-replicas/") + accessId;
            if (opaqueAccessIdPattern.match(accessId).hasMatch() &&
                (accessPath.isEmpty() || accessPath == expectedPath)) {
                m_privateCatalogAccessIds.insert(key, accessId);
            }
        }
        QString cloneUrl = repo.value("cloneUrl").toString().trimmed();
        if (cloneUrl.isEmpty())
            cloneUrl = repo.value("clone_url").toString().trimmed();
        if (cloneUrl.isEmpty())
            cloneUrl = hostedCloneUrl(owner, name);
        repo.insert("cloneUrl", cloneUrl);
        records.append(repo);
    }

    QHash<QString, QString> rootedByName;
    for (const QJsonObject &repo : std::as_const(records)) {
        const QString root =
            repo.value("rootCommit").toString().trimmed().toLower();
        const QString name =
            repo.value("name").toString().trimmed().toLower();
        if (!root.isEmpty() && !name.isEmpty() && !rootedByName.contains(name))
            rootedByName.insert(name, root);
    }
    QList<QList<QJsonObject>> groups;
    QHash<QString, int> groupIndexes;
    for (const QJsonObject &repo : std::as_const(records)) {
        const QString name =
            repo.value("name").toString().trimmed().toLower();
        QString root =
            repo.value("rootCommit").toString().trimmed().toLower();
        if (root.isEmpty())
            root = rootedByName.value(name);
        const QString groupKey = root.isEmpty()
                                     ? QStringLiteral("name:") + name
                                     : QStringLiteral("root:") + root;
        int index = groupIndexes.value(groupKey, -1);
        if (index < 0) {
            index = groups.size();
            groupIndexes.insert(groupKey, index);
            groups.append(QList<QJsonObject>());
        }
        groups[index].append(repo);
    }
    return groups;
}

// One display row per repository group: the best-placed member, renamed to the
// account (or clone origin) that owns it, sorted by owner/name.
QList<QJsonObject> MainWindow::canonicalNetworkRepoRows(
    const QList<QList<QJsonObject>> &groups)
{
    // Public user-directory ownership turns machine namespaces back into the
    // account that owns them. Organization aliases inserted by
    // refreshNetworkReposPage take precedence over this map below.
    QHash<QString, QString> nodeOwner;
    for (const MemberInfo &user : std::as_const(m_chatDirectoryUsers)) {
        const QString owner = user.name.trimmed();
        for (const QString &node :
             user.nodeName.split(QStringLiteral(", "), Qt::SkipEmptyParts)) {
            const QString key = node.trimmed().toLower();
            if (!key.isEmpty())
                nodeOwner.insert(key, owner);
        }
    }

    auto cloneIdentity = [](const QString &raw) {
        QString path = raw.trimmed();
        if (path.startsWith(QStringLiteral("git@")) && path.contains(':'))
            path = path.section(':', 1);
        else {
            const QUrl url(path);
            if (url.isValid() && !url.path().isEmpty())
                path = url.path();
        }
        path = path.section('?', 0, 0).section('#', 0, 0);
        const QStringList parts =
            path.split('/', Qt::SkipEmptyParts);
        if (parts.size() < 2)
            return QPair<QString, QString>();
        QString name = parts.last();
        if (name.endsWith(QStringLiteral(".git"), Qt::CaseInsensitive))
            name.chop(4);
        return qMakePair(parts.at(parts.size() - 2), name);
    };

    QList<QJsonObject> rows;
    QSet<QString> shownKeys;
    for (const QList<QJsonObject> &group : std::as_const(groups)) {
        if (group.isEmpty())
            continue;
        int best = 0;
        int bestScore = std::numeric_limits<int>::min();
        for (int i = 0; i < group.size(); ++i) {
            const QJsonObject &candidate = group.at(i);
            const QString source =
                candidate.value("source").toString().trimmed();
            const QString owner =
                candidate.value("owner").toString().trimmed().toLower();
            int score = 0;
            if (source == QLatin1String("organization-alias"))
                score += 1000;
            if (source == QLatin1String("local-node"))
                score += 100;
            if (!nodeOwner.contains(owner))
                score += 20;
            if (candidate.value("liveHost").toBool(false))
                score += 5;
            if (score > bestScore) {
                best = i;
                bestScore = score;
            }
        }

        QJsonObject canonical = group.at(best);
        mergeNetworkRepoGroupStats(group, canonical);
        const QString source =
            canonical.value("source").toString().trimmed();
        const QString rawOwner =
            canonical.value("owner").toString().trimmed();
        QString displayOwner = rawOwner;
        QString routeOwner = rawOwner;
        QString routeName =
            canonical.value("name").toString().trimmed();
        if (source == QLatin1String("organization-alias")) {
            routeOwner = displayOwner;
        } else if (source == QLatin1String("remote-clone")) {
            const auto identity =
                cloneIdentity(canonical.value("cloneUrl").toString());
            if (!identity.first.isEmpty() && !identity.second.isEmpty()) {
                displayOwner = identity.first;
                routeOwner = identity.first;
                routeName = identity.second;
            }
        } else {
            displayOwner =
                nodeOwner.value(rawOwner.toLower(), rawOwner);
        }
        const QString displayName =
            canonical.value("name").toString().trimmed();
        const QString displayKey = displayOwner + "/" + displayName;
        if (displayOwner.isEmpty() || displayName.isEmpty() ||
            shownKeys.contains(displayKey.toLower()))
            continue;
        shownKeys.insert(displayKey.toLower());

        QJsonArray memberOwners;
        QSet<QString> memberOwnerSet;
        for (const QJsonObject &member : group) {
            QString owner = member.value("servingOwner").toString().trimmed();
            if (owner.isEmpty())
                owner = member.value("owner").toString().trimmed();
            const QString key = owner.toLower();
            if (!owner.isEmpty() && !memberOwnerSet.contains(key)) {
                memberOwnerSet.insert(key);
                memberOwners.append(owner);
            }
        }
        canonical.insert("owner", displayOwner);
        canonical.insert("name", displayName);
        canonical.insert("_routeOwner", routeOwner);
        canonical.insert("_routeName", routeName);
        canonical.insert("_memberOwners", memberOwners);
        rows.append(canonical);
    }

    std::sort(rows.begin(), rows.end(), [](const QJsonObject &a,
                                           const QJsonObject &b) {
        const QString ak = a.value("owner").toString() + "/" +
                           a.value("name").toString();
        const QString bk = b.value("owner").toString() + "/" +
                           b.value("name").toString();
        return ak.compare(bk, Qt::CaseInsensitive) < 0;
    });
    return rows;
}

// The Switch / Fork / Mirror / Web / Delete buttons of one catalog row.
void MainWindow::addNetworkRepoRowActions(int row, const QString &routeOwner,
                                          const QString &routeName,
                                          const QString &cloneUrl,
                                          bool isPrivate, int localFork,
                                          int mirroredIndex)
{
    auto *actions = new QWidget;
    actions->setObjectName(QStringLiteral("repoRowActions"));
    // A stacked row is several lines tall; keep the buttons pinned to the
    // first line instead of floating in the middle of all that height.
    QGridLayout *actionGrid = networkRepoActionGrid(actions);

    auto *openButton = new QPushButton(QStringLiteral("Switch"));
    openButton->setObjectName("primaryButton");
    openButton->setCursor(Qt::PointingHandCursor);
    openButton->setToolTip(
        localFork >= 0 ? QStringLiteral("Switch to your local copy")
                       : QStringLiteral("Switch to this repository"));
    setOcticon(openButton, localFork >= 0 ? "repo-forked" : "repo", 13);
    connect(openButton, &QPushButton::clicked, this,
            [this, routeOwner, routeName, cloneUrl, isPrivate, localFork] {
                if (localFork >= 0) {
                    if (localFork < m_repositories.size() &&
                        m_repositories.at(localFork).owner != routeOwner)
                        configureForkSource(localFork, cloneUrl, false);
                    openRepoDetail(localFork);
                } else
                    openNetworkRepo(routeOwner, routeName, cloneUrl,
                                    isPrivate);
            });
    addNetworkRepoAction(actionGrid, openButton, 0);

    auto *forkButton = new QPushButton(localFork >= 0
                                           ? QStringLiteral("Forked")
                                           : QStringLiteral("Fork"));
    forkButton->setCursor(Qt::PointingHandCursor);
    forkButton->setToolTip(localFork >= 0 ? QStringLiteral("You already have a local fork")
                                          : QStringLiteral("Fork into your own node"));
    setOcticon(forkButton, "repo-forked", 13);
    if (localFork >= 0) {
        forkButton->setEnabled(false);
    } else {
        forkButton->setObjectName("ghostButton");
        connect(forkButton, &QPushButton::clicked, this,
                [this, routeOwner, routeName, cloneUrl, isPrivate] {
                    forkNetworkRepo(routeOwner, routeName, cloneUrl,
                                    isPrivate);
                });
    }
    addNetworkRepoAction(actionGrid, forkButton, 1);

    auto *mirrorButton = new QPushButton(mirroredIndex >= 0
                                             ? QStringLiteral("Mirrored")
                                             : QStringLiteral("Mirror"));
    mirrorButton->setObjectName("ghostButton");
    mirrorButton->setCursor(Qt::PointingHandCursor);
    mirrorButton->setToolTip(mirroredIndex >= 0
                                 ? QStringLiteral("This machine already mirrors it")
                                 : QStringLiteral("Mirror this repository on this machine"));
    setOcticon(mirrorButton, "sync", 13);
    if (mirroredIndex >= 0) {
        mirrorButton->setEnabled(false);
    } else {
        connect(mirrorButton, &QPushButton::clicked, this,
                [this, routeOwner, routeName, cloneUrl, isPrivate] {
                    mirrorNetworkRepo(routeOwner, routeName, cloneUrl,
                                      isPrivate);
                });
    }
    addNetworkRepoAction(actionGrid, mirrorButton, 2);

    auto *webButton = new QPushButton(QStringLiteral("Web"));
    webButton->setObjectName("ghostButton");
    webButton->setCursor(Qt::PointingHandCursor);
    webButton->setToolTip(
        QStringLiteral("View %1/%2 on the website")
            .arg(routeOwner, routeName));
    setOcticon(webButton, "link", 13);
    connect(webButton, &QPushButton::clicked, this,
            [this, routeOwner, routeName] {
                const QUrl url(repositoryWebUrl(routeOwner, routeName));
                if (url.isValid() && !url.host().isEmpty())
                    QDesktopServices::openUrl(url);
            });
    addNetworkRepoAction(actionGrid, webButton, 3);

    const int deleteIndex = mirroredIndex >= 0 ? mirroredIndex : localFork;
    auto *deleteButton = new QPushButton(QStringLiteral("Delete"));
    deleteButton->setObjectName("dangerButton");
    deleteButton->setCursor(Qt::PointingHandCursor);
    setOcticon(deleteButton, "trash", 13);
    if (deleteIndex < 0) {
        // Nothing local to delete. An administrator can still erase the
        // repository itself: the catalog row and every record the relay keeps
        // about it (adhoc #196). Anyone else gets the old dead button.
        if (canAdminPurgeRepos()) {
            deleteButton->setToolTip(
                QStringLiteral(
                    "Admin: erase %1/%2 from the network. This machine has no "
                    "copy, so nothing local is touched.")
                    .arg(routeOwner, routeName));
            connect(deleteButton, &QPushButton::clicked, this,
                    [this, routeOwner, routeName] {
                        adminPurgeNetworkRepo(routeOwner, routeName);
                    });
        } else {
            deleteButton->setEnabled(false);
            deleteButton->setToolTip(
                QStringLiteral("This repository is not on this machine"));
        }
    } else {
        const QString targetOwner = m_repositories.at(deleteIndex).owner;
        const QString targetName = m_repositories.at(deleteIndex).name;
        deleteButton->setToolTip(
            QStringLiteral("Delete %1/%2 from this machine")
                .arg(targetOwner, targetName));
        connect(deleteButton, &QPushButton::clicked, this,
                [this, targetOwner, targetName] {
                    const int index =
                        findNetworkRepoIndex(targetOwner, targetName, true);
                    if (index >= 0)
                        deleteRepositoryAt(index, false);
                    QTimer::singleShot(0, this,
                                       [this] { refreshNetworkReposPage(); });
                });
    }
    addNetworkRepoAction(actionGrid, deleteButton, 4);
    reposNextStyleRowActions(actions);
    m_networkReposTable->setCellWidget(row, kRepoColActions, actions);
    reposNextTrackCell(m_networkReposTable, actions);
}

// The catalog above can only ever show what the relay advertises. Everything
// this machine holds that never reaches the relay -- a private repository, or
// one still local-only -- lands here instead of nowhere.
void MainWindow::renderLocalOnlyRepos()
{
    if (!m_localReposTable || !m_localReposStatus)
        return;

    const QString localMachine = accountOwner().trimmed();
    // This machine's own roster entry describes the host every row here is
    // served from, so the Host column says the same things it does above.
    QString localPlatform;
    QString localVersion;
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        if (!member.self)
            continue;
        localPlatform = member.platform;
        localVersion = member.version;
        break;
    }

    QList<int> indexes;
    for (int i = 0; i < m_repositories.size(); ++i) {
        const RepositoryRecord &repo = m_repositories.at(i);
        if (repo.previewOnly || !repositoryBelongsToActiveRelay(repo) ||
            m_networkReposCoveredLocal.contains(i))
            continue;
        if (repo.owner.trimmed().isEmpty() || repo.name.trimmed().isEmpty())
            continue;
        // A record with neither a checkout nor a mirror is a pointer at someone
        // else's repository, not a copy this machine can serve or open.
        if (repo.localPath.trimmed().isEmpty() &&
            repo.mirrorPath.trimmed().isEmpty())
            continue;
        indexes.append(i);
    }
    std::sort(indexes.begin(), indexes.end(), [this](int a, int b) {
        const RepositoryRecord &ra = m_repositories.at(a);
        const RepositoryRecord &rb = m_repositories.at(b);
        return (ra.owner + QLatin1Char('/') + ra.name)
                   .compare(rb.owner + QLatin1Char('/') + rb.name,
                            Qt::CaseInsensitive) < 0;
    });

    // Down to zero first: clearContents() would leave the previous render's
    // action widgets sitting in rows this one may not fill.
    m_localReposTable->setRowCount(0);
    m_localReposTable->setRowCount(indexes.size());
    QList<QPair<QString, QString>> dirtyInputs; // repository key, checkout
    for (int row = 0; row < indexes.size(); ++row) {
        const int index = indexes.at(row);
        const RepositoryRecord &repo = m_repositories.at(index);
        const QString key = repo.owner + QLatin1Char('/') + repo.name;
        // Every action re-resolves the record by name when it runs: a delete
        // elsewhere on the page renumbers m_repositories under this row.
        const QString targetOwner = repo.owner;
        const QString targetName = repo.name;
        const QString dirtyPath = repo.localPath.trimmed().isEmpty()
                                      ? repo.mirrorPath.trimmed()
                                      : repo.localPath.trimmed();
        if (!dirtyPath.isEmpty())
            dirtyInputs.append(qMakePair(key, dirtyPath));

        // Same shape the catalog rows are built from, so both tables can be
        // filled by the same builder and read as one page.
        QJsonObject record;
        record.insert(QStringLiteral("owner"), repo.owner);
        record.insert(QStringLiteral("name"), repo.name);
        record.insert(QStringLiteral("private"), repo.isPrivate);
        record.insert(QStringLiteral("cloneUrl"), repo.cloneUrl);
        record.insert(QStringLiteral("actionsEnabled"), repo.actionsEnabled);
        record.insert(QStringLiteral("machineName"), localMachine);
        record.insert(QStringLiteral("platform"), localPlatform);
        record.insert(QStringLiteral("version"), localVersion);
        record.insert(QStringLiteral("source"),
                      repo.publishToNetwork
                          ? QStringLiteral("this machine, publish pending")
                          : QStringLiteral("this machine, local only"));
        if (repo.lastSyncMs > 0)
            record.insert(QStringLiteral("lastSync"),
                          QString::number(repo.lastSyncMs));
        if (repo.hostedSinceMs > 0)
            record.insert(QStringLiteral("hostedSince"),
                          QString::number(repo.hostedSinceMs));
        // Every row here is on this disk, so the badge says which copy it is
        // rather than repeating "Local fork" down the whole column.
        record.insert(QStringLiteral("_localForkPath"),
                      QDir::toNativeSeparators(repo.localPath.isEmpty()
                                                   ? repo.mirrorPath
                                                   : repo.localPath));
        record.insert(QStringLiteral("_localForkLabel"),
                      repo.localPath.isEmpty() ? QStringLiteral("Mirror")
                                               : QStringLiteral("Checkout"));
        record.insert(QStringLiteral("_localForkRepo"), key);
        record.insert(QStringLiteral("_dirtyPending"), true);

        // Whatever the last mirror-advert pass measured, rather than a fresh
        // round of synchronous git reads on the GUI thread.
        const MirrorAdvert advert = m_localRepoAdverts.value(key);
        if (!advert.commit.isEmpty()) {
            record.insert(QStringLiteral("branch"), advert.branch);
            record.insert(QStringLiteral("commit"), advert.commit);
            record.insert(QStringLiteral("commitSubject"),
                          advert.commitIdentity.subject);
            record.insert(QStringLiteral("commitAuthorName"),
                          advert.commitIdentity.author);
            if (advert.commitIdentity.committedAtMs > 0)
                record.insert(
                    QStringLiteral("commitAt"),
                    QString::number(advert.commitIdentity.committedAtMs));
            if (advert.sizeBytes > 0)
                record.insert(QStringLiteral("sizeBytes"),
                              QString::number(advert.sizeBytes));
            const auto count = [&record](const QString &field, int value) {
                if (value >= 0)
                    record.insert(field, QString::number(value));
            };
            count(QStringLiteral("issueCount"), advert.issueCount);
            count(QStringLiteral("pullCount"), advert.pullCount);
            count(QStringLiteral("commitCount"), advert.commitCount);
            count(QStringLiteral("mergeCount"), advert.mergeCount);
            count(QStringLiteral("branchCount"), advert.branchCount);
            count(QStringLiteral("discussionCount"), advert.discussionCount);
            count(QStringLiteral("worktreeCount"), advert.worktreeCount);
            count(QStringLiteral("artifactCount"), advert.artifactCount);
            if (!advert.activityWeeks.isEmpty())
                record.insert(QStringLiteral("activityWeeks"),
                              advert.activityWeeks);
            if (!advert.mergeActivityWeeks.isEmpty())
                record.insert(QStringLiteral("mergeActivityWeeks"),
                              advert.mergeActivityWeeks);
        }

        fillNetworkRepoDataCells(m_localReposTable, row, record);

        if (QTableWidgetItem *repoItem =
                m_localReposTable->item(row, kRepoColName)) {
            repoItem->setData(Qt::UserRole, key);
            repoItem->setData(Qt::UserRole + 1, repo.cloneUrl);
            repoItem->setData(Qt::UserRole + 2, repo.isPrivate);
            repoItem->setData(Qt::UserRole + 3, repo.owner);
            repoItem->setData(Qt::UserRole + 4, repo.name);
            if (Theme::isNext())
                repoItem->setData(kRepoNextCurrentRole, index == m_repoDetailIndex);
        }

        // No relay row means no mirrors to count: this copy is the only one.
        if (QTableWidgetItem *mirrorsItem =
                m_localReposTable->item(row, kRepoColMirrors)) {
            mirrorsItem->setText(QStringLiteral("0"));
            mirrorsItem->setToolTip(
                QStringLiteral("Served only from this machine"));
        }

        auto *actions = new QWidget;
        actions->setObjectName(QStringLiteral("repoRowActions"));
        QGridLayout *actionGrid = networkRepoActionGrid(actions);

        auto *openButton = new QPushButton(QStringLiteral("Switch"));
        openButton->setObjectName("primaryButton");
        openButton->setCursor(Qt::PointingHandCursor);
        openButton->setToolTip(QStringLiteral("Switch to %1 on this machine")
                                   .arg(key));
        setOcticon(openButton, "repo", 13);
        connect(openButton, &QPushButton::clicked, this,
                [this, targetOwner, targetName] {
                    const int current =
                        findNetworkRepoIndex(targetOwner, targetName, true);
                    if (current >= 0)
                        openRepoDetail(current);
                });
        addNetworkRepoAction(actionGrid, openButton, 0);

        auto *settingsButton = new QPushButton(QStringLiteral("Settings"));
        settingsButton->setObjectName("ghostButton");
        settingsButton->setCursor(Qt::PointingHandCursor);
        settingsButton->setToolTip(
            repo.isPrivate
                ? QStringLiteral("Private repositories stay off the relay; "
                                 "share or publish it from its settings")
                : QStringLiteral("Publish it to the relay from its settings"));
        setOcticon(settingsButton, "gear", 13);
        connect(settingsButton, &QPushButton::clicked, this,
                [this, targetOwner, targetName] {
                    const int current =
                        findNetworkRepoIndex(targetOwner, targetName, true);
                    if (current < 0)
                        return;
                    openRepoDetail(current);
                    // click(), not setChecked(): the tab pages are built lazily
                    // off the button group's signal.
                    if (m_repoDetailTabs && m_settingsTabIndex >= 0 &&
                        m_repoDetailTabs->button(m_settingsTabIndex))
                        m_repoDetailTabs->button(m_settingsTabIndex)->click();
                });
        addNetworkRepoAction(actionGrid, settingsButton, 1);

        auto *deleteButton = new QPushButton(QStringLiteral("Delete"));
        deleteButton->setObjectName("dangerButton");
        deleteButton->setCursor(Qt::PointingHandCursor);
        setOcticon(deleteButton, "trash", 13);
        deleteButton->setToolTip(QStringLiteral("Delete %1 from this machine")
                                     .arg(key));
        connect(deleteButton, &QPushButton::clicked, this,
                [this, targetOwner, targetName] {
                    const int current =
                        findNetworkRepoIndex(targetOwner, targetName, true);
                    if (current >= 0)
                        deleteRepositoryAt(current, false);
                    QTimer::singleShot(0, this,
                                       [this] { refreshNetworkReposPage(); });
                });
        addNetworkRepoAction(actionGrid, deleteButton, 4);
        reposNextStyleRowActions(actions);
        m_localReposTable->setCellWidget(row, kRepoColActions, actions);
        reposNextTrackCell(m_localReposTable, actions);
    }

    // Dirty checks can be expensive on generated trees. Paint the list first,
    // then replace each row's "Checking" state as its background scan returns.
    QPointer<QTableWidget> localTable(m_localReposTable);
    runOffThread<QStringList>(
        [dirtyInputs] {
            QStringList results;
            for (const auto &input : dirtyInputs) {
                QByteArray output;
                runGitCapture(input.second,
                              {QStringLiteral("status"),
                               QStringLiteral("--porcelain=v1"),
                               QStringLiteral("--untracked-files=all")},
                              &output, nullptr);
                const int count = QString::fromUtf8(output)
                                      .split(QLatin1Char('\n'),
                                             Qt::SkipEmptyParts)
                                      .size();
                results << input.first + QLatin1Char('\t') +
                               QString::number(count);
            }
            return results;
        },
        [localTable](const QStringList &results) {
            if (!localTable)
                return;
            QHash<QString, int> counts;
            for (const QString &result : results) {
                const int tab = result.lastIndexOf(QLatin1Char('\t'));
                if (tab > 0)
                    counts.insert(result.left(tab), result.mid(tab + 1).toInt());
            }
            for (int row = 0; row < localTable->rowCount(); ++row) {
                QTableWidgetItem *item = localTable->item(row, kRepoColName);
                if (!item)
                    continue;
                const QString key = item->data(Qt::UserRole).toString();
                if (!counts.contains(key))
                    continue;
                QStringList parts = item->data(kRepoCompactTextRole)
                                        .toString()
                                        .split(QStringLiteral(" | "));
                const int count = counts.value(key);
                const QString state = count > 0
                    ? QStringLiteral("Dirty: %1 %2")
                          .arg(count)
                          .arg(count == 1 ? QStringLiteral("file")
                                          : QStringLiteral("files"))
                    : QStringLiteral("Clean");
                if (parts.size() > 1)
                    parts[1] = state;
                else
                    parts << state;
                const QString compactText = parts.join(QStringLiteral(" | "));
                item->setData(kRepoCompactTextRole, compactText);
                if (localTable->property("compactRepoList").toBool())
                    item->setText(compactText);
            }
            reposNextSyncLocalDirty(localTable);
        });

    m_localReposStatus->setText(
        indexes.isEmpty()
            ? QString::fromUtf8("None \xE2\x80\x94 everything on this machine "
                                "is listed above")
            : QStringLiteral("%1 %2 not on the relay")
                  .arg(formatCount(indexes.size()),
                       indexes.size() == 1 ? QStringLiteral("repository")
                                           : QStringLiteral("repositories")));
    m_localReposTable->setVisible(true);
    reposNextRebuildCards(m_localReposTable, true);
    reposNextSetTabCount(m_localReposTable, indexes.size());
    reposNextApplyFilter(m_localReposTable);
}

// Shared by the catalog table and the private/local table below it, so a
// repository reads the same either way.
void MainWindow::fillNetworkRepoDataCells(QTableWidget *table, int row,
                                          const QJsonObject &repo)
{
    if (!table)
        return;
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const QString separator = QString::fromUtf8(" \xC2\xB7 ");

    auto number = [&repo](const QString &key) {
        const QJsonValue value = repo.value(key);
        if (value.isDouble())
            return QVariant(qint64(value.toDouble()));
        bool ok = false;
        const qint64 parsed = value.toString().trimmed().toLongLong(&ok);
        return ok ? QVariant(parsed) : QVariant();
    };
    auto text = [&repo](const QString &key) {
        return repo.value(key).toString().trimmed();
    };
    auto joinArray = [&repo](const QString &key) {
        QStringList values;
        for (const QJsonValue &value : repo.value(key).toArray()) {
            const QString entry = value.toString().trimmed();
            if (!entry.isEmpty())
                values << entry;
        }
        return values;
    };
    auto compact = [](const QStringList &parts) {
        QStringList kept;
        for (const QString &part : parts)
            if (!part.trimmed().isEmpty())
                kept << part.trimmed();
        return kept;
    };

    // A line is the same facts in the same order for every repository; whatever
    // this one has not published drops out of it, and a line left with nothing
    // to say prints an em dash so the lines under it keep their slot.
    auto line = [&](const QStringList &parts) {
        const QStringList kept = compact(parts);
        return kept.isEmpty() ? dash : kept.join(separator);
    };
    auto section = [&](const QString &label, const QStringList &parts) {
        return label + QStringLiteral(": ") + line(parts);
    };
    auto stackedCell = [&](int column, const QStringList &lines,
                           const QStringList &tips, bool titleLead) {
        auto *item = new QTableWidgetItem(lines.join(QLatin1Char('\n')));
        item->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
        if (titleLead)
            item->setData(kRepoLeadBoldRole, true);
        const QStringList keptTips = compact(tips);
        if (!keptTips.isEmpty())
            item->setToolTip(keptTips.join(QLatin1Char('\n')));
        table->setItem(row, column, item);
    };

    auto counted = [&](const QString &key, const QString &singular,
                       const QString &plural) {
        const QVariant value = number(key);
        if (!value.isValid())
            return QString();
        const qint64 amount = value.toLongLong();
        if (amount <= 0)
            return QString();
        return QStringLiteral("%1 %2").arg(formatCount(amount),
                                           amount == 1 ? singular : plural);
    };
    auto relative = [&](const QString &key) {
        const QVariant value = number(key);
        const qint64 ms = value.toLongLong();
        return !value.isValid() || ms <= 0 ? QString()
                                           : formatIssueRelativeTime(ms);
    };
    auto since = [&](const QString &word, const QString &key) {
        const QString ago = relative(key);
        return ago.isEmpty() ? QString()
                             : QStringLiteral("%1 %2").arg(word, ago);
    };
    auto detail = [](const QString &label, const QString &value) {
        return value.trimmed().isEmpty()
                   ? QString()
                   : QStringLiteral("%1: %2").arg(label, value.trimmed());
    };
    auto stamp = [&](const QString &label, const QString &key) {
        const QVariant value = number(key);
        const qint64 ms = value.toLongLong();
        return !value.isValid() || ms <= 0
                   ? QString()
                   : QStringLiteral("%1: %2").arg(label, formatRepoDate(ms));
    };
    auto usage = [&](const QString &label, const QString &usedKey,
                     const QString &totalKey) {
        const QVariant used = number(usedKey);
        const QVariant total = number(totalKey);
        if (!used.isValid() || !total.isValid() || total.toLongLong() <= 0)
            return QString();
        return QStringLiteral("%1 %2/%3")
            .arg(label, SystemStats::formatBytes(used.toLongLong()),
                 SystemStats::formatBytes(total.toLongLong()));
    };

    // Every fact owns a stable labeled line. In particular, the commit message
    // no longer shares space with the branch and hash, so all rows align even
    // when one repository has a long subject and another has no subject yet.
    const bool isPrivate = repo.value("private").toBool(false) ||
                           repo.value("isPrivate").toBool(false);
    const bool sharedWithMe = repo.value("sharedWithMe").toBool(false);
    const QString visibility =
        isPrivate ? (sharedWithMe ? QStringLiteral("Private (shared)")
                                  : QStringLiteral("Private"))
                  : QStringLiteral("Public");
    const QString termsCategory = text(QStringLiteral("termsCategory"));
    const QString terms =
        repo.value("termsFlagged").toBool(false)
            ? QStringLiteral("Flagged: %1")
                  .arg(termsCategory.isEmpty() ? QStringLiteral("terms")
                                               : termsCategory)
            : QString();
    const QString localForkPath = text(QStringLiteral("_localForkPath"));
    // "Local fork" is what a copy on this machine is called next to a network
    // row; the private/local table names what its copy actually is instead.
    const QString localForkLabel =
        localForkPath.isEmpty()
            ? QString()
            : (text(QStringLiteral("_localForkLabel")).isEmpty()
                   ? QStringLiteral("Local fork")
                   : text(QStringLiteral("_localForkLabel")));
    const QString commit = text(QStringLiteral("commit"));
    const QString subject = text(QStringLiteral("commitSubject"));
    const QStringList changedFiles = joinArray(QStringLiteral("changedFiles"));
    const QString cloneUrl = text(QStringLiteral("cloneUrl"));
    const QString sshUrl = text(QStringLiteral("sshUrl"));
    stackedCell(
        kRepoColName,
        {text(QStringLiteral("owner")) + QLatin1Char('/') +
             text(QStringLiteral("name")),
         section(QStringLiteral("Access"),
                 {visibility,
                  repo.value("liveHost").toBool(false)
                      ? QStringLiteral("Live host")
                      : QString(),
                  localForkLabel, terms}),
         section(QStringLiteral("Revision"),
                 {text(QStringLiteral("branch")), commit.left(12)}),
         section(QStringLiteral("Message"), {subject}),
         section(QStringLiteral("Author"),
                 {text(QStringLiteral("commitAuthorName")),
                  since(QStringLiteral("committed"),
                        QStringLiteral("commitAt"))}),
         section(QStringLiteral("Working tree"),
                 {since(QStringLiteral("updated"),
                        QStringLiteral("updatedAt")),
                  changedFiles.isEmpty()
                      ? QString()
                      : QStringLiteral("%1 uncommitted")
                            .arg(formatCount(changedFiles.size()))})},
        {detail(QStringLiteral("Commit"), commit), subject,
         stamp(QStringLiteral("Last commit"), QStringLiteral("commitAt")),
         stamp(QStringLiteral("Updated"), QStringLiteral("updatedAt")),
         detail(QStringLiteral("Clone"), cloneUrl),
         detail(QStringLiteral("SSH"), sshUrl),
         localForkPath.isEmpty()
             ? QString()
             : QStringLiteral("%1: %2\n%3")
                   .arg(localForkLabel, text(QStringLiteral("_localForkRepo")),
                        localForkPath),
        changedFiles.isEmpty()
             ? QString()
             : QStringLiteral("Uncommitted on the host:\n%1")
                   .arg(changedFiles.join(QLatin1Char('\n')))},
        true);
    if (QTableWidgetItem *nameItem = table->item(row, kRepoColName)) {
        const QString detailed = nameItem->text();
        QStringList compactParts;
        compactParts << (text(QStringLiteral("owner")) + QLatin1Char('/') +
                         text(QStringLiteral("name")));
        if (!changedFiles.isEmpty())
            compactParts << QStringLiteral("Dirty: %1 files")
                                .arg(formatCount(changedFiles.size()));
        else if (repo.value(QStringLiteral("_dirtyPending")).toBool(false))
            compactParts << QStringLiteral("Checking changes...");
        else if (!localForkPath.isEmpty())
            compactParts << QStringLiteral("Clean");
        const QString revision =
            compact({text(QStringLiteral("branch")), commit.left(12)})
                .join(QLatin1Char(' '));
        if (!revision.isEmpty())
            compactParts << revision;
        if (!subject.isEmpty())
            compactParts << subject;
        const QString author = text(QStringLiteral("commitAuthorName"));
        const QString committed = since(QStringLiteral("committed"),
                                        QStringLiteral("commitAt"));
        const QString authorStamp = compact({author, committed}).join(
            QStringLiteral(", "));
        if (!authorStamp.isEmpty())
            compactParts << authorStamp;
        const QString compactText = compactParts.join(QStringLiteral(" | "));
        nameItem->setData(kRepoDetailedTextRole, detailed);
        nameItem->setData(kRepoCompactTextRole, compactText);
        if (Theme::isNext())
            nameItem->setData(kRepoNextRecordRole, repo);
        if (table->property("compactRepoList").toBool())
            nameItem->setText(compactText);
    }

    // Contents are six individually aligned sections rather than four lines
    // whose meanings changed depending on which counters happened to exist.
    const QVariant sizeBytes = number(QStringLiteral("sizeBytes"));
    stackedCell(
        kRepoColContents,
        {section(QStringLiteral("Collaboration"),
                 {counted(QStringLiteral("issueCount"),
                          QStringLiteral("issue"), QStringLiteral("issues")),
                  counted(QStringLiteral("pullCount"), QStringLiteral("pull"),
                          QStringLiteral("pulls"))}),
         section(QStringLiteral("History"),
                 {counted(QStringLiteral("commitCount"),
                          QStringLiteral("commit"), QStringLiteral("commits")),
                  counted(QStringLiteral("mergeCount"),
                          QStringLiteral("merge"), QStringLiteral("merges")),
                  counted(QStringLiteral("branchCount"),
                          QStringLiteral("branch"),
                          QStringLiteral("branches"))}),
         section(QStringLiteral("Community"),
                 {counted(QStringLiteral("discussionCount"),
                          QStringLiteral("discussion"),
                          QStringLiteral("discussions"))}),
         section(QStringLiteral("Copies"),
                 {counted(QStringLiteral("worktreeCount"),
                          QStringLiteral("worktree"),
                          QStringLiteral("worktrees")),
                  counted(QStringLiteral("artifactCount"),
                          QStringLiteral("artifact"),
                          QStringLiteral("artifacts"))}),
         section(QStringLiteral("Storage"),
                 {sizeBytes.isValid() && sizeBytes.toLongLong() > 0
                      ? SystemStats::formatBytes(sizeBytes.toLongLong())
                      : QString()}),
         section(QStringLiteral("Traffic"),
                 {counted(QStringLiteral("clonesServed"),
                          QStringLiteral("clone"), QStringLiteral("clones")),
                  counted(QStringLiteral("websiteServed"),
                          QStringLiteral("web hit"),
                          QStringLiteral("web hits"))})},
        {number(QStringLiteral("issueMaxNumber")).isValid()
             ? QStringLiteral("Highest issue number ever assigned: %1")
                   .arg(number(QStringLiteral("issueMaxNumber")).toLongLong())
             : QString(),
         stamp(QStringLiteral("Last clone served"),
               QStringLiteral("cloneServedAt")),
         detail(QStringLiteral("Clone client"),
                text(QStringLiteral("cloneServedAgent"))),
         stamp(QStringLiteral("Last website hit"),
               QStringLiteral("websiteServedAt")),
         detail(QStringLiteral("Website client"),
                text(QStringLiteral("websiteServedAgent")))},
        false);

    // Host facts use the same labeled slots on every row. RAM and disk no
    // longer compete on one variable-width line.
    QString ci = text(QStringLiteral("actionsState"));
    if (ci.isEmpty() && repo.contains(QStringLiteral("actionsEnabled")))
        ci = repo.value("actionsEnabled").toBool() ? QStringLiteral("enabled")
                                                   : QStringLiteral("disabled");
    const QVariant cpuPercent = number(QStringLiteral("cpuPercent"));
    const QStringList hosts = joinArray(QStringLiteral("_memberOwners"));
    QString encryption = text(QStringLiteral("mirrorEncryption"));
    const QVariant keyEpoch = number(QStringLiteral("keyEpoch"));
    if (!encryption.isEmpty() && keyEpoch.isValid())
        encryption += separator + QStringLiteral("epoch %1").arg(keyEpoch.toLongLong());
    stackedCell(
        kRepoColHost,
        {section(QStringLiteral("Machine"),
                 {text(QStringLiteral("machineName"))}),
         section(QStringLiteral("System"),
                 {text(QStringLiteral("platform")),
                  text(QStringLiteral("runtimeMode"))}),
         section(QStringLiteral("Software"),
                 {text(QStringLiteral("version")),
                  ci.isEmpty() ? QString()
                               : QStringLiteral("Actions %1").arg(ci),
                  joinArray(QStringLiteral("agentProviders"))
                      .join(QStringLiteral(", "))}),
         section(QStringLiteral("Resources"),
                 {cpuPercent.isValid()
                      ? QStringLiteral("CPU %1%").arg(cpuPercent.toLongLong())
                      : QString(),
                  usage(QStringLiteral("RAM"), QStringLiteral("memUsedBytes"),
                        QStringLiteral("memTotalBytes"))}),
         section(QStringLiteral("Storage"),
                 {usage(QStringLiteral("Disk"),
                        QStringLiteral("diskUsedBytes"),
                        QStringLiteral("diskTotalBytes"))}),
         section(QStringLiteral("Sync"),
                 {text(QStringLiteral("source")),
                  since(QStringLiteral("synced"),
                        QStringLiteral("lastSync")),
                  hosts.join(QStringLiteral(", "))})},
        {hosts.isEmpty() ? QString()
                         : QStringLiteral("Hosts:\n%1")
                               .arg(hosts.join(QLatin1Char('\n'))),
         stamp(QStringLiteral("Hosted since"), QStringLiteral("hostedSince")),
         stamp(QStringLiteral("Last sync"), QStringLiteral("lastSync")),
         detail(QStringLiteral("Channel"), text(QStringLiteral("channel"))),
         detail(QStringLiteral("Encryption"), encryption),
         detail(QStringLiteral("Repo id"), text(QStringLiteral("opaqueRepoId"))),
         detail(QStringLiteral("Manifest"),
                text(QStringLiteral("encryptedManifestHash"))),
         detail(QStringLiteral("Solana"), text(QStringLiteral("solana"))),
         detail(QStringLiteral("Maintainer"),
                text(QStringLiteral("maintainer"))),
         detail(QStringLiteral("Node id"), text(QStringLiteral("nodeId"))),
         detail(QStringLiteral("Root commit"),
                text(QStringLiteral("rootCommit"))),
         detail(text(QStringLiteral("stateSig")).isEmpty()
                    ? QStringLiteral("State hash (unsigned)")
                    : QStringLiteral("State hash (owner-signed)"),
                text(QStringLiteral("stateHash")))},
        false);

    // The mirror count arrives from its own request; leave the ellipsis until
    // fetchNetworkRepoMirrors answers.
    auto *mirrorsItem =
        new QTableWidgetItem(QString::fromUtf8("\xE2\x80\xA6"));
    mirrorsItem->setData(kRepoMirrorCellRole, true);
    mirrorsItem->setForeground(Theme::remapColor(QColor("#8b949e")));
    mirrorsItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    table->setItem(row, kRepoColMirrors, mirrorsItem);

    auto installActivityChart = [&](int column, const QString &seriesField,
                                    const QString &totalField,
                                    const QString &singular,
                                    const QString &plural,
                                    const QColor &color,
                                    const QString &objectName) {
        const QJsonArray activity = repo.value(seriesField).toArray();
        if (activity.isEmpty()) {
            auto *item = new QTableWidgetItem(
                QStringLiteral("Activity not published"));
            item->setForeground(Theme::remapColor(QColor("#8b949e")));
            item->setTextAlignment(Qt::AlignLeft | Qt::AlignTop);
            item->setToolTip(
                QStringLiteral("This publisher has not reported %1 yet.")
                    .arg(seriesField));
            table->setItem(row, column, item);
            return;
        }
        QVector<qint64> weeks;
        weeks.reserve(activity.size());
        for (const QJsonValue &week : activity) {
            const QVariant value = week.isDouble()
                ? QVariant(qint64(week.toDouble()))
                : QVariant(week.toString().trimmed().toLongLong());
            weeks.append(qMax<qint64>(0, value.toLongLong()));
        }
        const QVariant total = number(totalField);
        table->setCellWidget(
            row, column,
            new NetworkRepoActivityChart(
                weeks, total.isValid() ? total.toLongLong() : -1,
                singular, plural, color, objectName, table));
    };

    installActivityChart(
        kRepoColActivity, QStringLiteral("activityWeeks"),
        QStringLiteral("commitCount"), QStringLiteral("commit"),
        QStringLiteral("commits"),
        QColor(currentThemeIsDark() ? "#3fb950" : "#1f883d"),
        QStringLiteral("networkRepoCommitActivityChart"));
    installActivityChart(
        kRepoColMergeActivity, QStringLiteral("mergeActivityWeeks"),
        QStringLiteral("mergeCount"), QStringLiteral("merge"),
        QStringLiteral("merges"),
        QColor(currentThemeIsDark() ? "#a371f7" : "#8250df"),
        QStringLiteral("networkRepoMergeActivityChart"));
}

// Per-repo memo for the mirror tally in the network repositories table.
//
// This is a request *per row*, issued from the row builder, so it did not need
// a timer to become load: every re-render of the table re-asked the relay about
// every repository on it. In a 21.5h network log that was ~60s per repo across
// five repos — more requests than any timer on the client. The tally itself is
// a count of mirror nodes, which moves when a mirror is added or drops out, not
// second to second.
//
// A cache hit repaints from the stored body, so the column still fills in on
// every render; only the HTTP disappears. Anything that genuinely changes the
// answer (a mirror mutation, opening the repo) already forces a re-render, and
// the TTL bounds how long a fleet change can stay invisible.
constexpr qint64 kNetworkRepoMirrorsCacheMs = 5 * 60 * 1000;

void MainWindow::fetchNetworkRepoMirrors(const QString &owner, const QString &name,
                                         int row, int generation)
{
    if (!m_networkAccess || !m_networkReposTable || owner.isEmpty() ||
        name.isEmpty())
        return;

    const QString cacheKey = owner.toLower() + QLatin1Char('/') + name.toLower();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 fetchedMs = m_networkRepoMirrorsFetchedMs.value(cacheKey, 0);
    if (fetchedMs > 0 && nowMs - fetchedMs < kNetworkRepoMirrorsCacheMs) {
        applyNetworkRepoMirrors(m_networkRepoMirrorsCache.value(cacheKey), owner,
                                name, row, generation);
        return;
    }

    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/mirrors")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(name))));
    url.setQuery(QString());

    QNetworkRequest request(url);
    request.setRawHeader("accept", "application/json");
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, owner, name, row, generation, cacheKey] {
                const QByteArray body = reply->readAll();
                const QNetworkReply::NetworkError error = reply->error();
                const QString errorString = reply->errorString();
                reply->deleteLater();
                if (error != QNetworkReply::NoError) {
                    // A failed fetch stamps nothing, so the next render retries
                    // instead of caching the failure for five minutes. The row
                    // must still be the one this reply was issued for: the same
                    // index can belong to a different repository after a
                    // re-render, and painting the dash there would blank a row
                    // that is perfectly healthy.
                    if (networkRepoMirrorCell(owner, name, row, generation)) {
                        QTableWidgetItem *item =
                            m_networkReposTable->item(row, kRepoColMirrors);
                        item->setText(QString::fromUtf8("\xE2\x80\x94"));
                        item->setToolTip(errorString);
                        item->setForeground(Theme::remapColor(QColor("#8b949e")));
                        reposNextSyncCardMirrors(m_networkReposTable, row);
                    }
                    return;
                }
                m_networkRepoMirrorsCache.insert(cacheKey, body);
                m_networkRepoMirrorsFetchedMs.insert(
                    cacheKey, QDateTime::currentMSecsSinceEpoch());
                applyNetworkRepoMirrors(body, owner, name, row, generation);
            });
}

// True when `row` still shows owner/name for this load generation and has a
// mirrors cell to paint. A reply (or a cache replay) can land after the table
// has been rebuilt, so every writer checks row identity, not just the index.
bool MainWindow::networkRepoMirrorCell(const QString &owner, const QString &name,
                                       int row, int generation) const
{
    if (generation != m_networkReposLoadGen || !m_networkReposTable || row < 0 ||
        row >= m_networkReposTable->rowCount())
        return false;
    const QTableWidgetItem *repoItem =
        m_networkReposTable->item(row, kRepoColName);
    if (!repoItem ||
        repoItem->data(Qt::UserRole + 3).toString()
                .compare(owner, Qt::CaseInsensitive) != 0 ||
        repoItem->data(Qt::UserRole + 4).toString()
                .compare(name, Qt::CaseInsensitive) != 0)
        return false;
    return m_networkReposTable->item(row, kRepoColMirrors) != nullptr;
}

// Paint one row's mirror tally from a `/api/repo/*/mirrors` body. Shared by the
// live reply and the cache replay above so both render identically.
void MainWindow::applyNetworkRepoMirrors(const QByteArray &body,
                                         const QString &owner,
                                         const QString &name, int row,
                                         int generation)
{
    if (!networkRepoMirrorCell(owner, name, row, generation))
        return;
    QTableWidgetItem *mirrorsItem =
        m_networkReposTable->item(row, kRepoColMirrors);

    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    if (obj.contains("ok") && !obj.value("ok").toBool()) {
        mirrorsItem->setText(QString::fromUtf8("\xE2\x80\x94"));
        mirrorsItem->setToolTip(QString());
        mirrorsItem->setForeground(Theme::remapColor(QColor("#8b949e")));
        reposNextSyncCardMirrors(m_networkReposTable, row);
        return;
    }

    const QJsonArray mirrors = obj.value("mirrors").toArray();
    QStringList mirrorToolTip;
    QSet<QString> countedNodes;
    for (const QJsonValue &value : mirrors) {
        const QJsonObject mirror = value.toObject();
        const QString mirrorSource = mirror.value("source").toString().trimmed();
        const QString mirrorRepo = mirror.value("repo").toString().trimmed();
        if (mirrorSource == QLatin1String("local-node") ||
            (mirror.value("owner").toString().compare(owner, Qt::CaseInsensitive) == 0 &&
             mirrorRepo.compare(name, Qt::CaseInsensitive) == 0))
            continue;
        QString node = mirror.value("node").toString().trimmed();
        if (node.isEmpty())
            node = mirror.value("owner").toString().trimmed();
        if (node.isEmpty())
            node = mirror.value("name").toString().trimmed();
        if (node.isEmpty())
            node = mirror.value("id").toString().trimmed();
        if (node.isEmpty())
            continue;
        const QString nodeKey = node.toLower();
        if (countedNodes.contains(nodeKey))
            continue;
        countedNodes.insert(nodeKey);
        const QString status =
            mirror.value("status").toString(
                mirror.value("cloneStatus").toString()).trimmed();
        const QString commit = mirror.value("commit").toString().trimmed();
        QString tip = node;
        if (!status.isEmpty())
            tip += QStringLiteral(" - %1").arg(status);
        if (!commit.isEmpty())
            tip += QStringLiteral(" - commit %1").arg(commit);
        mirrorToolTip << tip;
    }

    const int count = countedNodes.size();
    mirrorsItem->setData(Qt::DisplayRole, count);
    mirrorsItem->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    mirrorsItem->setToolTip(count == 0
                                ? QStringLiteral("No mirror nodes yet")
                                : mirrorToolTip.join('\n'));
    mirrorsItem->setForeground(count == 0 ? QBrush(Theme::remapColor(QColor("#8b949e")))
                                          : QBrush());
    reposNextSyncCardMirrors(m_networkReposTable, row);
}

void MainWindow::openNetworkRepo(const QString &owner, const QString &name,
                                 const QString &cloneUrl, bool isPrivate)
{
    if (owner.isEmpty() || name.isEmpty())
        return;

    int index = findNetworkRepoIndex(owner, name, true);
    if (index >= 0) {
        if (m_repositories.at(index).isPrivate &&
            PrivateMirrorStore::isOpaqueId(
                m_privateCatalogAccessIds.value(owner + "/" + name))) {
            m_repositories[index].privateReplicaId =
                m_privateCatalogAccessIds.value(owner + "/" + name);
        }
        openRepoDetail(index);
        const RepositoryRecord &repo = m_repositories.at(index);
        if (repo.previewOnly && !m_syncingRepos.contains(index) &&
            !QDir(repo.mirrorPath).exists())
            syncRepository(index);
        return;
    }

    RepositoryRecord repo;
    repo.owner = owner;
    repo.name = name;
    repo.cloneUrl = cloneUrl.isEmpty() ? hostedCloneUrl(owner, name) : cloneUrl;
    repo.isPrivate = isPrivate;
    if (isPrivate) {
        const QString accessId =
            m_privateCatalogAccessIds.value(owner + "/" + name);
        if (PrivateMirrorStore::isOpaqueId(accessId))
            repo.privateReplicaId = accessId;
    }
    repo.previewOnly = true;
    repo.actionsEnabled = false;
    repo.hostedSinceMs = QDateTime::currentMSecsSinceEpoch();
    repo.mirrorPath = repositoryPreviewPath(owner, name);
    stampRepositoryRelay(repo);
    m_repositories.append(repo);
    index = m_repositories.size() - 1;
    refreshRepositoryList();
    logSystem("Preview: caching " + owner + "/" + name + " from " + repo.cloneUrl);
    openRepoDetail(index);
    syncRepository(index);
}

void MainWindow::openRepositoryForkFromSystemLink(const QString &target)
{
    const QUrl url(target);
    const QStringList parts =
        url.path(QUrl::FullyDecoded).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    static const QRegularExpression segment(
        QStringLiteral("^[A-Za-z0-9._:-]{1,80}$"));
    if (!url.isValid() || url.scheme() != QLatin1String("forkmesh") ||
        url.host() != QLatin1String("fork") || !url.userInfo().isEmpty() ||
        url.port(-1) != -1 || !url.query().isEmpty() ||
        !url.fragment().isEmpty() || parts.size() != 2 ||
        !segment.match(parts.at(0)).hasMatch() ||
        !segment.match(parts.at(1)).hasMatch()) {
        flashMessage(QStringLiteral("The repository fork link is invalid."), true);
        return;
    }
    const QString owner = parts.at(0).toLower();
    const QString name = parts.at(1);
    // Always enter through the stable mesh route. It can select any eligible
    // mirror and does not require the source-of-truth node to be online.
    forkNetworkRepo(owner, name, hostedCloneUrl(owner, name), false);
}

void MainWindow::forkNetworkRepo(const QString &owner, const QString &name,
                                 const QString &cloneUrl, bool isPrivate)
{
    const int localFork = findNetworkLocalForkIndex(owner, name);
    if (localFork >= 0) {
        if (localFork < m_repositories.size() &&
            m_repositories.at(localFork).owner != owner)
            configureForkSource(localFork, cloneUrl, false);
        openRepoDetail(localFork);
        return;
    }
    m_pendingForkRepoKey = (owner + QLatin1Char('/') + name).toLower();
    openNetworkRepo(owner, name, cloneUrl, isPrivate);
    const int preview = findNetworkRepoIndex(owner, name, true);
    if (preview >= 0) {
        const RepositoryRecord cached = m_repositories.at(preview);
        const bool usable =
            QFileInfo(QDir(cached.mirrorPath)
                          .filePath(QStringLiteral("HEAD")))
                .isFile();
        // A killed clone can leave the preview directory behind without a Git
        // HEAD. openNetworkRepo normally treats an existing directory as a
        // cache hit, so repair that bounded, disposable cache here instead of
        // leaving the website handoff pending forever.
        if (!usable && !m_syncingRepos.contains(preview) &&
            QDir(cached.mirrorPath).exists()) {
            detachForksBorrowingMirror(cached.mirrorPath);
            if (!forkmesh::safedelete::removeTree(cached.mirrorPath,
                                                  repositoryPreviewRoot())) {
                m_pendingForkRepoKey.clear();
                flashMessage(
                    QStringLiteral(
                        "Could not reset the incomplete cache for %1/%2; the "
                        "fork was not started.")
                        .arg(owner, name),
                    true);
                return;
            }
            syncRepository(preview);
        }
        if (completePendingRepoFork(preview))
            return;
    }
    flashMessage(QStringLiteral("Caching %1/%2 from an available mirror before forking…")
                     .arg(owner, name));
}

void MainWindow::mirrorNetworkRepo(const QString &owner, const QString &name,
                                   const QString &cloneUrl, bool isPrivate)
{
    if (findNetworkRepoIndex(owner, name, false) >= 0) {
        flashMessage(QStringLiteral("This machine already mirrors %1/%2.")
                         .arg(owner, name));
        refreshNetworkReposPage();
        return;
    }

    const QString source = cloneUrl.isEmpty() ? hostedCloneUrl(owner, name) : cloneUrl;
    if (source.isEmpty()) {
        flashMessage(QStringLiteral("No clone URL is available for %1/%2.")
                         .arg(owner, name),
                     true);
        return;
    }

    mirrorCatalogRepo(owner, name, source, isPrivate);
    flashMessage(QStringLiteral("Mirroring %1/%2.").arg(owner, name));
    refreshNetworkReposPage();
}

// Whether this device can sign an administrator's repository purge: an admin
// account, signed in here with its own key, on a build that holds that key.
// The relay checks is_admin again on the account that signed, so this is only
// about not offering a button that could not possibly work.
bool MainWindow::canAdminPurgeRepos() const
{
    const QString account = accountOwner();
    return m_isAdmin && !account.isEmpty() && m_profileIdentity.isValid() &&
           hasOwnerSigningCapability(account);
}

// Erase one repository from the network: the catalog row and every record the
// relay keeps about it, whether or not this machine ever held a copy
// (adhoc #196). The local Delete button beside this one only ever removed
// this machine's copy, so a repository that was never cloned here had nothing
// it could delete.
void MainWindow::adminPurgeNetworkRepo(const QString &owner,
                                       const QString &name)
{
    if (!m_networkAccess || owner.isEmpty() || name.isEmpty())
        return;
    const QString admin = accountOwner();
    if (!canAdminPurgeRepos()) {
        flashMessage(QStringLiteral(
                         "Only a signed-in administrator can erase %1/%2 from "
                         "the network.")
                         .arg(owner, name),
                     true);
        return;
    }

    const QString target = owner + QLatin1Char('/') + name;
    // QStringLiteral cannot carry \xNN UTF-8 bytes (it is a u"" literal, so
    // each byte would become its own code unit); fromUtf8 decodes them.
    QMessageBox box(
        QMessageBox::Warning, QStringLiteral("Erase repository"),
        QString::fromUtf8(
            "Erase %1 from the network as an administrator?\n\n"
            "This deletes every record the relay keeps about it:\n\n"
            "  \xE2\x80\xA2  its catalog entry, stars and fork edges\n"
            "  \xE2\x80\xA2  its issue, pull and discussion inboxes\n"
            "  \xE2\x80\xA2  its chat rooms, agent state and scan history\n"
            "  \xE2\x80\xA2  its fediverse actor and followers\n\n"
            "Republishing is then refused, so the owner's node cannot bring "
            "it back on its next heartbeat. Checkouts and mirrors already on "
            "other machines are not touched \xE2\x80\x94 nothing can reach "
            "into them.")
            .arg(target),
        QMessageBox::Cancel, this);
    QPushButton *go = box.addButton(QString::fromUtf8("Erase\xE2\x80\xA6"),
                                    QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != go)
        return;

    bool ok = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Confirm erase"),
        QStringLiteral("Type %1 to erase it from the network:").arg(target),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || typed.trimmed().compare(target, Qt::CaseInsensitive) != 0)
        return;

    const QString ts =
        QString::number(QDateTime::currentMSecsSinceEpoch());
    const QString signature = m_profileIdentity.signData(
        forkmesh::catalogAdminDeleteCanonical(admin, owner, name, ts));
    if (signature.isEmpty()) {
        flashMessage(QStringLiteral("Could not sign the request to erase %1.")
                         .arg(target),
                     true);
        return;
    }
    QUrl url = catalogApiUrl();
    url.setQuery(forkmesh::catalogAdminDeleteQuery(admin, owner, name, ts,
                                                   signature));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader(QByteArrayLiteral("accept"),
                         QByteArrayLiteral("application/json"));
    request.setTransferTimeout(20000);
    logSystem(QStringLiteral("Admin: erasing %1 from the network.").arg(target));
    // deleteResource, not sendCustomRequest("DELETE"): a custom verb is
    // recorded as the "CUSTOM" method, which no endpoint-catalog entry can
    // describe, so the request would show up in the network log with no
    // purpose beside it.
    QNetworkReply *reply = m_networkAccess->deleteResource(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, owner, name, target] {
                const QByteArray body = reply->readAll();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QNetworkReply::NetworkError error = reply->error();
                const QString errorString = reply->errorString();
                reply->deleteLater();
                const QJsonObject parsed =
                    QJsonDocument::fromJson(body).object();
                if (error != QNetworkReply::NoError ||
                    !parsed.value(QStringLiteral("ok")).toBool()) {
                    const QString detail =
                        parsed.value(QStringLiteral("error")).toString();
                    flashMessage(
                        QStringLiteral("Could not erase %1: %2")
                            .arg(target, detail.isEmpty()
                                             ? (status > 0
                                                    ? QStringLiteral("HTTP %1")
                                                          .arg(status)
                                                    : errorString)
                                             : detail),
                        true);
                    logSystem(QStringLiteral("Admin erase of %1 failed: %2")
                                  .arg(target, detail.isEmpty() ? errorString
                                                                : detail));
                    return;
                }
                // The repository is gone from the relay, so this machine's
                // browsing cache for it is a record of something that no
                // longer exists. It is a disposable cache, so drop it without
                // another prompt.
                discardNetworkRepoPreview(owner, name);
                const bool hadRow =
                    parsed.value(QStringLiteral("deleted")).toBool();
                flashMessage(
                    hadRow
                        ? QStringLiteral("Erased %1 from the network.")
                              .arg(target)
                        : QStringLiteral(
                              "%1 had no catalog entry left; erased what "
                              "remained of it.")
                              .arg(target));
                logSystem(QStringLiteral("Admin: erased %1 from the network.")
                              .arg(target));
                refreshNetworkReposPage();
            });
}

// Drop this machine's preview clone of a repository that no longer exists.
// Previews are a disposable browsing cache, never a copy anyone serves, so
// they are removed without the confirmation a real local copy gets.
void MainWindow::discardNetworkRepoPreview(const QString &owner,
                                           const QString &name)
{
    const int index = findNetworkRepoIndex(owner, name, true);
    if (index < 0 || !m_repositories.at(index).previewOnly)
        return;
    const QString path = m_repositories.at(index).mirrorPath.trimmed();
    if (!path.isEmpty() && QDir(path).exists()) {
        detachForksBorrowingMirror(path);
        QString refused;
        if (!forkmesh::safedelete::removeTree(path, repositoryPreviewRoot(), &refused))
            logSystem(QStringLiteral("Preview %1 was kept: %2").arg(path, refused));
    }
    m_repositories.removeAt(index);
    saveRepositories();
    if (m_repoDetailIndex == index)
        m_repoDetailIndex = -1;
    else if (m_repoDetailIndex > index)
        --m_repoDetailIndex;
    refreshRepositoryList();
}
