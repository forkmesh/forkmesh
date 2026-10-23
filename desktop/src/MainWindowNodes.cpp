
#include "Tooltips.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SettingsSectionsView.h"
#include "MainWindowNetworkInternal.h"
#include "SystemPressure.h"
#include "AgentCliLocator.h"
#include "AgentLoginPty.h"
#include "AgentWorktree.h"
#include "RepoStatsStore.h"
#include "ActionStore.h"
#include "ControlNode.h"
#include "CurrentPageStack.h"
#include "MirrorSyncVerdict.h"
#include "MirrorNodeColumns.h"
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "KebabHeaderView.h"
#include "LogTimelineChart.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PromptDraftStore.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "RewardPoolSigner.h"
#include "TerminalWidget.h"
#include "WorldSpeechBridge.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QUrlQuery>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <signal.h>
#endif

using namespace forkmesh::ui;

// --- Nodes ------------------------------------------------------------------
// A sortable directory of every registered node the relay exposes through its
// public user directory, merged with live roster and serving-only nodes. Each row
// carries the node's platform badge, name, online state, owner, advertised
// ForkMesh version, repo/mirror counts and its CPU/RAM/disk telemetry bars.
// Online state trusts the relay's /api/network/stats "onlineNodes" list as the
// canonical set — the same signal the Mirror nodes list and the Network page
// use. That lets headless mirrors serving via the relay without joining this
// client's chat room show online and stops a stale roster entry
// painting a node online after it stopped serving. Our own node is
// the exception: it trusts the local backend, since it may host only private
// repos the relay never lists. Before the first relay reply we fall back to the
// encrypted roster's presence flag.

namespace {
// The website's Mirrors table (dashboard.js mirrorTableHead) is the reference
// layout: every column it publishes for a node is repeated here in the same
// order, with the desktop-only columns (owner, repo/mirror counts, host
// telemetry, node id, admin actions) slotted in beside their neighbours.
enum NodeCol {
    kNodeColName = 0,
    kNodeColRole,
    kNodeColScope,
    kNodeColStatus,
    kNodeColActivity,
    kNodeColOwner,
    kNodeColVersion,
    kNodeColRevision,
    kNodeColMessage,
    kNodeColAuthor,
    kNodeColSynced,
    kNodeColEndpoint,
    kNodeColChecked,
    kNodeColLatency,
    kNodeColRegion,
    kNodeColIntegrity,
    kNodeColRoute,
    kNodeColReachability,
    kNodeColCaps,
    kNodeColPlatform,
    kNodeColRepos,
    kNodeColMirrors,
    kNodeColSize,
    kNodeColGitDatabase,
    kNodeColIssues,
    kNodeColIssuesToSource,
    kNodeColCommits,
    kNodeColCommitsToSource,
    kNodeColBranches,
    kNodeColBranchesToSource,
    kNodeColPulls,
    kNodeColPullsToSource,
    kNodeColDiscussions,
    kNodeColDiscussionsToSource,
    kNodeColArtifacts,
    kNodeColWorktrees,
    kNodeColClones,
    kNodeColWebsite,
    kNodeColHealth,
    kNodeColCpu,
    kNodeColRam,
    kNodeColDisk,
    kNodeColId,
    kNodeColActions,
    kNodeColCount,
};
bool isProtectedMeshNode(const QString &node)
{
    const QString key = node.trimmed().toLower();
    return key.isEmpty() || key == QLatin1String("forkmesh") ||
           key == QLatin1String("forkmesh-mainnode");
}

QString nextNodeDash()
{
    return QString::fromUtf8("\xE2\x80\x94");
}

QString nextNodeCellText(const QTableWidget *table, int row, int column)
{
    const QTableWidgetItem *item = table->item(row, column);
    if (!item)
        return QString();
    const QString text = item->text().trimmed();
    return text == nextNodeDash() ? QString() : text;
}

struct NextNodeName {
    QString name;
    bool self = false;
    bool source = false;
    bool child = false;
};

NextNodeName nextSplitNodeName(QString text)
{
    NextNodeName out;
    const QString childPrefix = QString::fromUtf8("    \xE2\x94\x94 ");
    if (text.startsWith(childPrefix)) {
        out.child = true;
        text = text.mid(childPrefix.size());
    }
    const QString star = QString::fromUtf8("  \xE2\x98\x85 source of truth");
    if (text.contains(star)) {
        out.source = true;
        text.remove(star);
    }
    for (const QString &mark :
         {QStringLiteral("  (this machine)"), QStringLiteral("  (you)")}) {
        if (text.contains(mark)) {
            out.self = true;
            text.remove(mark);
        }
    }
    out.name = text.trimmed();
    return out;
}

bool nextNodeRowIsChild(const QTableWidget *table, int row)
{
    const QTableWidgetItem *name = table->item(row, kNodeColName);
    return name && name->data(kNodeGroupChildRole).toBool();
}

QString nextNodeRowKey(const QTableWidget *table, int row)
{
    for (int r = row; r >= 0; --r) {
        const QTableWidgetItem *name = table->item(r, kNodeColName);
        if (!name)
            return QString();
        if (name->data(kNodeGroupChildRole).toBool())
            continue;
        return name->data(Qt::UserRole + 5).toString() + QLatin1Char('|') +
               name->data(Qt::UserRole).toString().trimmed().toLower();
    }
    return QString();
}

int nextNodeRowForKey(const QTableWidget *table, const QString &key)
{
    if (key.isEmpty())
        return -1;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (!nextNodeRowIsChild(table, row) && nextNodeRowKey(table, row) == key)
            return row;
    }
    return -1;
}

bool nextNodeOnline(const QModelIndex &index)
{
    const QModelIndex name = index.siblingAtColumn(kNodeColName);
    if (name.data(kNodeGroupChildRole).toBool())
        return name.data(kMirrorNodeOnlineRole).toBool();
    return index.siblingAtColumn(kNodeColStatus)
        .data()
        .toString()
        .startsWith(QLatin1String("Online"));
}

QColor nextNodeDotColor(const QModelIndex &index)
{
    const int light = index.siblingAtColumn(kNodeColName).data(kNodeLightRole).toInt();
    if (light == 2)
        return Theme::color(Theme::Role::Danger);
    if (light == 1)
        return Theme::color(Theme::Role::Warning);
    return nextNodeOnline(index) ? Theme::color(Theme::Role::Success)
                                 : Theme::color(Theme::Role::FgSubtle);
}

QString nextNodeRoleIcon(const QString &roleKey)
{
    if (roleKey == QLatin1String("core"))
        return QStringLiteral("server");
    if (roleKey == QLatin1String("runner"))
        return QStringLiteral("terminal");
    if (roleKey == QLatin1String("mirror"))
        return QStringLiteral("sync");
    if (roleKey == QLatin1String("relay"))
        return QStringLiteral("broadcast");
    return QStringLiteral("device-desktop");
}

QFont nextNodeFont(const QFont &base, int px, bool mono, QFont::Weight weight)
{
    QFont font = base;
    if (mono)
        font.setFamily(Theme::monoFontFamily());
    font.setPixelSize(px);
    font.setWeight(weight);
    return font;
}

struct NextNodeBadge {
    QString text;
    bool soft = false;
};

class NextNodeCellDelegate : public QStyledItemDelegate
{
public:
    enum Kind { Name, Muted, Mono };

    NextNodeCellDelegate(Kind kind, QObject *parent)
        : QStyledItemDelegate(parent), m_kind(kind)
    {
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect cell = option.rect.adjusted(kPad, 0, -kPad, 0);
        if (m_kind == Name)
            paintName(painter, cell, option, index);
        else
            paintText(painter, cell, option, index);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        const QSize base = QStyledItemDelegate::sizeHint(option, index);
        if (m_kind != Name) {
            const QFontMetrics metrics(textFont(option));
            return QSize(metrics.horizontalAdvance(displayText(index)) + 2 * kPad + 4,
                         base.height());
        }
        const NextNodeName split = nextSplitNodeName(index.data().toString());
        const QFontMetrics metrics(nameFont(option, index));
        int width = 2 * kPad + kDot + kGap + metrics.horizontalAdvance(split.name) + 4;
        if (split.child)
            width += kIndent;
        const QFontMetrics badgeMetrics(badgeFont(option));
        for (const NextNodeBadge &badge : badges(index, split))
            width += kGap + badgeMetrics.horizontalAdvance(badge.text) + 10;
        return QSize(width, base.height());
    }

private:
    static constexpr int kPad = 10;
    static constexpr int kDot = 6;
    static constexpr int kGap = 8;
    static constexpr int kIndent = 18;

    QFont textFont(const QStyleOptionViewItem &option) const
    {
        return m_kind == Mono ? nextNodeFont(option.font, 12, true, QFont::Normal)
                              : nextNodeFont(option.font, 13, false, QFont::Normal);
    }

    static QFont nameFont(const QStyleOptionViewItem &option, const QModelIndex &index)
    {
        const QFont itemFont = index.data(Qt::FontRole).value<QFont>();
        const bool strong = index.data(Qt::FontRole).isValid() && itemFont.bold();
        return nextNodeFont(option.font, 12, true,
                            strong ? QFont::Medium : QFont::Normal);
    }

    static QFont badgeFont(const QStyleOptionViewItem &option)
    {
        return nextNodeFont(option.font, 10, false, QFont::Medium);
    }

    static QString displayText(const QModelIndex &index)
    {
        QString text = index.data().toString();
        text.replace(QStringLiteral(" (serving)"), QString::fromUtf8(" \xC2\xB7 serving"));
        return text;
    }

    static QList<NextNodeBadge> badges(const QModelIndex &index, const NextNodeName &split)
    {
        QList<NextNodeBadge> out;
        if (split.self)
            out.append({QStringLiteral("You"), true});
        if (split.source)
            out.append({QStringLiteral("Source of truth"), false});
        const QModelIndex name = index.siblingAtColumn(kNodeColName);
        if (name.data(Qt::UserRole + 5).toString() == QLatin1String("relay") &&
            index.siblingAtColumn(kNodeColScope).data().toString() ==
                QLatin1String("active"))
            out.append({QStringLiteral("Active relay"), false});
        return out;
    }

    void paintText(QPainter *painter, const QRect &cell,
                   const QStyleOptionViewItem &option, const QModelIndex &index) const
    {
        const QVariant foreground = index.data(Qt::ForegroundRole);
        QColor color = Theme::color(Theme::Role::FgMuted);
        if (foreground.isValid())
            color = foreground.value<QBrush>().color();
        const QFont font = textFont(option);
        painter->setFont(font);
        painter->setPen(color);
        const QVariant alignment = index.data(Qt::TextAlignmentRole);
        Qt::Alignment align = alignment.isValid()
                                  ? Qt::Alignment(alignment.toInt())
                                  : Qt::Alignment(Qt::AlignLeft | Qt::AlignVCenter);
        align = (align & Qt::AlignHorizontal_Mask) | Qt::AlignVCenter;
        const QString text =
            QFontMetrics(font).elidedText(displayText(index), Qt::ElideRight, cell.width());
        painter->drawText(cell, int(align), text);
    }

    void paintName(QPainter *painter, QRect cell, const QStyleOptionViewItem &option,
                   const QModelIndex &index) const
    {
        const NextNodeName split = nextSplitNodeName(index.data().toString());
        const QFont font = nameFont(option, index);
        const QFontMetrics metrics(font);
        if (split.child) {
            painter->setFont(font);
            painter->setPen(Theme::color(Theme::Role::FgSubtle));
            painter->drawText(QRect(cell.left(), cell.top(), kIndent, cell.height()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              QString::fromUtf8("\xE2\x94\x94"));
            cell.setLeft(cell.left() + kIndent);
        }
        const int centerY = cell.center().y();
        painter->setPen(Qt::NoPen);
        painter->setBrush(nextNodeDotColor(index));
        painter->drawEllipse(QRectF(cell.left(), centerY - kDot / 2.0 + 0.5, kDot, kDot));
        cell.setLeft(cell.left() + kDot + kGap);

        const QList<NextNodeBadge> list = badges(index, split);
        const QFont small = badgeFont(option);
        const QFontMetrics smallMetrics(small);
        int badgesWidth = 0;
        for (const NextNodeBadge &badge : list)
            badgesWidth += kGap + smallMetrics.horizontalAdvance(badge.text) + 10;
        const int nameRoom = qMax(0, cell.width() - badgesWidth);
        const QString name = metrics.elidedText(split.name, Qt::ElideRight, nameRoom);
        painter->setFont(font);
        painter->setPen(split.child ? Theme::color(Theme::Role::FgMuted)
                                    : Theme::color(Theme::Role::Fg));
        painter->drawText(QRect(cell.left(), cell.top(), nameRoom, cell.height()),
                          Qt::AlignLeft | Qt::AlignVCenter, name);
        int x = cell.left() + metrics.horizontalAdvance(name);
        painter->setFont(small);
        for (const NextNodeBadge &badge : list) {
            x += kGap;
            const int width = smallMetrics.horizontalAdvance(badge.text) + 10;
            if (x + width > cell.right() + 1)
                break;
            const QRectF pill(x, centerY - 8 + 0.5, width, 16);
            if (badge.soft) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(Theme::color(Theme::Role::AccentSubtle));
                painter->drawRoundedRect(pill, 4, 4);
                painter->setPen(Theme::color(Theme::Role::AccentFg));
            } else {
                painter->setPen(QPen(Theme::color(Theme::Role::BorderStrong), 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRoundedRect(pill.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
                painter->setPen(Theme::color(Theme::Role::FgMuted));
            }
            painter->drawText(pill, Qt::AlignCenter, badge.text);
            x += width;
        }
    }

    Kind m_kind;
};

class NextNodesTableChrome : public QObject
{
public:
    explicit NextNodesTableChrome(QTableWidget *table) : QObject(table), m_table(table)
    {
        table->viewport()->setMouseTracking(true);
        table->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != m_table->viewport())
            return false;
        switch (event->type()) {
        case QEvent::MouseMove:
            setHovered(m_table->rowAt(
                static_cast<QMouseEvent *>(event)->position().toPoint().y()));
            break;
        case QEvent::Leave:
            setHovered(-1);
            break;
        case QEvent::Paint:
            paintBands();
            break;
        default:
            break;
        }
        return false;
    }

private:
    void setHovered(int row)
    {
        if (row == m_hovered)
            return;
        const int before = m_hovered;
        m_hovered = row;
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

    void paintBands()
    {
        const int rows = m_table->rowCount();
        if (rows == 0)
            return;
        QWidget *viewport = m_table->viewport();
        int first = m_table->rowAt(0);
        int last = m_table->rowAt(viewport->height() - 1);
        if (first < 0)
            first = 0;
        if (last < 0)
            last = rows - 1;
        const QString key = m_table->property("nextPanelKey").toString();
        const QString hoveredKey =
            m_hovered >= 0 ? nextNodeRowKey(m_table, m_hovered) : QString();
        QPainter painter(viewport);
        const int width = viewport->width();
        const QColor hairline = Theme::color(Theme::Role::Border);
        for (int row = first; row <= last; ++row) {
            if (m_table->isRowHidden(row))
                continue;
            const int top = m_table->rowViewportPosition(row);
            const int height = m_table->rowHeight(row);
            const QString rowKey = nextNodeRowKey(m_table, row);
            if (!key.isEmpty() && rowKey == key)
                painter.fillRect(0, top, width, height,
                                 Theme::color(Theme::Role::SurfaceActive));
            else if (!hoveredKey.isEmpty() && rowKey == hoveredKey)
                painter.fillRect(0, top, width, height,
                                 Theme::color(Theme::Role::SurfaceRaised));
            painter.fillRect(0, top + height - 1, width, 1, hairline);
        }
    }

    QTableWidget *m_table = nullptr;
    int m_hovered = -1;
};

class NextNodeMeter : public QWidget
{
public:
    explicit NextNodeMeter(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(6);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setValue(int value)
    {
        if (m_value == value)
            return;
        m_value = value;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        const QRectF track(rect());
        const qreal radius = track.height() / 2.0;
        painter.setBrush(Theme::color(Theme::Role::SurfaceActive));
        painter.drawRoundedRect(track, radius, radius);
        if (m_value <= 0)
            return;
        QRectF fill = track;
        fill.setWidth(track.width() * qMin(100, m_value) / 100.0);
        painter.setBrush(m_value > 80 ? Theme::color(Theme::Role::Fg, 180)
                                      : Theme::color(Theme::Role::Accent));
        painter.drawRoundedRect(fill, radius, radius);
    }

private:
    int m_value = -1;
};

QFrame *nextStatTile(const QString &objectName, const QString &label, QWidget *parent)
{
    auto *tile = new QFrame(parent);
    tile->setObjectName(objectName);
    tile->setProperty("networkRole", QStringLiteral("statTile"));
    tile->setAttribute(Qt::WA_StyledBackground, true);
    tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    tile->setMinimumWidth(120);
    auto *layout = new QVBoxLayout(tile);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(4);
    auto *caption = new QLabel(label, tile);
    caption->setObjectName(QStringLiteral("networkStatLabel"));
    auto *value = new QLabel(nextNodeDash(), tile);
    value->setObjectName(QStringLiteral("networkStatValue"));
    value->setTextFormat(Qt::PlainText);
    auto *hint = new QLabel(tile);
    hint->setObjectName(QStringLiteral("networkStatHint"));
    hint->setTextFormat(Qt::PlainText);
    for (QLabel *text : {caption, value, hint}) {
        text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(text);
    }
    return tile;
}

void nextSetStatTile(QWidget *scope, const QString &objectName, const QString &value,
                     const QString &hint)
{
    auto *tile = scope ? scope->findChild<QFrame *>(objectName) : nullptr;
    if (!tile)
        return;
    if (auto *label = tile->findChild<QLabel *>(QStringLiteral("networkStatValue")))
        label->setText(value.isEmpty() ? nextNodeDash() : value);
    if (auto *label = tile->findChild<QLabel *>(QStringLiteral("networkStatHint")))
        label->setText(hint);
}

void nextComposeNetworkSummary(QLabel *label)
{
    if (!label)
        return;
    QStringList parts;
    for (const char *key : {"nextNodesPart", "nextDiagPart"}) {
        const QString part = label->property(key).toString();
        if (!part.isEmpty())
            parts.append(part);
    }
    label->setText(parts.join(QString::fromUtf8(" \xC2\xB7 ")));
}

QString nextPlural(int count, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? one : many);
}

void nextRefreshNodeStats(QTableWidget *table, QLabel *summary)
{
    QWidget *scope = table->parentWidget();
    int total = 0;
    int online = 0;
    QMap<QString, int> offlineByRole;
    QList<double> latencies;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (nextNodeRowIsChild(table, row))
            continue;
        ++total;
        if (nextNodeCellText(table, row, kNodeColStatus).startsWith(QLatin1String("Online")))
            ++online;
        else
            ++offlineByRole[nextNodeCellText(table, row, kNodeColRole).toLower()];
        const QTableWidgetItem *latency = table->item(row, kNodeColLatency);
        bool ok = false;
        const double ms = latency ? latency->data(kTableSortRole).toDouble(&ok) : -1.0;
        if (ok && ms >= 0 && latency->text().endsWith(QLatin1String(" ms")))
            latencies.append(ms);
    }

    QString onlineHint;
    const int offline = total - online;
    if (total == 0)
        onlineHint = QStringLiteral("No nodes known yet");
    else if (offline == 0)
        onlineHint = QStringLiteral("All online");
    else if (offlineByRole.size() == 1 && !offlineByRole.firstKey().isEmpty())
        onlineHint = QStringLiteral("%1 %2 offline").arg(offline).arg(offlineByRole.firstKey());
    else
        onlineHint = QStringLiteral("%1 offline").arg(offline);
    nextSetStatTile(scope, QStringLiteral("networkStatOnline"),
                    total > 0 ? QStringLiteral("%1 / %2").arg(online).arg(total)
                              : QString(),
                    onlineHint);

    QString median;
    if (!latencies.isEmpty()) {
        std::sort(latencies.begin(), latencies.end());
        const int mid = int(latencies.size()) / 2;
        const double value = latencies.size() % 2
                                 ? latencies.at(mid)
                                 : (latencies.at(mid - 1) + latencies.at(mid)) / 2.0;
        median = QStringLiteral("%1 ms").arg(qRound(value));
    }
    nextSetStatTile(scope, QStringLiteral("networkStatLatency"), median,
                    latencies.isEmpty()
                        ? QStringLiteral("No node measured yet")
                        : nextPlural(int(latencies.size()), QStringLiteral("node measured"),
                                     QStringLiteral("nodes measured")));

    const QVariant served = table->property("nextReposServed");
    const int servingNodes = table->property("nextReposNodes").toInt();
    nextSetStatTile(scope, QStringLiteral("networkStatRepos"),
                    served.isValid() ? QString::number(served.toInt()) : QString(),
                    servingNodes > 0
                        ? QStringLiteral("across %1")
                              .arg(nextPlural(servingNodes, QStringLiteral("node"),
                                              QStringLiteral("nodes")))
                        : QStringLiteral("No mirrors reported"));

    if (summary) {
        summary->setProperty("nextNodesPart",
                             total > 0 ? QStringLiteral("%1 of %2 nodes online")
                                             .arg(online)
                                             .arg(total)
                                       : QString());
        nextComposeNetworkSummary(summary);
    }
}

void nextApplyNodeTextFilter(QTableWidget *table)
{
    const QString needle = table->property("nextNodeFilter").toString();
    if (needle.isEmpty())
        return;
    bool headShown = false;
    for (int row = 0; row < table->rowCount(); ++row) {
        if (nextNodeRowIsChild(table, row)) {
            if (!headShown)
                table->setRowHidden(row, true);
            continue;
        }
        headShown = false;
        if (table->isRowHidden(row))
            continue;
        for (int column : {int(kNodeColName), int(kNodeColRole), int(kNodeColOwner),
                           int(kNodeColStatus), int(kNodeColRegion),
                           int(kNodeColPlatform), int(kNodeColEndpoint)}) {
            const QTableWidgetItem *item = table->item(row, column);
            if (item && item->text().contains(needle, Qt::CaseInsensitive)) {
                headShown = true;
                break;
            }
        }
        if (!headShown)
            table->setRowHidden(row, true);
    }
}

QLabel *nextPanelLabel(const QString &objectName, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setObjectName(objectName);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QFrame *nextBuildNodePanel(QWidget *parent)
{
    auto *panel = new QFrame(parent);
    panel->setObjectName(QStringLiteral("networkNodePanel"));
    panel->setAttribute(Qt::WA_StyledBackground, true);
    panel->setFixedWidth(320);
    panel->hide();
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *head = new QWidget(panel);
    head->setObjectName(QStringLiteral("networkNodePanelHead"));
    head->setAttribute(Qt::WA_StyledBackground, true);
    auto *headRow = new QHBoxLayout(head);
    headRow->setContentsMargins(16, 14, 10, 14);
    headRow->setSpacing(12);
    auto *icon = nextPanelLabel(QStringLiteral("networkNodePanelIcon"), head);
    icon->setFixedSize(40, 40);
    icon->setAlignment(Qt::AlignCenter);
    headRow->addWidget(icon, 0, Qt::AlignVCenter);
    auto *titles = new QVBoxLayout;
    titles->setContentsMargins(0, 0, 0, 0);
    titles->setSpacing(2);
    auto *nameRow = new QHBoxLayout;
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(8);
    auto *name = nextPanelLabel(QStringLiteral("networkNodePanelName"), head);
    name->setFont(nextNodeFont(name->font(), 13, true, QFont::Medium));
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    nameRow->addWidget(name, 1);
    auto *you = new kit::Badge(QStringLiteral("This node"), kit::Tone::Accent, false, head);
    you->hide();
    nameRow->addWidget(you, 0, Qt::AlignVCenter);
    titles->addLayout(nameRow);
    auto *sub = nextPanelLabel(QStringLiteral("networkNodePanelSub"), head);
    sub->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    titles->addWidget(sub);
    headRow->addLayout(titles, 1);
    auto *close = new QToolButton(head);
    close->setObjectName(QStringLiteral("networkNodePanelClose"));
    close->setAutoRaise(true);
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("Close"));
    close->setAccessibleName(QStringLiteral("Close node details"));
    close->setIcon(QIcon(kit::tintedIcon(QStringLiteral("x"),
                                         Theme::color(Theme::Role::FgMuted), 16,
                                         panel->devicePixelRatioF())));
    close->setIconSize(QSize(16, 16));
    close->setFixedSize(28, 28);
    headRow->addWidget(close, 0, Qt::AlignVCenter);
    layout->addWidget(head);

    auto *scroll = new QScrollArea(panel);
    scroll->setObjectName(QStringLiteral("networkNodePanelScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("networkNodePanelBody"));
    body->setAutoFillBackground(false);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(16, 16, 16, 16);
    bodyLayout->setSpacing(18);

    auto *notice = nextPanelLabel(QStringLiteral("networkNodePanelNotice"), body);
    notice->setWordWrap(true);
    notice->hide();
    bodyLayout->addWidget(notice);

    auto *tiles = new QHBoxLayout;
    tiles->setContentsMargins(0, 0, 0, 0);
    tiles->setSpacing(8);
    const QList<QPair<QString, QString>> tileKeys{
        {QStringLiteral("latency"), QStringLiteral("Latency")},
        {QStringLiteral("synced"), QStringLiteral("Synced")},
        {QStringLiteral("version"), QStringLiteral("Version")}};
    for (const auto &[key, caption] : tileKeys) {
        auto *tile = new QFrame(body);
        tile->setObjectName(QStringLiteral("networkNodePanelTile"));
        tile->setProperty("tile", key);
        tile->setAttribute(Qt::WA_StyledBackground, true);
        auto *tileLayout = new QVBoxLayout(tile);
        tileLayout->setContentsMargins(6, 8, 6, 8);
        tileLayout->setSpacing(2);
        auto *tileKey = nextPanelLabel(QStringLiteral("networkNodePanelTileKey"), tile);
        tileKey->setText(caption);
        auto *tileValue = nextPanelLabel(QStringLiteral("networkNodePanelTileValue"), tile);
        for (QLabel *text : {tileKey, tileValue}) {
            text->setAlignment(Qt::AlignCenter);
            text->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            tileLayout->addWidget(text);
        }
        tiles->addWidget(tile, 1);
    }
    bodyLayout->addLayout(tiles);

    auto *meters = new QWidget(body);
    meters->setObjectName(QStringLiteral("networkNodePanelMeters"));
    auto *metersLayout = new QVBoxLayout(meters);
    metersLayout->setContentsMargins(0, 0, 0, 0);
    metersLayout->setSpacing(12);
    const QList<QPair<QString, QString>> meterKeys{
        {QStringLiteral("cpu"), QStringLiteral("CPU")},
        {QStringLiteral("ram"), QStringLiteral("Memory")},
        {QStringLiteral("disk"), QStringLiteral("Disk")}};
    const QHash<QString, QString> meterIcons{{QStringLiteral("cpu"), QStringLiteral("graph")},
                                             {QStringLiteral("ram"), QStringLiteral("server")},
                                             {QStringLiteral("disk"),
                                              QStringLiteral("file-directory")}};
    for (const auto &[key, caption] : meterKeys) {
        auto *meter = new QWidget(meters);
        meter->setObjectName(QStringLiteral("networkNodePanelMeter"));
        meter->setProperty("meter", key);
        auto *meterLayout = new QVBoxLayout(meter);
        meterLayout->setContentsMargins(0, 0, 0, 0);
        meterLayout->setSpacing(6);
        auto *line = new QHBoxLayout;
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        auto *glyph = new QLabel(meter);
        glyph->setPixmap(kit::tintedIcon(meterIcons.value(key),
                                         Theme::color(Theme::Role::FgMuted), 14,
                                         panel->devicePixelRatioF()));
        line->addWidget(glyph);
        auto *meterName = nextPanelLabel(QStringLiteral("networkNodePanelMeterName"), meter);
        meterName->setText(caption);
        line->addWidget(meterName, 1);
        line->addWidget(nextPanelLabel(QStringLiteral("networkNodePanelMeterValue"), meter));
        meterLayout->addLayout(line);
        auto *bar = new NextNodeMeter(meter);
        bar->setObjectName(QStringLiteral("networkNodePanelBar"));
        meterLayout->addWidget(bar);
        metersLayout->addWidget(meter);
    }
    bodyLayout->addWidget(meters);

    auto *heading = nextPanelLabel(QStringLiteral("networkNodePanelHeading"), body);
    heading->setText(QStringLiteral("DETAILS"));
    {
        QFont font = nextNodeFont(heading->font(), 11, false, QFont::Medium);
        font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
        heading->setFont(font);
    }
    auto *list = new QFrame(body);
    list->setObjectName(QStringLiteral("networkNodePanelList"));
    list->setAttribute(Qt::WA_StyledBackground, true);
    auto *listLayout = new QVBoxLayout(list);
    listLayout->setContentsMargins(6, 6, 6, 6);
    listLayout->setSpacing(0);
    auto *details = new QVBoxLayout;
    details->setContentsMargins(0, 0, 0, 0);
    details->setSpacing(8);
    details->addWidget(heading);
    details->addWidget(list);
    bodyLayout->addLayout(details);
    bodyLayout->addStretch(1);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);

    auto *footer = new QWidget(panel);
    footer->setObjectName(QStringLiteral("networkNodePanelFooter"));
    footer->setAttribute(Qt::WA_StyledBackground, true);
    auto *footerRow = new QHBoxLayout(footer);
    footerRow->setContentsMargins(12, 12, 12, 12);
    footerRow->setSpacing(6);
    auto *profile = new QPushButton(QStringLiteral("Profile"), footer);
    profile->setObjectName(QStringLiteral("networkNodePanelProfile"));
    profile->setToolTip(QStringLiteral("Open this node's profile"));
    profile->setIcon(QIcon(kit::tintedIcon(QStringLiteral("person"),
                                           Theme::color(Theme::Role::FgMuted), 14,
                                           panel->devicePixelRatioF())));
    auto *action = new QPushButton(footer);
    action->setObjectName(QStringLiteral("networkNodePanelAction"));
    for (QPushButton *button : {profile, action}) {
        button->setCursor(Qt::PointingHandCursor);
        button->setIconSize(QSize(14, 14));
        footerRow->addWidget(button, 1);
    }
    auto *more = new QToolButton(footer);
    more->setObjectName(QStringLiteral("networkNodePanelMore"));
    more->setAutoRaise(true);
    more->setCursor(Qt::PointingHandCursor);
    more->setToolTip(QStringLiteral("More actions"));
    more->setAccessibleName(QStringLiteral("More node actions"));
    more->setPopupMode(QToolButton::InstantPopup);
    more->setIcon(QIcon(kit::tintedIcon(QStringLiteral("kebab-horizontal"),
                                        Theme::color(Theme::Role::FgMuted), 16,
                                        panel->devicePixelRatioF())));
    more->setIconSize(QSize(16, 16));
    more->setFixedSize(28, 28);
    more->setMenu(new QMenu(more));
    footerRow->addWidget(more);
    layout->addWidget(footer);
    return panel;
}

void nextFillNodePanel(QTableWidget *table, QFrame *panel)
{
    const QString key = table->property("nextPanelKey").toString();
    const int row = nextNodeRowForKey(table, key);
    if (row < 0) {
        if (!key.isEmpty())
            table->setProperty("nextPanelKey", QString());
        panel->hide();
        table->viewport()->update();
        return;
    }
    panel->setProperty("nextPanelRow", row);
    const QTableWidgetItem *nameItem = table->item(row, kNodeColName);
    const NextNodeName split = nextSplitNodeName(nameItem ? nameItem->text() : QString());
    const QTableWidgetItem *roleItem = table->item(row, kNodeColRole);
    const QString roleKey = roleItem ? roleItem->data(Qt::UserRole).toString() : QString();
    const qreal ratio = panel->devicePixelRatioF();

    if (auto *icon = panel->findChild<QLabel *>(QStringLiteral("networkNodePanelIcon")))
        icon->setPixmap(kit::tintedIcon(nextNodeRoleIcon(roleKey),
                                        Theme::color(Theme::Role::FgMuted), 16, ratio));
    if (auto *name = panel->findChild<QLabel *>(QStringLiteral("networkNodePanelName"))) {
        name->setText(split.name);
        name->setToolTip(split.name);
    }
    if (auto *you = panel->findChild<QLabel *>(QStringLiteral("badge")))
        you->setVisible(split.self);
    QStringList subParts;
    const QString role = nextNodeCellText(table, row, kNodeColRole);
    if (!role.isEmpty())
        subParts.append(role);
    QString place = nextNodeCellText(table, row, kNodeColRegion);
    if (place.isEmpty())
        place = nextNodeCellText(table, row, kNodeColPlatform);
    if (!place.isEmpty())
        subParts.append(place);
    if (auto *sub = panel->findChild<QLabel *>(QStringLiteral("networkNodePanelSub")))
        sub->setText(subParts.join(QString::fromUtf8(" \xC2\xB7 ")));

    const QString status = nextNodeCellText(table, row, kNodeColStatus);
    if (auto *notice = panel->findChild<QLabel *>(QStringLiteral("networkNodePanelNotice"))) {
        const bool offline = !status.isEmpty() && !status.startsWith(QLatin1String("Online"));
        const QTableWidgetItem *statusItem = table->item(row, kNodeColStatus);
        const QString tip = statusItem ? statusItem->toolTip().trimmed() : QString();
        notice->setText(tip.isEmpty() ? status : status + QLatin1Char('\n') + tip);
        notice->setVisible(offline);
    }

    const QHash<QString, QString> tileValues{
        {QStringLiteral("latency"), nextNodeCellText(table, row, kNodeColLatency)},
        {QStringLiteral("synced"), nextNodeCellText(table, row, kNodeColSynced)},
        {QStringLiteral("version"), nextNodeCellText(table, row, kNodeColVersion)}};
    for (QFrame *tile : panel->findChildren<QFrame *>(QStringLiteral("networkNodePanelTile"))) {
        const QString value = tileValues.value(tile->property("tile").toString());
        if (auto *label = tile->findChild<QLabel *>(QStringLiteral("networkNodePanelTileValue"))) {
            label->setText(value.isEmpty() ? nextNodeDash() : value);
            label->setToolTip(value);
        }
    }

    const QHash<QString, int> meterColumns{{QStringLiteral("cpu"), kNodeColCpu},
                                           {QStringLiteral("ram"), kNodeColRam},
                                           {QStringLiteral("disk"), kNodeColDisk}};
    bool anyMeter = false;
    for (QWidget *meter : panel->findChildren<QWidget *>(QStringLiteral("networkNodePanelMeter"))) {
        const QTableWidgetItem *item =
            table->item(row, meterColumns.value(meter->property("meter").toString()));
        const QVariant data = item ? item->data(kProgressBarRole) : QVariant();
        const int value = data.isValid() ? data.toInt() : -1;
        anyMeter |= value >= 0;
        if (auto *label = meter->findChild<QLabel *>(QStringLiteral("networkNodePanelMeterValue")))
            label->setText(value >= 0 ? QStringLiteral("%1%").arg(value) : nextNodeDash());
        if (QWidget *bar = meter->findChild<QWidget *>(QStringLiteral("networkNodePanelBar")))
            static_cast<NextNodeMeter *>(bar)->setValue(value);
        meter->setToolTip(item ? item->toolTip() : QString());
    }
    if (auto *meters = panel->findChild<QWidget *>(QStringLiteral("networkNodePanelMeters")))
        meters->setVisible(anyMeter);

    if (auto *list = panel->findChild<QFrame *>(QStringLiteral("networkNodePanelList"))) {
        QLayout *listLayout = list->layout();
        while (QLayoutItem *old = listLayout->takeAt(0)) {
            if (QWidget *widget = old->widget())
                widget->deleteLater();
            delete old;
        }
        const QList<QPair<QString, int>> fields{
            {QStringLiteral("Status"), kNodeColStatus},
            {QStringLiteral("Activity"), kNodeColActivity},
            {QStringLiteral("Owner"), kNodeColOwner},
            {QStringLiteral("Platform"), kNodeColPlatform},
            {QStringLiteral("Revision"), kNodeColRevision},
            {QStringLiteral("Endpoint"), kNodeColEndpoint},
            {QStringLiteral("Health"), kNodeColHealth},
            {QStringLiteral("Node id"), kNodeColId}};
        int shown = 0;
        for (const auto &[caption, column] : fields) {
            const QString value = nextNodeCellText(table, row, column);
            if (value.isEmpty())
                continue;
            const QTableWidgetItem *item = table->item(row, column);
            auto *line = new QWidget(list);
            line->setObjectName(QStringLiteral("networkNodePanelRow"));
            auto *lineLayout = new QHBoxLayout(line);
            lineLayout->setContentsMargins(8, 0, 8, 0);
            lineLayout->setSpacing(12);
            line->setFixedHeight(32);
            auto *keyLabel = nextPanelLabel(QStringLiteral("networkNodePanelRowKey"), line);
            keyLabel->setText(caption);
            lineLayout->addWidget(keyLabel);
            auto *valueLabel = nextPanelLabel(QStringLiteral("networkNodePanelRowValue"), line);
            const bool mono = column == kNodeColRevision || column == kNodeColId ||
                              column == kNodeColEndpoint;
            valueLabel->setFont(nextNodeFont(valueLabel->font(), mono ? 12 : 13, mono,
                                             QFont::Normal));
            valueLabel->setText(QFontMetrics(valueLabel->font())
                                    .elidedText(value.simplified(), Qt::ElideRight, 180));
            valueLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            const QString tip = item ? item->toolTip().trimmed() : QString();
            valueLabel->setToolTip(tip.isEmpty() ? value : tip);
            lineLayout->addWidget(valueLabel, 1);
            listLayout->addWidget(line);
            ++shown;
        }
        list->setVisible(shown > 0);
        if (auto *heading = panel->findChild<QLabel *>(QStringLiteral("networkNodePanelHeading")))
            heading->setVisible(shown > 0);
    }

    if (auto *profile = panel->findChild<QPushButton *>(QStringLiteral("networkNodePanelProfile")))
        profile->setVisible(nameItem &&
                            !nameItem->data(kMirrorNodeIdRole).toString().isEmpty());
    const QTableWidgetItem *actionItem = table->item(row, kNodeColActions);
    if (auto *action = panel->findChild<QPushButton *>(QStringLiteral("networkNodePanelAction"))) {
        const bool linked = actionItem &&
                            !actionItem->data(Qt::UserRole + 3).toString().isEmpty() &&
                            !actionItem->text().isEmpty();
        action->setVisible(linked);
        if (linked) {
            action->setText(actionItem->text());
            action->setToolTip(actionItem->toolTip());
            const bool relay =
                actionItem->data(Qt::UserRole + 3).toString() == QLatin1String("relay");
            action->setIcon(QIcon(kit::tintedIcon(relay ? QStringLiteral("link")
                                                        : QStringLiteral("terminal"),
                                                  Theme::color(Theme::Role::FgMuted), 14,
                                                  ratio)));
        }
    }
    if (auto *more = panel->findChild<QToolButton *>(QStringLiteral("networkNodePanelMore"))) {
        auto *button = qobject_cast<QPushButton *>(table->cellWidget(row, kNodeColActions));
        more->setVisible(button && !button->isHidden());
    }
    panel->show();
    table->viewport()->update();
}
} // namespace

QWidget *MainWindow::buildNodesSection()
{
    const bool next = Theme::isNext();
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, next ? 16 : 4, 0, 0);
    outer->setSpacing(next ? 16 : 6);

    auto *subtitle = new QLabel(QString::fromUtf8(
        "Every node on the network with its role: core servers, runners, "
        "mirror servers, desktop nodes and the relays this app talks to. It "
        "lists every registered node from the relay directory, including "
        "offline ones, every fleet or registered runner and every configured "
        "relay, probed live for its status, response time and version. Each "
        "node's row also shows its copy of the repository chosen under "
        "Repository mirrors, compared with that repository's source of truth; "
        "a node holding several copies lists each one on its own line beneath "
        "it. An underlined value doesn't match the source of truth \xE2\x80\x94 "
        "that node is serving different data. An \xE2\x86\x91 column is "
        "populated only when a copy has unique work still waiting to reconcile "
        "back into the source of truth. Size is canonical content; Git DB is "
        "the physical object database on that host. Route shows its latest "
        "direct-HTTPS proof. Click a column header to sort; double-click a "
        "relay to open its website, a runner to read its logs, or a Health "
        "result (or select it and press Enter) to draft a troubleshooting "
        "prompt. \"Online (serving)\" means the relay reports it live even "
        "when it is not connected to this client's chat room. Switch the "
        "active relay from the relay dropdown in the top bar; ForkMesh never "
        "silently fails over to another relay."));
    subtitle->setObjectName("mutedLabel");
    subtitle->setWordWrap(true);
    outer->addWidget(subtitle);
    if (next)
        subtitle->hide();

    QHBoxLayout *controls = next ? nullptr : new QHBoxLayout;
    if (controls)
        controls->setContentsMargins(0, 0, 0, 0);
    m_nodesStatus = new QLabel;
    m_nodesStatus->setObjectName("mutedLabel");
    if (controls)
        controls->addWidget(m_nodesStatus, 1);
    auto *roleLabel = new QLabel(QStringLiteral("Role"));
    roleLabel->setObjectName("mutedLabel");
    if (controls)
        controls->addWidget(roleLabel);
    m_nodesRoleFilter = new QComboBox;
    m_nodesRoleFilter->setObjectName(QStringLiteral("nodesRoleFilter"));
    m_nodesRoleFilter->setToolTip(QStringLiteral(
        "Show only nodes of one role, in both tables"));
    m_nodesRoleFilter->addItem(QStringLiteral("All roles"), QString());
    for (forkmesh::noderole::Role role : forkmesh::noderole::allRoles())
        m_nodesRoleFilter->addItem(forkmesh::noderole::label(role),
                                   forkmesh::noderole::key(role));
    {
        const int saved = m_nodesRoleFilter->findData(
            QSettings().value(QStringLiteral("network/nodesRoleFilter")).toString());
        m_nodesRoleFilter->setCurrentIndex(qMax(0, saved));
    }
    connect(m_nodesRoleFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                QSettings().setValue(QStringLiteral("network/nodesRoleFilter"),
                                     m_nodesRoleFilter->currentData().toString());
                refreshNodesTable();
                applyNodeRoleFilter();
            });
    if (controls)
        controls->addWidget(m_nodesRoleFilter);
    auto *updateAll =
        new QPushButton(QStringLiteral("Update all from this binary"));
    updateAll->setObjectName(QStringLiteral("nodesUpdateAllBinaryButton"));
    updateAll->setCursor(Qt::PointingHandCursor);
    updateAll->setToolTip(QStringLiteral(
        "SSH to every saved host in parallel, install the current published "
        "ForkMesh binary, and show live restart progress for each node."));
    setOcticon(updateAll, "download", 14);
    connect(updateAll, &QPushButton::clicked, this, [this] {
        showNetworkTab(kNetworkHostsTab);
        QTimer::singleShot(0, this,
                           &MainWindow::runHostInstallAllFromBinary);
    });
    QWidget *mirrorSection = nullptr;
    if (controls) {
        controls->addWidget(updateAll);
        outer->addLayout(controls);
        outer->addWidget(buildMirrorNodesSection());
    } else {
        mirrorSection = buildMirrorNodesSection();
    }

    m_nodesTable = new QTableWidget(0, kNodeColCount);
    installColumnHeaderMenu(m_nodesTable, true);
    m_nodesTable->setObjectName("issueTable");
    m_nodesTable->setProperty("nodesDirectory", true);
    // Icon + caption headers, the same shape the Mirror nodes table uses: the
    // glyph carries the meaning at a glance and the tooltip spells out what the
    // column actually reports (many of these are per-repository facts from the
    // node's freshest signed catalog record).
    struct NodeHeaderIcon {
        int column;
        const char *icon;
        const char *text;  // caption: terse, the icon carries the rest
        const char *label; // tooltip: the full meaning of the column
    };
    static constexpr NodeHeaderIcon kNodeHeaderIcons[] = {
        {kNodeColName, "device-desktop", "Node", "Node"},
        {kNodeColRole, "server", "Role",
         "Role: core, runner, mirror, desktop node or relay"},
        {kNodeColScope, "people", "Scope",
         "Runner scope (pool or personal), or whether a relay is the active "
         "instance"},
        {kNodeColStatus, "broadcast", "Status", "Status"},
        {kNodeColActivity, "workflow", "Activity",
         "What this node is doing right now"},
        {kNodeColOwner, "person", "Owner", "Account that owns this node"},
        {kNodeColVersion, "tag", "Version", "ForkMesh version"},
        {kNodeColRevision, "git-commit", "Revision",
         "Commit this node is serving, and its branch"},
        {kNodeColMessage, "note", "Message", "Message"},
        {kNodeColAuthor, "pencil", "Author", "Author"},
        {kNodeColSynced, "history", "Synced", "Last mirror sync"},
        {kNodeColEndpoint, "link", "Endpoint",
         "Direct HTTPS endpoint the relay routes clones to"},
        {kNodeColChecked, "check-circle", "Checked",
         "When that endpoint was last health checked"},
        {kNodeColLatency, "rocket", "Latency",
         "Endpoint response time at the last health check"},
        {kNodeColRegion, "cloud", "Region", "Region the endpoint answered from"},
        {kNodeColIntegrity, "lock", "Integrity", "Endpoint integrity"},
        {kNodeColRoute, "link-external", "Route", "Direct HTTPS route"},
        {kNodeColReachability, "link", "Reach", "Reachability"},
        {kNodeColCaps, "list-unordered", "Caps",
         "Capabilities this node advertises"},
        {kNodeColPlatform, "terminal", "Platform", "Operating system"},
        {kNodeColRepos, "repo", "Repos", "Repositories owned by this node"},
        {kNodeColMirrors, "repo-forked", "Mirrors",
         "Repositories this node mirrors"},
        {kNodeColSize, "pie-chart", "Size", "Repository size"},
        {kNodeColGitDatabase, "database", "Git DB",
         "Physical Git object database"},
        {kNodeColIssues, "issue-opened", "Issues", "Issues"},
        {kNodeColIssuesToSource, "upload", "Issues \xE2\x86\x91",
         "Issues waiting to sync back to the source of truth"},
        {kNodeColCommits, "git-commit", "Commits", "Commits"},
        {kNodeColCommitsToSource, "upload", "Commits \xE2\x86\x91",
         "Commits waiting to sync back to the source of truth"},
        {kNodeColBranches, "git-branch", "Branches", "Branches"},
        {kNodeColBranchesToSource, "upload", "Branches \xE2\x86\x91",
         "Branches waiting to sync back to the source of truth"},
        {kNodeColPulls, "git-pull-request", "Pulls", "Pull requests"},
        {kNodeColPullsToSource, "upload", "Pulls \xE2\x86\x91",
         "Pull requests waiting to sync back to the source of truth"},
        {kNodeColDiscussions, "comment", "Discuss", "Discussions"},
        {kNodeColDiscussionsToSource, "upload", "Discuss \xE2\x86\x91",
         "Discussions waiting to sync back to the source of truth"},
        {kNodeColArtifacts, "package", "Artifacts", "Artifacts"},
        {kNodeColWorktrees, "copy", "Worktrees", "Worktrees"},
        {kNodeColClones, "download", "Clones", "Clones served"},
        {kNodeColWebsite, "eye", "Web hits", "Website requests served"},
        {kNodeColHealth, "shield-check", "Health", "Reported diagnostics"},
        {kNodeColCpu, "graph", "CPU", "CPU"},
        {kNodeColRam, "server", "RAM", "RAM"},
        {kNodeColDisk, "file-directory", "Disk", "Disk"},
        {kNodeColId, "key", "Node id", "Node id (Ed25519 public key)"},
        {kNodeColActions, "gear", "Actions", "Actions"},
    };
    static_assert(sizeof(kNodeHeaderIcons) / sizeof(kNodeHeaderIcons[0]) ==
                      kNodeColCount,
                  "every node directory column needs a header icon");
    for (const NodeHeaderIcon &h : kNodeHeaderIcons) {
        auto *headerItem = new QTableWidgetItem;
        if (!next)
            headerItem->setIcon(themedOcticon(QLatin1String(h.icon),
                                              Theme::remapColor(QColor("#8b949e")), 14));
        headerItem->setText(QString::fromUtf8(h.text));
        headerItem->setToolTip(QString::fromUtf8(h.label));
        if (h.column == kNodeColRole)
            headerItem->setData(Qt::UserRole, QStringLiteral("role"));
        m_nodesTable->setHorizontalHeaderItem(h.column, headerItem);
    }
    // Captions ride alongside the icons across 32 columns; the theme's global
    // 14px would push the header far past the width of the panel. An inline
    // stylesheet is the only thing that beats the theme's QSS font size.
    m_nodesTable->horizontalHeader()->setStyleSheet(
        next ? QStringLiteral("QHeaderView::section { font-size: 12px; font-weight: 400;"
                              " padding: 0px 10px; min-height: 34px; }")
             : Theme::remapSheet(
                   "QHeaderView::section { font-size: 10px; padding: 2px 3px; }"));
    m_nodesTable->horizontalHeader()->setMinimumSectionSize(22);
    m_nodesTable->verticalHeader()->setDefaultSectionSize(next ? 44 : 26);
    m_nodesTable->setWordWrap(false);
    m_nodesTable->verticalHeader()->setVisible(false);
    m_nodesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nodesTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_nodesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_nodesTable->setShowGrid(false);
    m_nodesTable->setSortingEnabled(false);
    m_nodesTable->horizontalHeader()->setSectionsClickable(true);
    m_nodesTable->horizontalHeader()->setSortIndicatorShown(true);
    m_nodesTable->horizontalHeader()->setSectionResizeMode(kNodeColName,
                                                           QHeaderView::Stretch);
    for (int c = kNodeColName + 1; c < kNodeColCount; ++c)
        m_nodesTable->horizontalHeader()->setSectionResizeMode(
            c, QHeaderView::ResizeToContents);
    // Actions holds a real Delete button, and nothing that sizes columns here
    // measures cell widgets: ResizeToContents and makeColumnsResizable() both
    // size a column from its items, and the items in this one are empty, so the
    // column collapses and every button is clipped to a red sliver. Keep it
    // interactive so those passes leave it alone -- refreshNodeActionButtons()
    // widens it to the button, the way the hosts table sizes its own actions.
    m_nodesTable->horizontalHeader()->setSectionResizeMode(
        kNodeColActions, QHeaderView::Interactive);
    makeColumnsResizable(m_nodesTable); // spreadsheet-style draggable columns (#263)
    auto *resourceBars = new ResourceBarDelegate(m_nodesTable);
    resourceBars->mutedSelectionBand = true;
    for (int col : {kNodeColCpu, kNodeColRam, kNodeColDisk})
        m_nodesTable->setItemDelegateForColumn(col, resourceBars);
    m_nodesTable->setItemDelegateForColumn(
        kNodeColSynced, new MirrorSyncDelegate(m_nodesTable));
    if (next) {
        m_nodesTable->setItemDelegateForColumn(
            kNodeColName,
            new NextNodeCellDelegate(NextNodeCellDelegate::Name, m_nodesTable));
        auto *muted = new NextNodeCellDelegate(NextNodeCellDelegate::Muted, m_nodesTable);
        for (int col : {kNodeColRole, kNodeColScope, kNodeColStatus, kNodeColActivity,
                        kNodeColOwner, kNodeColLatency, kNodeColRegion})
            m_nodesTable->setItemDelegateForColumn(col, muted);
        auto *mono = new NextNodeCellDelegate(NextNodeCellDelegate::Mono, m_nodesTable);
        for (int col : {kNodeColVersion, kNodeColRevision})
            m_nodesTable->setItemDelegateForColumn(col, mono);
        new NextNodesTableChrome(m_nodesTable);
        QHeaderView *header = m_nodesTable->horizontalHeader();
        header->moveSection(header->visualIndex(kNodeColLatency),
                            header->visualIndex(kNodeColStatus) + 1);
    }
    auto *pacmanTick = new QTimer(m_nodesTable);
    pacmanTick->setInterval(1000);
    connect(pacmanTick, &QTimer::timeout, m_nodesTable, [this] {
        if (!m_nodesTable->isVisible())
            return;
        for (int r = 0; r < m_nodesTable->rowCount(); ++r) {
            const QTableWidgetItem *synced = m_nodesTable->item(r, kNodeColSynced);
            if (synced && synced->data(kPacmanAnchorRole).isValid())
                m_nodesTable->update(
                    m_nodesTable->model()->index(r, kNodeColSynced));
        }
    });
    pacmanTick->start();
    connect(m_nodesTable, &QTableWidget::itemActivated, this,
            [this](QTableWidgetItem *item) {
                if (!item)
                    return;
                if (item->column() == kNodeColHealth &&
                    !item->data(kMirrorDiagnosticPromptRole).toString().isEmpty()) {
                    draftMirrorNodeDiagnosticsPrompt(item);
                    return;
                }
                const QTableWidgetItem *action =
                    m_nodesTable->item(item->row(), kNodeColActions);
                if (action && !action->data(Qt::UserRole + 3).toString().isEmpty())
                    return;
                const QTableWidgetItem *name =
                    m_nodesTable->item(item->row(), kNodeColName);
                const QString nodeId =
                    name ? name->data(kMirrorNodeIdRole).toString() : QString();
                if (!nodeId.isEmpty())
                    showNodeProfile(nodeId, name->data(Qt::UserRole).toString());
            });
    connect(m_nodesTable, &QTableWidget::cellClicked, this,
            [this](int row, int column) {
                if (column == kNodeColActions)
                    activateNodeDirectoryRow(row);
            });
    connect(m_nodesTable, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { activateNodeDirectoryRow(row); });
    connect(m_nodesTable->horizontalHeader(),
            &QHeaderView::sortIndicatorChanged, this, &MainWindow::sortNodesTable);
    if (next) {
        auto *body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(16);
        auto *column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(16);

        auto *stats = new QHBoxLayout;
        stats->setContentsMargins(0, 0, 0, 0);
        stats->setSpacing(12);
        const QList<QPair<QString, QString>> tiles{
            {QStringLiteral("networkStatOnline"), QStringLiteral("Nodes online")},
            {QStringLiteral("networkStatLatency"), QStringLiteral("Median latency")},
            {QStringLiteral("networkStatTraffic"), QStringLiteral("Relay traffic")},
            {QStringLiteral("networkStatRepos"), QStringLiteral("Repositories served")}};
        for (const auto &[name, caption] : tiles)
            stats->addWidget(nextStatTile(name, caption, page), 1);
        column->addLayout(stats);

        updateAll->setProperty("networkRole", QStringLiteral("outline"));
        column->addWidget(mirrorSection);
        if (auto *mirrorRow = mirrorSection->findChild<QHBoxLayout *>(
                QStringLiteral("mirrorNodesHeaderRow")))
            mirrorRow->addWidget(updateAll);
        else
            column->addWidget(updateAll, 0, Qt::AlignRight);

        auto *toolbar = new QFrame(page);
        toolbar->setObjectName(QStringLiteral("networkNodesToolbar"));
        toolbar->setAttribute(Qt::WA_StyledBackground, true);
        auto *toolbarRow = new QHBoxLayout(toolbar);
        toolbarRow->setContentsMargins(8, 6, 8, 6);
        toolbarRow->setSpacing(8);
        const QHash<QString, QString> segmentLabels{
            {QString(), QStringLiteral("All")},
            {QStringLiteral("core"), QStringLiteral("Core")},
            {QStringLiteral("runner"), QStringLiteral("Runners")},
            {QStringLiteral("mirror"), QStringLiteral("Mirrors")},
            {QStringLiteral("desktop"), QStringLiteral("Desktop nodes")},
            {QStringLiteral("relay"), QStringLiteral("Relays")}};
        QStringList labels;
        for (int i = 0; i < m_nodesRoleFilter->count(); ++i) {
            const QString key = m_nodesRoleFilter->itemData(i).toString();
            labels.append(segmentLabels.value(key, m_nodesRoleFilter->itemText(i)));
        }
        auto *segments = new kit::SegmentedControl(labels, toolbar);
        segments->setToolTip(m_nodesRoleFilter->toolTip());
        segments->setCurrentIndex(m_nodesRoleFilter->currentIndex());
        segments->onChanged([this](int index) {
            if (m_nodesRoleFilter && index >= 0 &&
                index != m_nodesRoleFilter->currentIndex())
                m_nodesRoleFilter->setCurrentIndex(index);
        });
        connect(m_nodesRoleFilter, QOverload<int>::of(&QComboBox::currentIndexChanged),
                segments, [segments](int index) {
                    if (segments->currentIndex() != index)
                        segments->setCurrentIndex(index);
                });
        toolbarRow->addWidget(segments);
        toolbarRow->addWidget(roleLabel);
        toolbarRow->addWidget(m_nodesRoleFilter);
        roleLabel->hide();
        m_nodesRoleFilter->hide();
        toolbarRow->addStretch(1);
        auto *filter = new QLineEdit(toolbar);
        filter->setObjectName(QStringLiteral("networkNodesFilter"));
        filter->setPlaceholderText(QStringLiteral("Filter nodes"));
        filter->setToolTip(QStringLiteral(
            "Show only nodes whose name, role, owner, status, region, platform or "
            "endpoint contains this text"));
        filter->setClearButtonEnabled(true);
        filter->setFixedWidth(224);
        filter->addAction(QIcon(kit::tintedIcon(QStringLiteral("search"),
                                                Theme::color(Theme::Role::FgSubtle), 14,
                                                page->devicePixelRatioF())),
                          QLineEdit::LeadingPosition);
        connect(filter, &QLineEdit::textChanged, this, [this](const QString &text) {
            m_nodesTable->setProperty("nextNodeFilter", text.trimmed());
            applyNodeRoleFilter();
            refreshNodesStatusLine();
            m_nodesTable->viewport()->update();
        });
        toolbarRow->addWidget(filter);
        toolbarRow->addWidget(
            makeInlineHelpButton(QStringLiteral("About nodes"), subtitle->text()));

        auto *tableStack = new QVBoxLayout;
        tableStack->setContentsMargins(0, 0, 0, 0);
        tableStack->setSpacing(0);
        tableStack->addWidget(toolbar);
        tableStack->addWidget(m_nodesTable, 1);
        column->addLayout(tableStack, 1);
        m_nodesStatus->setProperty("networkRole", QStringLiteral("footnote"));
        column->addWidget(m_nodesStatus);
        body->addLayout(column, 1);

        QFrame *panel = nextBuildNodePanel(page);
        body->addWidget(panel);
        outer->addLayout(body, 1);

        connect(m_nodesTable, &QTableWidget::cellClicked, panel,
                [this, panel](int row, int) {
                    m_nodesTable->setProperty("nextPanelKey",
                                              nextNodeRowKey(m_nodesTable, row));
                    nextFillNodePanel(m_nodesTable, panel);
                });
        if (auto *close = panel->findChild<QToolButton *>(
                QStringLiteral("networkNodePanelClose")))
            connect(close, &QToolButton::clicked, panel, [this, panel] {
                m_nodesTable->setProperty("nextPanelKey", QString());
                nextFillNodePanel(m_nodesTable, panel);
            });
        if (auto *profile = panel->findChild<QPushButton *>(
                QStringLiteral("networkNodePanelProfile")))
            connect(profile, &QPushButton::clicked, panel, [this, panel] {
                const QTableWidgetItem *name = m_nodesTable->item(
                    panel->property("nextPanelRow").toInt(), kNodeColName);
                const QString nodeId =
                    name ? name->data(kMirrorNodeIdRole).toString() : QString();
                if (!nodeId.isEmpty())
                    showNodeProfile(nodeId, name->data(Qt::UserRole).toString());
            });
        if (auto *action = panel->findChild<QPushButton *>(
                QStringLiteral("networkNodePanelAction")))
            connect(action, &QPushButton::clicked, panel, [this, panel] {
                activateNodeDirectoryRow(panel->property("nextPanelRow").toInt());
            });
        if (auto *more = panel->findChild<QToolButton *>(
                QStringLiteral("networkNodePanelMore"))) {
            QMenu *menu = more->menu();
            menu->setToolTipsVisible(true);
            connect(menu, &QMenu::aboutToShow, panel, [this, panel, menu] {
                menu->clear();
                auto *button = qobject_cast<QPushButton *>(m_nodesTable->cellWidget(
                    panel->property("nextPanelRow").toInt(), kNodeColActions));
                if (!button)
                    return;
                QAction *remove = menu->addAction(button->icon(), button->text());
                remove->setToolTip(button->toolTip());
                remove->setEnabled(button->isEnabled());
                connect(remove, &QAction::triggered, button, &QPushButton::click);
            });
        }
    } else {
        outer->addWidget(m_nodesTable, 1);
    }

    refreshChatUserDirectory();
    fetchRelayOnlineNodes();
    fetchNodesCatalogInfo();
    refreshRelayNodes();
    refreshNodesTable();
    return page;
}

void MainWindow::fetchRelayOnlineNodes(bool force)
{
    if (!m_networkAccess)
        return;
    const QString backoffKey = QStringLiteral("relay-online-nodes");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 minIntervalMs = force ? 15000 : 60000;
    if (m_relayOnlineNodesFetchedMs > 0 &&
        now - m_relayOnlineNodesFetchedMs < minIntervalMs)
        return;
    if (!m_pollBackoff.ready(backoffKey, now))
        return;
    m_relayOnlineNodesFetchedMs = now;

    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/network/stats"));
    url.setQuery(QString());
    QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, backoffKey] {
        const QByteArray body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const QJsonObject resp = QJsonDocument::fromJson(body).object();
        if (!ok || !resp.value("ok").toBool()) {
            m_pollBackoff.noteFailure(backoffKey,
                                      QDateTime::currentMSecsSinceEpoch());
            return;
        }
        m_pollBackoff.noteSuccess(backoffKey);
        QSet<QString> online;
        for (const QJsonValue &v : resp.value("onlineNodes").toArray()) {
            const QString name = v.toString().trimmed().toLower();
            if (!name.isEmpty())
                online.insert(name);
        }
        const bool firstReply = !m_relayOnlineNodesFetched;
        m_relayOnlineNodesFetched = true;
        if (!firstReply && online == m_relayOnlineNodes)
            return;
        m_relayOnlineNodes = online;
        refreshNodesTable();
        if (mirrorNodesOnScreen())
            loadMirrorNodesPanel();
    });
}

void MainWindow::fetchNodesCatalogInfo(bool force)
{
    if (!m_networkAccess)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 minIntervalMs = force ? 15000 : 5 * 60000;
    if (m_nodesCatalogFetchedMs > 0 &&
        now - m_nodesCatalogFetchedMs < minIntervalMs)
        return;
    m_nodesCatalogFetchedMs = now;

    constexpr int kMaxSources = 12;
    QStringList sources;
    for (bool previews : {false, true}) {
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (repo.isPrivate || repo.previewOnly != previews)
                continue;
            const QString owner = repo.owner.trimmed();
            const QString name = repo.name.trimmed();
            if (owner.isEmpty() || name.isEmpty())
                continue;
            const QString source = owner + QLatin1Char('/') + name;
            if (!sources.contains(source, Qt::CaseInsensitive))
                sources.append(source);
            if (sources.size() >= kMaxSources)
                break;
        }
        if (sources.size() >= kMaxSources)
            break;
    }
    if (sources.isEmpty())
        return;

    struct Sweep {
        int pending = 0;
        QHash<QString, QJsonObject> nodes;
        // Only the repositories that actually answered this round. One skipped
        // by the poll backoff, or one that answered ok:false, proves nothing
        // about the nodes it used to list, so its records survive the merge.
        QSet<QString> covered;
    };
    auto sweep = std::make_shared<Sweep>();
    for (const QString &source : std::as_const(sources)) {
        const int slash = source.indexOf(QLatin1Char('/'));
        QUrl url = catalogApiUrl();
        url.setPath(QStringLiteral("/api/repo/%1/%2/mirrors")
                        .arg(QString::fromUtf8(QUrl::toPercentEncoding(
                                 source.left(slash))),
                             QString::fromUtf8(QUrl::toPercentEncoding(
                                 source.mid(slash + 1)))));
        const QString backoffKey =
            QStringLiteral("nodes-catalog|") + url.toString();
        if (!m_pollBackoff.ready(backoffKey, now))
            continue;
        ++sweep->pending;
        QNetworkReply *reply = m_networkAccess->get(QNetworkRequest(url));
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, sweep, source, backoffKey] {
            const QByteArray body = reply->readAll();
            const bool expectedDegraded =
                forkmesh::networkReplyIsExpectedDegraded(reply);
            reply->deleteLater();
            const QJsonObject resp = QJsonDocument::fromJson(body).object();
            if (!resp.value("ok").toBool()) {
                // A fail-closed public-snapshot barrier (a node deletion, a
                // repo going private) answers 503 by design for a couple of
                // minutes. Escalating the exponential backoff over it would
                // leave these columns stale for up to ten -- exactly while
                // someone is watching this table to see the node go. The
                // sweep's own 5-minute interval already keeps the retry
                // polite, so leave the channel's state untouched.
                if (!expectedDegraded)
                    m_pollBackoff.noteFailure(
                        backoffKey, QDateTime::currentMSecsSinceEpoch());
            } else {
                m_pollBackoff.noteSuccess(backoffKey);
                sweep->covered.insert(source);
                for (const QJsonValue &value :
                     resp.value("mirrors").toArray()) {
                    const QJsonObject record = value.toObject();
                    const QString key = record.value("node")
                                            .toString()
                                            .trimmed()
                                            .toLower();
                    if (key.isEmpty())
                        continue;
                    QJsonObject &known = sweep->nodes[key];
                    QJsonArray mirrorSources =
                        known.value("mirrorSources").toArray();
                    if (!mirrorSources.contains(source))
                        mirrorSources.append(source);
                    if (known.isEmpty() ||
                        record.value("lastSync").toDouble() >=
                            known.value("lastSync").toDouble())
                        known = record;
                    known.insert("mirrorSources", mirrorSources);
                }
            }
            if (--sweep->pending > 0)
                return;
            m_nodesCatalogInfo = mergeNodesCatalogSweep(
                m_nodesCatalogInfo, sweep->nodes, sweep->covered);
            {
                QJsonArray answered;
                for (const QJsonObject &record : std::as_const(sweep->nodes))
                    answered.append(record);
                promoteSavedHostsProvenHealthy(
                    answered, QStringLiteral("the public mirror catalog"));
            }
            refreshNodesTable();
        });
    }
}

void MainWindow::applyCatalogNodeInfo(MemberInfo &info,
                                      const QString &node) const
{
    const QJsonObject record =
        m_nodesCatalogInfo.value(node.trimmed().toLower());
    if (record.isEmpty())
        return;
    if (info.ownerUser.trimmed().isEmpty())
        info.ownerUser = record.value("ownerUser").toString().trimmed();
    if (info.version.trimmed().isEmpty())
        info.version = record.value("version").toString().trimmed();
    if (info.platform.trimmed().isEmpty())
        info.platform = record.value("platform").toString().trimmed();
    if (info.id.trimmed().isEmpty())
        info.id = record.value("id").toString().trimmed();
    if (info.cpuPercent < 0.0)
        info.cpuPercent = record.value("cpuPercent").toDouble(-1.0);
    if (info.memTotalBytes <= 0) {
        info.memUsedBytes = qint64(record.value("memUsedBytes").toDouble());
        info.memTotalBytes = qint64(record.value("memTotalBytes").toDouble());
    }
    if (info.diskTotalBytes <= 0) {
        info.diskUsedBytes = qint64(record.value("diskUsedBytes").toDouble());
        info.diskTotalBytes =
            qint64(record.value("diskTotalBytes").toDouble());
    }
    if (info.mirrors.isEmpty()) {
        for (const QJsonValue &value :
             record.value("mirrorSources").toArray()) {
            const QString source = value.toString().trimmed();
            if (source.isEmpty())
                continue;
            info.mirrors.append(source);
            MirrorAdvert advert;
            advert.source = source;
            info.mirrorDetails.append(advert);
        }
    }
}

void MainWindow::refreshNodesTable()
{

    if (m_nodesTable && m_nodesTable->isVisible()) {
        fetchRelayOnlineNodes();
        fetchNodesCatalogInfo();
    }

    QList<NodeMenuEntry> visible;
    QList<MemberInfo> visibleRoster;
    collectVisibleMeshNodes(visible, visibleRoster);
    const QSet<QString> savedHosts = savedHostNodeNames();

    QSet<QString> meshNames;
    for (const NodeMenuEntry &entry : std::as_const(visible))
        meshNames.insert(entry.name.trimmed().toLower());
    QList<forkmesh::nodedir::RunnerRow> runners;
    QList<int> meshRows;
    {
        const QList<forkmesh::nodedir::RunnerRow> merged = directoryRunnerRows();
        QSet<QString> runnerKeys;
        for (const forkmesh::nodedir::RunnerRow &runner : merged) {
            const QString key = runner.name.trimmed().toLower();
            if (meshNames.contains(key))
                continue;
            runners.append(runner);
            runnerKeys.insert(forkmesh::nodedir::runnerKey(key));
        }
        for (int i = 0; i < visible.size(); ++i) {
            if (!runnerKeys.contains(
                    forkmesh::nodedir::runnerKey(visible.at(i).name)))
                meshRows.append(i);
        }
    }

    updateNetworkCounts(-1, meshRows.size() + runners.size(), -1);
    m_nodeDotEntries = visible;
    refreshNodeDotMatrix();
    if (!m_nodesTable)
        return;
    if (Theme::isNext()) {
        QSet<QString> served;
        int servingNodes = 0;
        for (const MemberInfo &info : std::as_const(visibleRoster)) {
            if (info.mirrors.isEmpty())
                continue;
            ++servingNodes;
            for (const QString &repo : info.mirrors)
                served.insert(repo.trimmed().toLower());
        }
        m_nodesTable->setProperty("nextReposServed", int(served.size()));
        m_nodesTable->setProperty("nextReposNodes", servingNodes);
    }
    if (!m_nodesTable->isVisible()) {
        m_nodesTableStale = true;
        return;
    }
    m_nodesTableStale = false;

    m_nodesTable->clearContents();
    m_nodesTable->setRowCount(meshRows.size() + m_servers.size() + runners.size());
    const qint64 tableNowMs = QDateTime::currentMSecsSinceEpoch();
    bool sawCatalogRecord = false;
    int row = 0;
    for (int i : std::as_const(meshRows)) {
        fillNodesTableRow(row, visible.at(i), savedHosts, visibleRoster.at(i),
                          tableNowMs, sawCatalogRecord);
        if (QTableWidgetItem *name = m_nodesTable->item(row, kNodeColName))
            name->setData(Qt::UserRole + 5, QStringLiteral("node"));
        ++row;
    }
    for (int i = 0; i < m_servers.size(); ++i)
        fillRelayNodeRow(row++, i);
    for (const forkmesh::nodedir::RunnerRow &runner : std::as_const(runners))
        fillRunnerNodeRow(row++, runner, tableNowMs);
    mergeMirrorRowsIntoNodesTable();
    if (sawCatalogRecord &&
        !m_nodesTable->property("catalogColumnsFitted").toBool()) {
        m_nodesTable->setProperty("catalogColumnsFitted", true);
        m_nodesTable->resizeColumnsToContents();
    } else {
        growColumnsToContents(m_nodesTable);
    }
    sortNodesTable();
    refreshNodesStatusLine();
}

bool MainWindow::mirrorNodesOnScreen() const
{
    return m_nodesTable && m_nodesTable->isVisible();
}

void MainWindow::mergeMirrorRowsIntoNodesTable()
{
    if (!m_nodesTable)
        return;
    enum class Take { Prefer, Fill };
    struct ColumnMap {
        int mirror;
        int node;
        Take take;
    };
    static constexpr ColumnMap kColumns[] = {
        {MirrorNodeColRole, kNodeColRole, Take::Fill},
        {MirrorNodeColOwner, kNodeColOwner, Take::Fill},
        {MirrorNodeColVersion, kNodeColVersion, Take::Fill},
        {MirrorNodeColCommit, kNodeColRevision, Take::Prefer},
        {MirrorNodeColMessage, kNodeColMessage, Take::Prefer},
        {MirrorNodeColAuthor, kNodeColAuthor, Take::Prefer},
        {MirrorNodeColSynced, kNodeColSynced, Take::Prefer},
        {MirrorNodeColRoute, kNodeColRoute, Take::Prefer},
        {MirrorNodeColReachability, kNodeColReachability, Take::Prefer},
        {MirrorNodeColPlatform, kNodeColPlatform, Take::Fill},
        {MirrorNodeColRepos, kNodeColMirrors, Take::Prefer},
        {MirrorNodeColSize, kNodeColSize, Take::Prefer},
        {MirrorNodeColGitDatabase, kNodeColGitDatabase, Take::Prefer},
        {MirrorNodeColIssues, kNodeColIssues, Take::Prefer},
        {MirrorNodeColIssuesToSource, kNodeColIssuesToSource, Take::Prefer},
        {MirrorNodeColCommits, kNodeColCommits, Take::Prefer},
        {MirrorNodeColCommitsToSource, kNodeColCommitsToSource, Take::Prefer},
        {MirrorNodeColBranches, kNodeColBranches, Take::Prefer},
        {MirrorNodeColBranchesToSource, kNodeColBranchesToSource, Take::Prefer},
        {MirrorNodeColPulls, kNodeColPulls, Take::Prefer},
        {MirrorNodeColPullsToSource, kNodeColPullsToSource, Take::Prefer},
        {MirrorNodeColDiscussions, kNodeColDiscussions, Take::Prefer},
        {MirrorNodeColDiscussionsToSource, kNodeColDiscussionsToSource,
         Take::Prefer},
        {MirrorNodeColArtifacts, kNodeColArtifacts, Take::Prefer},
        {MirrorNodeColClones, kNodeColClones, Take::Prefer},
        {MirrorNodeColWebsite, kNodeColWebsite, Take::Prefer},
        {MirrorNodeColHealth, kNodeColHealth, Take::Prefer},
        {MirrorNodeColCpu, kNodeColCpu, Take::Fill},
        {MirrorNodeColRam, kNodeColRam, Take::Fill},
        {MirrorNodeColDisk, kNodeColDisk, Take::Fill},
        {MirrorNodeColId, kNodeColId, Take::Fill},
    };
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const auto blank = [&dash](const QTableWidgetItem *item) {
        if (!item)
            return true;
        const QString text = item->text().trimmed();
        return (text.isEmpty() || text == dash) &&
               !item->data(kProgressBarRole).isValid() &&
               !item->data(kPacmanAnchorRole).isValid();
    };
    const auto placeRow = [&](int sourceRow, int row, bool flat) {
        for (const ColumnMap &map : kColumns) {
            const QTableWidgetItem *source =
                m_mirrorNodesTable->item(sourceRow, map.mirror);
            if (!source || blank(source))
                continue;
            if (flat && map.take == Take::Fill &&
                !blank(m_nodesTable->item(row, map.node)))
                continue;
            m_nodesTable->setItem(row, map.node, source->clone());
        }
    };

    int rows = m_nodesTable->rowCount();
    QHash<QString, int> rowByKey;
    int selfRow = -1;
    for (int row = 0; row < rows; ++row) {
        QTableWidgetItem *name = m_nodesTable->item(row, kNodeColName);
        if (!name) {
            name = new QTableWidgetItem;
            m_nodesTable->setItem(row, kNodeColName, name);
        }
        name->setData(kNodeGroupRole, QStringLiteral("row:%1").arg(row));
        if (name->data(Qt::UserRole + 5).toString() == QLatin1String("relay"))
            continue;
        const QString key = name->data(Qt::UserRole).toString().trimmed().toLower();
        if (!key.isEmpty() && !rowByKey.contains(key))
            rowByKey.insert(key, row);
        if (name->text().endsWith(QLatin1String("(this machine)")))
            selfRow = row;
    }

    QMap<int, QList<int>> byNodeRow;
    QMap<QString, QList<int>> unmatched;
    QStringList unmatchedOrder;
    if (m_mirrorNodesTable) {
        for (int r = 0; r < m_mirrorNodesTable->rowCount(); ++r) {
            const QTableWidgetItem *name =
                m_mirrorNodesTable->item(r, MirrorNodeColNode);
            if (!name || !name->data(kMirrorNodeKeysRole).isValid())
                continue;
            const QStringList keys = name->data(kMirrorNodeKeysRole).toStringList();
            int nodeRow = name->data(kMirrorNodeSelfRole).toBool() ? selfRow : -1;
            for (int k = 0; k < keys.size() && nodeRow < 0; ++k)
                nodeRow = rowByKey.value(keys.at(k), -1);
            if (nodeRow >= 0) {
                byNodeRow[nodeRow].append(r);
                continue;
            }
            const QString key = keys.value(0, QStringLiteral("mirror:%1").arg(r));
            if (!unmatched.contains(key))
                unmatchedOrder.append(key);
            unmatched[key].append(r);
        }
    }

    const QString childPrefix = QString::fromUtf8("    \xE2\x94\x94 ");
    const auto addChildRows = [&](int headRow, const QList<int> &sourceRows) {
        const QTableWidgetItem *head = m_nodesTable->item(headRow, kNodeColName);
        const QString group = head->data(kNodeGroupRole).toString();
        const QString nodeName = head->data(Qt::UserRole).toString();
        for (int sourceRow : sourceRows) {
            const int row = m_nodesTable->rowCount();
            m_nodesTable->insertRow(row);
            QTableWidgetItem *name =
                m_mirrorNodesTable->item(sourceRow, MirrorNodeColNode)->clone();
            name->setText(childPrefix + name->text());
            name->setData(Qt::UserRole, nodeName);
            name->setData(kNodeGroupRole, group);
            name->setData(kNodeGroupChildRole, true);
            m_nodesTable->setItem(row, kNodeColName, name);
            placeRow(sourceRow, row, false);
        }
    };

    for (auto it = byNodeRow.constBegin(); it != byNodeRow.constEnd(); ++it) {
        const int row = it.key();
        const QList<int> &sourceRows = it.value();
        if (sourceRows.size() > 1) {
            addChildRows(row, sourceRows);
            continue;
        }
        const int sourceRow = sourceRows.first();
        placeRow(sourceRow, row, true);
        const QTableWidgetItem *mirrorName =
            m_mirrorNodesTable->item(sourceRow, MirrorNodeColNode);
        QTableWidgetItem *name = m_nodesTable->item(row, kNodeColName);
        const QString star = QString::fromUtf8("  \xE2\x98\x85 source of truth");
        if (mirrorName->text().contains(star.trimmed()) &&
            !name->text().contains(star.trimmed()))
            name->setText(name->text() + star);
        const QString mirrorTip = mirrorName->toolTip().trimmed();
        if (!mirrorTip.isEmpty())
            name->setToolTip(name->toolTip().isEmpty()
                                 ? mirrorTip
                                 : name->toolTip() + QLatin1Char('\n') + mirrorTip);
        name->setData(kMirrorNodeIdRole, mirrorName->data(kMirrorNodeIdRole));
        if (mirrorName->data(kNodeLightRole).toInt() != 0) {
            name->setData(kNodeLightRole, mirrorName->data(kNodeLightRole));
            name->setIcon(mirrorName->icon());
            name->setForeground(mirrorName->foreground());
            name->setFont(mirrorName->font());
        }
    }

    for (const QString &key : std::as_const(unmatchedOrder)) {
        const QList<int> &sourceRows = unmatched.value(key);
        const int headSource = sourceRows.first();
        const int row = m_nodesTable->rowCount();
        m_nodesTable->insertRow(row);
        QTableWidgetItem *name =
            m_mirrorNodesTable->item(headSource, MirrorNodeColNode)->clone();
        const QString nodeName =
            name->text().section(QStringLiteral("  "), 0, 0).trimmed();
        name->setData(Qt::UserRole, nodeName);
        name->setData(Qt::UserRole + 5, QStringLiteral("mirror"));
        name->setData(kNodeGroupRole, QStringLiteral("mirror:%1").arg(key));
        m_nodesTable->setItem(row, kNodeColName, name);
        const bool online = name->data(kMirrorNodeOnlineRole).toBool();
        auto *status = new QTableWidgetItem(online ? QStringLiteral("Online")
                                                   : QStringLiteral("Offline"));
        status->setToolTip(name->toolTip());
        m_nodesTable->setItem(row, kNodeColStatus, status);
        placeRow(headSource, row, false);
        if (sourceRows.size() > 1)
            addChildRows(row, sourceRows.mid(1));
    }
}

void MainWindow::sortNodesTable()
{
    if (!m_nodesTable)
        return;
    const QHeaderView *header = m_nodesTable->horizontalHeader();
    const int column = header->sortIndicatorSection();
    const Qt::SortOrder order = header->sortIndicatorOrder();
    const int rows = m_nodesTable->rowCount();
    const int columns = m_nodesTable->columnCount();

    struct Group {
        int head;
        QList<int> children;
    };
    QList<Group> groups;
    QHash<QString, int> groupByKey;
    QList<int> children;
    for (int row = 0; row < rows; ++row) {
        const QTableWidgetItem *name = m_nodesTable->item(row, kNodeColName);
        if (name && name->data(kNodeGroupChildRole).toBool()) {
            children.append(row);
            continue;
        }
        const QString key = name ? name->data(kNodeGroupRole).toString() : QString();
        if (!key.isEmpty())
            groupByKey.insert(key, groups.size());
        groups.append(Group{row, {}});
    }
    for (int row : std::as_const(children)) {
        const int group = groupByKey.value(
            m_nodesTable->item(row, kNodeColName)->data(kNodeGroupRole).toString(),
            -1);
        if (group >= 0)
            groups[group].children.append(row);
        else
            groups.append(Group{row, {}});
    }

    if (column >= 0 && column < columns) {
        const auto itemLess = [](const QTableWidgetItem &left,
                                 const QTableWidgetItem &right) {
            const QVariant l = left.data(kTableSortRole);
            const QVariant r = right.data(kTableSortRole);
            if (l.isValid() && r.isValid()) {
                bool lOk = false;
                bool rOk = false;
                const double ln = l.toDouble(&lOk);
                const double rn = r.toDouble(&rOk);
                if (lOk && rOk)
                    return ln < rn;
                return l.toString().compare(r.toString(), Qt::CaseInsensitive) < 0;
            }
            return left < right;
        };
        std::stable_sort(groups.begin(), groups.end(),
                         [&](const Group &a, const Group &b) {
                             const QTableWidgetItem *l =
                                 m_nodesTable->item(a.head, column);
                             const QTableWidgetItem *r =
                                 m_nodesTable->item(b.head, column);
                             if (!l || !r)
                                 return l && !r;
                             return order == Qt::AscendingOrder ? itemLess(*l, *r)
                                                                : itemLess(*r, *l);
                         });
    }

    QList<int> newOrder;
    newOrder.reserve(rows);
    for (const Group &group : std::as_const(groups)) {
        newOrder.append(group.head);
        newOrder.append(group.children);
    }
    bool moved = false;
    for (int row = 0; row < rows && !moved; ++row)
        moved = newOrder.at(row) != row;
    if (moved) {
        QList<QList<QTableWidgetItem *>> taken(rows);
        for (int row = 0; row < rows; ++row) {
            m_nodesTable->removeCellWidget(row, kNodeColActions);
            taken[row].reserve(columns);
            for (int c = 0; c < columns; ++c)
                taken[row].append(m_nodesTable->takeItem(row, c));
        }
        for (int row = 0; row < rows; ++row) {
            const QList<QTableWidgetItem *> &source = taken.at(newOrder.at(row));
            for (int c = 0; c < columns; ++c)
                if (source.at(c))
                    m_nodesTable->setItem(row, c, source.at(c));
        }
    }
    refreshNodeActionButtons();
    applyNodeRoleFilter();
    if (Theme::isNext()) {
        nextRefreshNodeStats(m_nodesTable, m_networkDiagnosticsStatus);
        if (QWidget *scope = m_nodesTable->parentWidget())
            if (auto *panel = scope->findChild<QFrame *>(QStringLiteral("networkNodePanel")))
                nextFillNodePanel(m_nodesTable, panel);
    }
}

void MainWindow::refreshNodesStatusLine()
{
    if (!m_nodesTable || !m_nodesStatus || m_nodeDeleteInProgress)
        return;
    QHash<QString, int> roleCounts;
    int shown = 0;
    int online = 0;
    int total = 0;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        const QTableWidgetItem *name = m_nodesTable->item(row, kNodeColName);
        if (name && name->data(kNodeGroupChildRole).toBool())
            continue;
        ++total;
        if (const QTableWidgetItem *role = m_nodesTable->item(row, kNodeColRole))
            ++roleCounts[role->text()];
        if (const QTableWidgetItem *status = m_nodesTable->item(row, kNodeColStatus))
            online += status->text().startsWith(QLatin1String("Online")) ? 1 : 0;
        if (!m_nodesTable->isRowHidden(row))
            ++shown;
    }
    QStringList roleSummary;
    for (forkmesh::noderole::Role role : forkmesh::noderole::allRoles()) {
        const int n = roleCounts.value(forkmesh::noderole::label(role));
        if (n > 0)
            roleSummary.append(
                QStringLiteral("%1 %2%3")
                    .arg(n)
                    .arg(forkmesh::noderole::label(role).toLower())
                    .arg(n == 1 ? QString() : QStringLiteral("s")));
    }
    QString text = total == 0
        ? QStringLiteral("No nodes known yet.")
        : QString::fromUtf8("%1 node%2 \xC2\xB7 %3 online")
              .arg(total)
              .arg(total == 1 ? "" : "s")
              .arg(online);
    if (!roleSummary.isEmpty())
        text += QString::fromUtf8(" \xC2\xB7 ") +
                roleSummary.join(QStringLiteral(", "));
    if (shown != total)
        text += QString::fromUtf8(" \xC2\xB7 %1 shown").arg(shown);
    if (!m_servers.isEmpty()) {
        QList<forkmesh::nodedir::RelayProbe> probes;
        for (const ServerConfig &server : std::as_const(m_servers))
            probes.append(m_relayProbes.value(serverHost(server.url)));
        text += QString::fromUtf8(" \xC2\xB7 ") +
                forkmesh::nodedir::relaySummaryText(probes,
                                                    m_relayProbesUpdatedAt);
    }
    if (!m_appReleaseVersion.isEmpty())
        text += QString::fromUtf8(" \xC2\xB7 app release %1%2")
                    .arg(m_appReleaseVersion,
                         m_appReleaseRev.isEmpty()
                             ? QString()
                             : QStringLiteral(" (rev %1)")
                                   .arg(m_appReleaseRev.left(12)));
    m_nodesStatus->setStyleSheet(QString());
    m_nodesStatus->setText(text);
    m_nodesStatus->show();
    m_nodesStatus->setToolTip(QStringLiteral(
        "Core and mirror versions come from each node's published catalog "
        "record; the app release is the relay's /api/version, and each relay "
        "row shows its own. Runners publish no catalog, so their version is "
        "only in the daemon status."));
}

QList<forkmesh::nodedir::RunnerRow> MainWindow::directoryRunnerRows() const
{
    return forkmesh::nodedir::mergeRunnerRows(
        m_runners, forkmesh::corelogs::mergeFleetNodes(
                       forkmesh::corelogs::loadFleetNodes(), m_vultrFleetNodes));
}

void MainWindow::fillRelayNodeRow(int row, int serverIndex)
{
    if (!m_nodesTable || serverIndex < 0 || serverIndex >= m_servers.size())
        return;
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const ServerConfig &server = m_servers.at(serverIndex);
    const QString host = serverHost(server.url);
    const QString instanceName = server.name.trimmed().isEmpty()
                                     ? (host.isEmpty() ? server.url : host)
                                     : server.name.trimmed();
    const bool active = serverIndex == m_activeServer;
    auto *nameItem = new QTableWidgetItem(QIcon(faviconFor(server)), instanceName);
    nameItem->setData(Qt::UserRole, instanceName);
    nameItem->setData(Qt::UserRole + 5, QStringLiteral("relay"));
    nameItem->setData(Qt::UserRole + 6, host);
    nameItem->setToolTip(QStringLiteral("%1\nInstance id: %2%3")
                             .arg(server.url, server.id,
                                  active ? QStringLiteral("\nActive instance")
                                         : QString()));
    if (active) {
        QFont font = nameItem->font();
        font.setBold(true);
        nameItem->setFont(font);
    }
    m_nodesTable->setItem(row, kNodeColName, nameItem);
    m_nodesTable->setItem(row, kNodeColRole,
                          makeNodeRoleCell(forkmesh::noderole::Role::Relay));
    auto *scopeItem = new QTableWidgetItem(active ? QStringLiteral("active")
                                                  : QStringLiteral("standby"));
    scopeItem->setToolTip(active
        ? QStringLiteral("The active instance: accounts, repository actions, "
                         "admin tools and special agent replies use only this relay")
        : QStringLiteral("Configured, not active; switch to it from the relay "
                         "dropdown in the top bar"));
    m_nodesTable->setItem(row, kNodeColScope, scopeItem);
    auto *endpointItem = new QTableWidgetItem(host.isEmpty() ? dash : host);
    endpointItem->setToolTip(server.url);
    m_nodesTable->setItem(row, kNodeColEndpoint, endpointItem);
    auto *actionItem = new QTableWidgetItem(QStringLiteral("Website"));
    actionItem->setData(Qt::UserRole, instanceName);
    actionItem->setData(Qt::UserRole + 3, QStringLiteral("relay"));
    actionItem->setData(Qt::UserRole + 4, serverIndex);
    actionItem->setForeground(
        Theme::nextOr(Theme::Role::AccentFg, QColor("#58a6ff")));
    actionItem->setToolTip(QStringLiteral("Open %1 in the browser").arg(instanceName));
    actionItem->setFlags(Qt::ItemIsEnabled);
    m_nodesTable->setItem(row, kNodeColActions, actionItem);

    const forkmesh::nodedir::RelayProbe probe = m_relayProbes.value(host);
    const QString state = probe.state.isEmpty()
                              ? QString::fromUtf8("Checking\xE2\x80\xA6")
                              : probe.state;
    auto *statusItem = new QTableWidgetItem(state);
    if (state == QLatin1String("Online"))
        statusItem->setForeground(QColor(QStringLiteral("#3fb950")));
    else if (state == QLatin1String("Offline"))
        statusItem->setForeground(QColor(QStringLiteral("#8b949e")));
    if (!probe.failure.isEmpty())
        statusItem->setToolTip(probe.failure);
    m_nodesTable->setItem(row, kNodeColStatus, statusItem);
    auto *latencyItem = new SortTableWidgetItem(
        probe.latencyMs >= 0 ? QStringLiteral("%1 ms").arg(probe.latencyMs) : dash);
    latencyItem->setData(kTableSortRole, double(probe.latencyMs));
    latencyItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (probe.latencyMs >= 0)
        latencyItem->setToolTip(
            QStringLiteral("Response time of the relay's /api/version"));
    m_nodesTable->setItem(row, kNodeColLatency, latencyItem);
    m_nodesTable->setItem(row, kNodeColVersion,
                          new QTableWidgetItem(probe.version.isEmpty()
                                                   ? dash
                                                   : probe.version));
}

void MainWindow::updateRelayNodeRows()
{
    if (!m_nodesTable)
        return;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        const QTableWidgetItem *name = m_nodesTable->item(row, kNodeColName);
        if (!name ||
            name->data(Qt::UserRole + 5).toString() != QLatin1String("relay"))
            continue;
        const QString host = name->data(Qt::UserRole + 6).toString();
        for (int i = 0; i < m_servers.size(); ++i) {
            if (serverHost(m_servers.at(i).url) == host) {
                fillRelayNodeRow(row, i);
                break;
            }
        }
    }
    sortNodesTable();
    refreshNodesStatusLine();
}

void MainWindow::fillRunnerNodeRow(int row,
                                   const forkmesh::nodedir::RunnerRow &runner,
                                   qint64 nowMs)
{
    if (!m_nodesTable)
        return;
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const bool online = forkmesh::nodedir::runnerOnline(runner, nowMs);
    auto *nameItem =
        new QTableWidgetItem(osBadgeIcon(QStringLiteral("linux"), online, 16),
                             runner.name);
    nameItem->setData(Qt::UserRole, runner.name);
    nameItem->setData(Qt::UserRole + 5, QStringLiteral("runner"));
    nameItem->setData(Qt::UserRole + 6, runner.feedName);
    QStringList tip;
    if (!runner.label.isEmpty())
        tip.append(QStringLiteral("Fleet instance %1").arg(runner.label));
    if (!runner.registered)
        tip.append(QStringLiteral(
            "Not in the runner registry this account can read (pool runners "
            "are listed for app admins)"));
    nameItem->setToolTip(tip.join(QLatin1Char('\n')));
    m_nodesTable->setItem(row, kNodeColName, nameItem);
    m_nodesTable->setItem(row, kNodeColRole,
                          makeNodeRoleCell(forkmesh::noderole::Role::Runner));
    m_nodesTable->setItem(
        row, kNodeColScope,
        new QTableWidgetItem(runner.scope.isEmpty() ? dash : runner.scope));
    auto *statusItem = new QTableWidgetItem(
        forkmesh::nodedir::runnerStatusText(runner, nowMs));
    if (online)
        statusItem->setForeground(QColor(QStringLiteral("#3fb950")));
    if (runner.lastSeenAtMs > 0)
        statusItem->setToolTip(
            QStringLiteral("Last check-in %1")
                .arg(QDateTime::fromMSecsSinceEpoch(runner.lastSeenAtMs)
                         .toString(Qt::ISODate)));
    m_nodesTable->setItem(row, kNodeColStatus, statusItem);
    const QString activity = forkmesh::nodedir::runnerActivityText(runner);
    auto *activityItem =
        new QTableWidgetItem(activity.isEmpty() ? dash : activity);
    activityItem->setToolTip(activity.isEmpty()
                                 ? QStringLiteral("Job leases not reported")
                                 : QStringLiteral("Unexpired agent job leases"));
    m_nodesTable->setItem(row, kNodeColActivity, activityItem);
    const qint64 seenSecs = runner.lastSeenAtMs / 1000;
    auto *checkedItem = new SortTableWidgetItem(
        seenSecs > 0 ? formatShortRelativeTime(seenSecs) + QStringLiteral(" ago")
                     : dash);
    checkedItem->setData(kTableSortRole, double(seenSecs));
    checkedItem->setToolTip(QStringLiteral("Last check-in with the app"));
    m_nodesTable->setItem(row, kNodeColChecked, checkedItem);
    m_nodesTable->setItem(row, kNodeColOwner,
                          new QTableWidgetItem(runner.registeredBy.isEmpty()
                                                   ? dash
                                                   : runner.registeredBy));
    auto *endpointItem =
        new QTableWidgetItem(runner.address.isEmpty() ? dash : runner.address);
    if (!runner.label.isEmpty())
        endpointItem->setToolTip(runner.label);
    m_nodesTable->setItem(row, kNodeColEndpoint, endpointItem);
    auto *actionItem = new QTableWidgetItem(
        runner.feedName.isEmpty() ? QString() : QStringLiteral("Logs"));
    actionItem->setData(Qt::UserRole, runner.name);
    actionItem->setData(Qt::UserRole + 3, QStringLiteral("runner"));
    actionItem->setData(Qt::UserRole + 4, runner.feedName);
    actionItem->setForeground(
        Theme::nextOr(Theme::Role::AccentFg, QColor("#58a6ff")));
    if (!runner.feedName.isEmpty())
        actionItem->setToolTip(
            QStringLiteral("Open the Log page filtered to %1's journal")
                .arg(runner.feedName));
    actionItem->setFlags(Qt::ItemIsEnabled);
    m_nodesTable->setItem(row, kNodeColActions, actionItem);
}

void MainWindow::activateNodeDirectoryRow(int row)
{
    if (!m_nodesTable || row < 0 || row >= m_nodesTable->rowCount())
        return;
    const QTableWidgetItem *action = m_nodesTable->item(row, kNodeColActions);
    if (!action)
        return;
    const QString kind = action->data(Qt::UserRole + 3).toString();
    if (kind == QLatin1String("relay")) {
        openServerWebsite(action->data(Qt::UserRole + 4).toInt());
    } else if (kind == QLatin1String("runner")) {
        const QString feed = action->data(Qt::UserRole + 4).toString();
        if (!feed.isEmpty())
            openNodeLogsFor(feed);
    }
}

void MainWindow::openNodeLogsFor(const QString &feedName)
{
    m_nodeLogFilter = {};
    m_nodeLogFilter.nodes.insert(feedName);
    updateNodeLogFilterButton();
    m_networkLogViewStale = true;
    showSection(4);
    refreshLogTimelineChart();
}

void MainWindow::showNodesForRole(const QString &roleKey)
{
    QSettings().setValue(QStringLiteral("network/nodesRoleFilter"), roleKey);
    showNetworkTab(kNetworkNodesTab);
    if (!m_nodesRoleFilter)
        return;
    const int index = m_nodesRoleFilter->findData(roleKey);
    if (index >= 0 && index != m_nodesRoleFilter->currentIndex())
        m_nodesRoleFilter->setCurrentIndex(index);
    else
        applyNodeRoleFilter();
}

void MainWindow::noteAppRelease(const QJsonObject &versionReply)
{
    const QString version =
        versionReply.value(QStringLiteral("version")).toString().trimmed();
    const QString rev = versionReply.value(QStringLiteral("rev")).toString().trimmed();
    if (version.isEmpty() ||
        (version == m_appReleaseVersion && rev == m_appReleaseRev))
        return;
    m_appReleaseVersion = version;
    m_appReleaseRev = rev;
    if (m_nodesTable && m_nodesTable->isVisible())
        refreshNodesTable();
}

QSet<QString> MainWindow::savedHostNodeNames() const
{
    QSet<QString> names;
    QSettings settings;
    const QJsonArray hosts =
        forkmesh::control::loadSavedHosts(settings, kHostsSetting, nullptr);
    for (const QJsonValue &value : hosts) {
        const QString name = value.toObject()
                                 .value(QStringLiteral("name"))
                                 .toString()
                                 .trimmed()
                                 .toLower();
        if (!name.isEmpty())
            names.insert(name);
    }
    return names;
}

forkmesh::noderole::Role
MainWindow::nodeRoleFor(const forkmesh::noderole::Facts &facts) const
{
    if (!m_nextFleetNamesLoaded) {
        m_nextFleetNamesLoaded = true;
        for (const QString &path : {forkmesh::corelogs::fleetInstancesPath(),
                                    forkmesh::corelogs::autoscaledInstancesPath()}) {
            QFile fleet(path);
            if (!fleet.open(QIODevice::ReadOnly))
                continue;
            const forkmesh::noderole::FleetNames names =
                forkmesh::noderole::parseFleetInstances(fleet.read(1024 * 1024));
            m_nextFleetNames.core.unite(names.core);
            m_nextFleetNames.runner.unite(names.runner);
        }
    }
    return forkmesh::noderole::classify(facts, m_nextFleetNames);
}

QTableWidgetItem *MainWindow::makeNodeRoleCell(forkmesh::noderole::Role role)
{
    auto *item = new QTableWidgetItem(forkmesh::noderole::label(role));
    item->setData(Qt::UserRole, forkmesh::noderole::key(role));
    item->setToolTip(forkmesh::noderole::description(role));
    return item;
}

void MainWindow::applyNodeRoleFilter()
{
    const QString wanted =
        m_nodesRoleFilter ? m_nodesRoleFilter->currentData().toString() : QString();
    for (QTableWidget *table : {m_nodesTable, m_mirrorNodesTable}) {
        if (!table)
            continue;
        int roleColumn = -1;
        for (int c = 0; c < table->columnCount() && roleColumn < 0; ++c) {
            const QTableWidgetItem *header = table->horizontalHeaderItem(c);
            if (header && header->data(Qt::UserRole).toString() ==
                              QLatin1String("role"))
                roleColumn = c;
        }
        if (roleColumn < 0)
            continue;
        bool headHidden = false;
        for (int row = 0; row < table->rowCount(); ++row) {
            const QTableWidgetItem *name = table->item(row, 0);
            if (table == m_nodesTable && name &&
                name->data(kNodeGroupChildRole).toBool()) {
                table->setRowHidden(row, headHidden);
                continue;
            }
            const QTableWidgetItem *role = table->item(row, roleColumn);
            headHidden = !wanted.isEmpty() && role &&
                         role->data(Qt::UserRole).toString() != wanted;
            table->setRowHidden(row, headHidden);
        }
    }
    if (m_nodesTable && Theme::isNext())
        nextApplyNodeTextFilter(m_nodesTable);
}

// Merge the roster, the chat directory's linked nodes and the relay's serving
// set into the rows the Nodes table (and the chrome dot matrix) shows.
void MainWindow::collectVisibleMeshNodes(QList<NodeMenuEntry> &visible,
                                         QList<MemberInfo> &visibleRoster)
{
    auto rosterInfo = [this](const QString &name) -> MemberInfo {
        MemberInfo best;
        bool found = false;
        for (const MemberInfo &m : std::as_const(m_homeRoster)) {
            if (nodeListIdentityKey(m) != name)
                continue;
            if (!found || (m.online && !best.online)) {
                best = m;
                found = true;
                continue;
            }
            if (best.ownerUser.trimmed().isEmpty())
                best.ownerUser = m.ownerUser;
            if (best.version.trimmed().isEmpty())
                best.version = m.version;
            if (best.solanaAddress.trimmed().isEmpty())
                best.solanaAddress = m.solanaAddress;
            if (best.platform.trimmed().isEmpty())
                best.platform = m.platform;
            if (best.mirrors.isEmpty() && !m.mirrors.isEmpty()) {
                best.mirrors = m.mirrors;
                best.mirrorDetails = m.mirrorDetails;
            }
        }
        return best;
    };
    auto relayOnline = [this](const QString &name) {
        return m_relayOnlineNodes.contains(name.trimmed().toLower());
    };
    auto catalogPublishedMs = [this](const QString &name) {
        const QJsonValue value =
            m_nodesCatalogInfo.value(name.trimmed().toLower()).value(
                QStringLiteral("lastSync"));
        if (value.isString()) {
            bool ok = false;
            const qint64 stamp = value.toString().toLongLong(&ok);
            return ok ? stamp : qint64(0);
        }
        return qint64(value.toDouble());
    };
    auto wasDeleted = [this, &catalogPublishedMs](const QString &name) {
        const QString key = name.trimmed().toLower();
        return m_deletedMeshNodeNames.contains(key) ||
               meshNodeWasDeleted(key, catalogPublishedMs(key));
    };

    QHash<QString, QString> directoryOwner;
    for (const MemberInfo &user : std::as_const(m_chatDirectoryUsers)) {
        const QString owner = user.name.trimmed();
        const QStringList nodes =
            user.nodeName.split(QStringLiteral(", "), Qt::SkipEmptyParts);
        for (const QString &node : nodes) {
            const QString key = node.trimmed().toLower();
            if (!key.isEmpty())
                directoryOwner.insert(key, owner);
        }
    }

    // Drop entries whose roster identity is a plain user account (a chat-only
    // human/bot, accountKind "user") rather than a real serving node — e.g.
    // ForkBot's relayed replies or a desktop profile signed in as a user, not a
    // linked node. Missing accountKind (older peers, or a name only known via
    // a locally hosted repo's owner field) still counts as a node.

    // Our own row is the same story: a desktop signed in as a user account is a
    // *user*, not a node — its nodes show as their own rows. The backend stamps
    // that as accountKind "user" on the self entry, but the self row carries
    // live telemetry and can be painted before the kind propagates (
    // "jett" listed as a node), so gate it on the local predicate directly.
    const bool selfIsUserAccount =
        m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty();
    const QString localUser = selfIsUserAccount
                                  ? accountOwner().trimmed().toLower()
                                  : QString();
    auto isDirectoryUser = [this, localUser](const QString &key) {
        const QString normalized = key.trimmed().toLower();
        return m_chatDirectoryUsers.contains(normalized) ||
               (!localUser.isEmpty() && normalized == localUser);
    };
    QSet<QString> visibleNames;
    for (const NodeMenuEntry &e : std::as_const(m_nodeMenuEntries)) {
        MemberInfo mi = rosterInfo(e.name);
        const QString key = e.name.trimmed().toLower();
        if (key.isEmpty() || wasDeleted(key) || isDirectoryUser(key))
            continue;
        if (mi.accountKind == QLatin1String("user"))
            continue;
        if (isTemporaryChatGuest(mi))
            continue;
        if (e.self && selfIsUserAccount)
            continue;
        if (visibleNames.contains(key))
            continue;
        if (mi.ownerUser.trimmed().isEmpty())
            mi.ownerUser = directoryOwner.value(key);
        visible.append(e);
        visibleRoster.append(mi);
        visibleNames.insert(key);
    }
    QStringList directoryNodes = directoryOwner.keys();
    std::sort(directoryNodes.begin(), directoryNodes.end(),
              [](const QString &a, const QString &b) {
                  return a.compare(b, Qt::CaseInsensitive) < 0;
              });
    for (const QString &key : std::as_const(directoryNodes)) {
        if (wasDeleted(key) || visibleNames.contains(key))
            continue;
        NodeMenuEntry entry;
        entry.name = key;
        entry.online = relayOnline(key);
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (!repo.previewOnly &&
                repo.owner.compare(key, Qt::CaseInsensitive) == 0)
                ++entry.repoCount;
        }
        MemberInfo info = rosterInfo(key);
        info.ownerUser = directoryOwner.value(key);
        visible.append(entry);
        visibleRoster.append(info);
        visibleNames.insert(key);
    }
    QStringList servingNodes = m_relayOnlineNodes.values();
    std::sort(servingNodes.begin(), servingNodes.end(),
              [](const QString &a, const QString &b) {
                  return a.compare(b, Qt::CaseInsensitive) < 0;
              });
    for (const QString &key : std::as_const(servingNodes)) {
        if (key.isEmpty() || wasDeleted(key) || isDirectoryUser(key) ||
            visibleNames.contains(key))
            continue;
        NodeMenuEntry entry;
        entry.name = key;
        entry.online = true;
        MemberInfo info = rosterInfo(key);
        visible.append(entry);
        visibleRoster.append(info);
        visibleNames.insert(key);
    }
    // A headless mirror that serves via the relay never joins the chat room, so
    // its roster record is blank — the whole stats side of the table (version /
    // platform / CPU / RAM / disk / mirrors) went em-dash for the entire fleet.
    // Its signed catalog mirror records still carry all of that (renewed every
    // registration lease), so backfill whatever the roster left empty.
    for (int i = 0; i < visibleRoster.size(); ++i)
        applyCatalogNodeInfo(visibleRoster[i], visible.at(i).name);
}

bool MainWindow::nodeEntryOnline(const NodeMenuEntry &e) const
{
    const bool relayLive =
        m_relayOnlineNodes.contains(e.name.trimmed().toLower());
    if (e.self)
        return e.online;
    if (m_relayOnlineNodesFetched)
        return relayLive;
    return e.online || relayLive; // pre-fetch fallback
}

// One Nodes-table row: identity, status, catalog columns and live telemetry.
// Returns whether the node counts as online.
bool MainWindow::fillNodesTableRow(int i, const NodeMenuEntry &e,
                                   const QSet<QString> &savedHosts,
                                   const MemberInfo &mi, qint64 tableNowMs,
                                   bool &sawCatalogRecord)
{
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    // The relay's /api/network/stats "onlineNodes" is the network-canonical
    // live set (an update channel or a fresh signed heartbeat) — the same
    // signal the Mirror nodes list and the Network page trust. Once we have
    // fetched it, it is authoritative even over a roster entry that still
    // says "online": a node whose chat socket lingers after the machine
    // stopped serving was being painted online here when it no longer was
    // Our own node is the exception — trust the local backend
    // for it, since we may host only private repos the relay never lists.
    const bool rosterLive = e.online;
    const bool isOnline = nodeEntryOnline(e);
    const bool serving = isOnline && !e.self && !rosterLive;

    QString label = e.name.isEmpty() ? QStringLiteral("(unnamed)") : e.name;
    if (e.self)
        label += QStringLiteral("  (this machine)");
    QString platformText = e.platform.trimmed();
    if (platformText.isEmpty())
        platformText = mi.platform.trimmed();
    auto *nameItem =
        new QTableWidgetItem(osBadgeIcon(platformText, isOnline, 16), label);
    nameItem->setData(Qt::UserRole, e.name);
    nameItem->setData(Qt::UserRole + 1, mi.ownerUser.trimmed());
    nameItem->setData(Qt::UserRole + 2, platformText);
    nameItem->setData(Qt::UserRole + 3, e.repoCount);
    nameItem->setData(Qt::UserRole + 4, int(mi.mirrors.size()));
    m_nodesTable->setItem(i, kNodeColName, nameItem);
    {
        const QString key = e.name.trimmed().toLower();
        const QJsonObject catalog = m_nodesCatalogInfo.value(key);
        forkmesh::noderole::Facts facts;
        facts.name = e.name;
        facts.self = e.self;
        facts.savedHost = savedHosts.contains(key);
        facts.declaredRole = catalog.value(QStringLiteral("role")).toString();
        facts.runtimeMode = catalog.value(QStringLiteral("runtimeMode")).toString();
        m_nodesTable->setItem(i, kNodeColRole, makeNodeRoleCell(nodeRoleFor(facts)));
    }

    auto *statusItem = new QTableWidgetItem(
        !isOnline ? QStringLiteral("Offline")
                  : (serving ? QStringLiteral("Online (serving)")
                             : QStringLiteral("Online")));
    if (serving)
        statusItem->setToolTip(QStringLiteral(
            "The relay reports this node live (update channel / signed "
            "heartbeat) even though it isn't in this client's chat room."));
    m_nodesTable->setItem(i, kNodeColStatus, statusItem);

    if (fillNodesTableCatalogCells(i, e.name))
        sawCatalogRecord = true;

    const QString owner = mi.ownerUser.trimmed();
    m_nodesTable->setItem(i, kNodeColOwner,
                          new QTableWidgetItem(owner.isEmpty() ? dash : owner));

    const QString version = mi.version.trimmed();
    m_nodesTable->setItem(i, kNodeColVersion, new QTableWidgetItem(
        version.isEmpty() ? dash : version));

    m_nodesTable->setItem(i, kNodeColPlatform, new QTableWidgetItem(
        platformText.isEmpty() ? dash : platformText));

    auto *repoItem = new QTableWidgetItem;
    repoItem->setData(Qt::DisplayRole, e.repoCount); // int -> numeric sort
    repoItem->setTextAlignment(Qt::AlignCenter);
    m_nodesTable->setItem(i, kNodeColRepos, repoItem);

    auto *mirrorItem = new QTableWidgetItem;
    mirrorItem->setData(Qt::DisplayRole, int(mi.mirrors.size()));
    mirrorItem->setTextAlignment(Qt::AlignCenter);
    m_nodesTable->setItem(i, kNodeColMirrors, mirrorItem);

    m_nodesTable->setItem(
        i, kNodeColHealth,
        makeNodeHealthCell(mi.diagnostics, mi.diagnosticsMs, tableNowMs));

    m_nodesTable->setItem(i, kNodeColCpu, makeCpuUsageCell(mi.cpuPercent));
    m_nodesTable->setItem(i, kNodeColRam,
        makeByteUsageCell(QStringLiteral("RAM"), mi.memUsedBytes,
                          mi.memTotalBytes));
    m_nodesTable->setItem(i, kNodeColDisk,
        makeByteUsageCell(QStringLiteral("Disk"), mi.diskUsedBytes,
                          mi.diskTotalBytes));

    // Stable node id (public key), shortened like the Mirror nodes table;
    // the full key stays readable via the tooltip.
    const QString nodeId = mi.id.trimmed();
    auto *idItem = new QTableWidgetItem(
        nodeId.isEmpty()
            ? dash
            : nodeId.left(12) + (nodeId.size() > 12
                                     ? QString::fromUtf8("\xE2\x80\xA6")
                                     : QString()));
    if (!nodeId.isEmpty())
        idItem->setToolTip(nodeId);
    m_nodesTable->setItem(i, kNodeColId, idItem);

    auto *actionItem = new QTableWidgetItem;
    actionItem->setData(Qt::UserRole, e.name);
    actionItem->setData(Qt::UserRole + 1, nodeId);
    actionItem->setData(Qt::UserRole + 2, e.self);
    actionItem->setFlags(Qt::ItemIsEnabled);
    m_nodesTable->setItem(i, kNodeColActions, actionItem);
    return isOnline;
}

// The website Mirrors-table columns for one node row, from its freshest signed
// catalog record. Returns whether that record exists.
bool MainWindow::fillNodesTableCatalogCells(int i, const QString &nodeName)
{
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    // Cell builders for the mirror-record columns the website's Mirrors table
    // publishes. Everything here shares the "-1 / empty means the node never
    // reported it" rule, so an em dash is a real state and never a fake zero.
    auto textCell = [&dash](const QString &value,
                            const QString &tip = QString()) {
        const QString text = value.trimmed();
        auto *item = new QTableWidgetItem(text.isEmpty() ? dash : text);
        if (!tip.isEmpty())
            item->setToolTip(tip);
        else if (!text.isEmpty())
            item->setToolTip(text);
        return item;
    };
    auto countCell = [&dash](int value, const QString &singular,
                             const QString &plural) {
        auto *item = new SortTableWidgetItem(
            value >= 0 ? formatCount(value) : dash);
        item->setData(kTableSortRole, double(value));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        if (value >= 0)
            item->setToolTip(QStringLiteral("%1 %2")
                                 .arg(formatCount(value),
                                      value == 1 ? singular : plural));
        return item;
    };
    auto agoCell = [&dash](qint64 whenMs) {
        const qint64 secs = whenMs / 1000;
        auto *item = new SortTableWidgetItem(
            secs > 0 ? formatShortRelativeTime(secs) + QStringLiteral(" ago")
                     : dash);
        item->setData(kTableSortRole, double(secs));
        if (secs > 0)
            item->setToolTip(
                QDateTime::fromSecsSinceEpoch(secs).toString(Qt::ISODate));
        return item;
    };
    // The rest of the website's Mirrors columns come from the node's
    // freshest signed catalog record (fetchNodesCatalogInfo sweeps the same
    // /api/repo/<owner>/<repo>/mirrors endpoint the website reads). Repo
    // facts — revision, size, counts — describe that one repository, so the
    // tooltips name it rather than implying a fleet-wide total.
    const QJsonObject catalog =
        m_nodesCatalogInfo.value(nodeName.trimmed().toLower());
    const QString catalogRepoName =
        catalog.value(QStringLiteral("repo")).toString().trimmed();
    const QString catalogRepoOwner =
        catalog.value(QStringLiteral("owner")).toString().trimmed();
    const QString catalogRepo =
        catalogRepoName.isEmpty() || catalogRepoOwner.isEmpty()
            ? catalogRepoName
            : catalogRepoOwner + QLatin1Char('/') + catalogRepoName;
    const QString repoNote =
        catalogRepo.isEmpty()
            ? QString()
            : QStringLiteral("\nReported for %1").arg(catalogRepo);
    auto repoCountCell = [&](const char *field, const QString &singular,
                             const QString &plural) {
        auto *item = countCell(
            catalog.value(QLatin1String(field)).toInt(-1), singular, plural);
        if (!repoNote.isEmpty() && !item->toolTip().isEmpty())
            item->setToolTip(item->toolTip() + repoNote);
        return item;
    };

    const QString activity =
        catalog.value(QStringLiteral("activity")).toString().trimmed().toLower();
    static const QHash<QString, QString> kActivityLabels = {
        {QStringLiteral("running-actions"), QStringLiteral("running actions")},
        {QStringLiteral("syncing"), QStringLiteral("syncing refs")},
        {QStringLiteral("verifying"), QStringLiteral("verifying")},
        {QStringLiteral("integrity-blocked"), QStringLiteral("integrity blocked")},
        {QStringLiteral("awaiting-verification"),
         QStringLiteral("awaiting verification")},
        {QStringLiteral("serving"), QStringLiteral("serving")},
        {QStringLiteral("offline"), QStringLiteral("offline")},
    };
    m_nodesTable->setItem(
        i, kNodeColActivity,
        textCell(kActivityLabels.value(activity, activity),
                 activity.isEmpty() ? QStringLiteral("Activity not reported")
                                    : QString()));

    const QString commit =
        catalog.value(QStringLiteral("commit")).toString().trimmed();
    const QString branch =
        catalog.value(QStringLiteral("branch")).toString().trimmed();
    m_nodesTable->setItem(
        i, kNodeColRevision,
        textCell(commit.isEmpty()
                     ? QString()
                     : commit.left(7) + (branch.isEmpty()
                                             ? QString()
                                             : QLatin1Char('/') + branch),
                 commit.isEmpty() ? QString() : commit + repoNote));
    m_nodesTable->setItem(
        i, kNodeColSynced,
        agoCell(qint64(catalog.value(QStringLiteral("lastSync")).toDouble())));

    const QString endpoint =
        catalog.value(QStringLiteral("endpoint")).toString().trimmed();
    const QUrl endpointUrl(endpoint);
    // Host and port, the way the website renders it: a mirror on a
    // non-standard port is a different endpoint, not the same one.
    QString endpointHost = endpointUrl.host();
    if (!endpointHost.isEmpty() && endpointUrl.port() > 0)
        endpointHost += QStringLiteral(":%1").arg(endpointUrl.port());
    m_nodesTable->setItem(i, kNodeColEndpoint,
                          textCell(endpointHost, endpoint));
    m_nodesTable->setItem(
        i, kNodeColChecked,
        agoCell(qint64(catalog.value(QStringLiteral("checkedAt")).toDouble())));

    auto *latencyItem = new SortTableWidgetItem(dash);
    if (catalog.contains(QStringLiteral("latencyMs"))) {
        const int latencyMs =
            qMax(0, catalog.value(QStringLiteral("latencyMs")).toInt());
        latencyItem->setText(QStringLiteral("%1 ms").arg(latencyMs));
        latencyItem->setData(kTableSortRole, double(latencyMs));
        latencyItem->setToolTip(QStringLiteral(
            "Endpoint answered the last health check in %1 ms").arg(latencyMs));
    } else {
        latencyItem->setData(kTableSortRole, -1.0);
    }
    latencyItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_nodesTable->setItem(i, kNodeColLatency, latencyItem);

    m_nodesTable->setItem(
        i, kNodeColRegion,
        textCell(catalog.value(QStringLiteral("region")).toString()));

    const QString integrity =
        catalog.value(QStringLiteral("endpointIntegrity")).toString().trimmed();
    auto *integrityItem = textCell(integrity);
    if (!integrity.isEmpty() && integrity != QLatin1String("ok")) {
        // Same signal the website paints amber for: the endpoint is not
        // proving the content it serves.
        integrityItem->setForeground(QColor("#d29922"));
        QFont integrityFont = integrityItem->font();
        integrityFont.setUnderline(true);
        integrityItem->setFont(integrityFont);
    }
    m_nodesTable->setItem(i, kNodeColIntegrity, integrityItem);

    QStringList operations;
    for (const QJsonValue &value :
         catalog.value(QStringLiteral("operations")).toArray()) {
        const QString operation = value.toString().trimmed();
        if (!operation.isEmpty())
            operations.append(operation);
    }
    auto *capsItem = countCell(operations.isEmpty() ? -1 : operations.size(),
                               QStringLiteral("capability"),
                               QStringLiteral("capabilities"));
    capsItem->setToolTip(operations.isEmpty()
                             ? QStringLiteral("Capabilities not reported")
                             : QStringLiteral("Capabilities: %1")
                                   .arg(operations.join(QStringLiteral(", "))));
    m_nodesTable->setItem(i, kNodeColCaps, capsItem);

    const qint64 sizeBytes =
        qint64(catalog.value(QStringLiteral("sizeBytes")).toDouble());
    auto *sizeItem = new SortTableWidgetItem(
        sizeBytes > 0 ? formatByteSize(sizeBytes) : dash);
    sizeItem->setData(kTableSortRole, double(sizeBytes));
    sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    if (sizeBytes > 0)
        sizeItem->setToolTip(
            QStringLiteral("Repository size reported by this node%1")
                .arg(repoNote));
    m_nodesTable->setItem(i, kNodeColSize, sizeItem);

    m_nodesTable->setItem(
        i, kNodeColIssues,
        repoCountCell("issueCount", QStringLiteral("issue"),
                      QStringLiteral("issues")));
    m_nodesTable->setItem(
        i, kNodeColCommits,
        repoCountCell("commitCount", QStringLiteral("commit"),
                      QStringLiteral("commits")));
    m_nodesTable->setItem(
        i, kNodeColBranches,
        repoCountCell("branchCount", QStringLiteral("branch"),
                      QStringLiteral("branches")));
    m_nodesTable->setItem(
        i, kNodeColPulls,
        repoCountCell("pullCount", QStringLiteral("pull request"),
                      QStringLiteral("pull requests")));
    m_nodesTable->setItem(
        i, kNodeColDiscussions,
        repoCountCell("discussionCount", QStringLiteral("discussion"),
                      QStringLiteral("discussions")));
    m_nodesTable->setItem(
        i, kNodeColArtifacts,
        repoCountCell("artifactCount", QStringLiteral("artifact"),
                      QStringLiteral("artifacts")));
    m_nodesTable->setItem(
        i, kNodeColWorktrees,
        repoCountCell("worktreeCount", QStringLiteral("worktree"),
                      QStringLiteral("worktrees")));
    m_nodesTable->setItem(
        i, kNodeColClones,
        countCell(catalog.value(QStringLiteral("clonesServed")).toInt(-1),
                  QStringLiteral("clone served"),
                  QStringLiteral("clones served")));
    m_nodesTable->setItem(
        i, kNodeColWebsite,
        countCell(catalog.value(QStringLiteral("websiteServed")).toInt(-1),
                  QStringLiteral("website request"),
                  QStringLiteral("website requests")));
    return !catalog.isEmpty();
}

void MainWindow::refreshNodeActionButtons()
{
    if (!m_nodesTable)
        return;
    bool linkActions = false;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        const QTableWidgetItem *item = m_nodesTable->item(row, kNodeColActions);
        linkActions |= item && !item->data(Qt::UserRole + 3).toString().isEmpty() &&
                       !item->text().isEmpty();
    }
    m_nodesTable->setColumnHidden(kNodeColActions, !m_isAdmin && !linkActions);
    if (!m_isAdmin)
        return;
    const QString actor = accountOwner().trimmed();
    int buttonHeight = 0;
    int buttonWidth = 0;
    for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_nodesTable->item(row, kNodeColActions);
        const QString node =
            item ? item->data(Qt::UserRole).toString().trimmed() : QString();
        const QString nodeId =
            item ? item->data(Qt::UserRole + 1).toString() : QString();
        const bool self =
            item && (item->data(Qt::UserRole + 2).toBool() ||
                     (!actor.isEmpty() &&
                      node.compare(actor, Qt::CaseInsensitive) == 0));
        if (node.isEmpty() || self || isProtectedMeshNode(node) ||
            (item && !item->data(Qt::UserRole + 3).toString().isEmpty())) {
            m_nodesTable->removeCellWidget(row, kNodeColActions);
            continue;
        }
        auto *deleteButton = new QPushButton(QStringLiteral("Delete"));
        deleteButton->setObjectName(QStringLiteral("dangerButton"));
        deleteButton->setCursor(Qt::PointingHandCursor);
        // This button sets the height of every row in the directory, so it is
        // the smallest of the theme's sizes: a taller one spaced 22 nodes out
        // over a screen and a half.
        deleteButton->setProperty("buttonSize", "xs");
        setOcticon(deleteButton, "trash", 12);
        if (m_nodeDeleteInProgress) {
            deleteButton->setEnabled(false);
            deleteButton->setToolTip(
                QStringLiteral("Deleting \"%1\"; wait for the relay result.")
                    .arg(m_nodeDeleteTarget));
        } else {
            deleteButton->setToolTip(QString::fromUtf8(
                "Delete \"%1\" for good: destroy its Vultr server, remove its DNS "
                "record, erase it from the relay (account, mirrors, agent jobs and "
                "the /status history) and forget the saved SSH host here.")
                                         .arg(node));
        }
        connect(deleteButton, &QPushButton::clicked, this,
                [this, node, nodeId] {
                    QTimer::singleShot(0, this, [this, node, nodeId] {
                        deleteMeshNodeCompletely(node, nodeId);
                    });
                });
        m_nodesTable->setCellWidget(row, kNodeColActions, deleteButton);
        buttonHeight = qMax(buttonHeight, deleteButton->sizeHint().height());
        buttonWidth = qMax(buttonWidth, deleteButton->sizeHint().width());
    }
    if (buttonWidth > 0 &&
        m_nodesTable->columnWidth(kNodeColActions) < buttonWidth + 16)
        m_nodesTable->setColumnWidth(kNodeColActions, buttonWidth + 16);
    if (buttonHeight > 0) {
        for (int row = 0; row < m_nodesTable->rowCount(); ++row) {
            if (m_nodesTable->rowHeight(row) < buttonHeight + 4)
                m_nodesTable->setRowHeight(row, buttonHeight + 4);
        }
    }
}

void MainWindow::setNodeDeleteStatus(const QString &text)
{
    if (m_nodesStatus) {
        m_nodesStatus->setStyleSheet(QString());
        m_nodesStatus->setText(text);
        m_nodesStatus->show();
    }
    logSystem(text);
}

void MainWindow::deleteMeshNodeCompletely(const QString &node,
                                          const QString &nodeId)
{
    const QString target = node.trimmed().toLower();
    if (target.isEmpty() || isProtectedMeshNode(target))
        return;
    // Every refusal below is this gate answering the operator's click before
    // anything is attempted, so each is labelled a local operation and stays
    // off the relay's error log (ClientErrorReports.h). The typed-confirmation
    // one quotes the node's name, so reporting it opened a fresh error group —
    // and pinged every administrator — for each node whose name was mistyped.
    if (m_nodeDeleteInProgress) {
        showLocalNotice(m_nodesStatus,
                        QStringLiteral("Deletion of \"%1\" is already in progress.")
                            .arg(m_nodeDeleteTarget),
                        true, forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (!m_isAdmin) {
        showLocalNotice(m_nodesStatus,
                        QStringLiteral(
                            "Only a platform admin can delete a node from the mesh."),
                        true, forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const bool canSign = m_profileIdentity.isValid() &&
                         hasOwnerSigningCapability(accountOwner());
    if (activeAccountSessionToken().isEmpty() && !canSign) {
        showLocalNotice(m_nodesStatus,
                        QStringLiteral(
                            "Sign in to your admin account from Settings first: "
                            "deleting a node needs either an account session or "
                            "this desktop's account key."),
                        true, forkmesh::kLocalOperationNoticeKind);
        return;
    }

    const QString required = QStringLiteral("DELETE ") + target;
    bool accepted = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Delete node"),
        QString::fromUtf8(
            "This permanently deletes \"%1\" everywhere:\n\n"
            "\xE2\x80\xA2 its Vultr server is destroyed and billing stops\n"
            "\xE2\x80\xA2 its DNS record is removed\n"
            "\xE2\x80\xA2 its account, mirrors, agent jobs and status-page "
            "history are erased from the relay\n"
            "\xE2\x80\xA2 the saved SSH host is forgotten on this machine, and "
            "the git remotes that pushed to it are removed\n\n"
            "This cannot be undone. Type %2 to confirm.")
            .arg(target, required),
        QLineEdit::Normal, QString(), &accepted);
    if (!accepted)
        return;
    if (typed.trimmed() != required) {
        const QString message =
            QStringLiteral(
                "Node was not deleted. Click Delete again, then type \"%1\" "
                "exactly, including uppercase DELETE, to confirm.")
                .arg(required);
        showLocalNotice(m_nodesStatus, message, true,
                        forkmesh::kLocalOperationNoticeKind);
        return;
    }

    m_nodeDeleteInProgress = true;
    m_nodeDeleteTarget = target;
    refreshNodeActionButtons();
    setNodeDeleteStatus(
        QString::fromUtf8("1/3 \xE2\x80\x94 Destroying the server for "
                          "\"%1\"\xE2\x80\xA6")
            .arg(target));
    fileMessage(QStringLiteral("Deleting node \"%1\". Waiting for the relay to confirm it.")
                    .arg(target));
    destroyVultrServerForNode(target, [this, target, nodeId](QString outcome) {
        if (!outcome.isEmpty())
            setNodeDeleteStatus(outcome);
        setNodeDeleteStatus(
            QString::fromUtf8("2/3 \xE2\x80\x94 Removing DNS for "
                              "\"%1\"\xE2\x80\xA6")
                .arg(target));
        removeVultrMirrorDns(target, [this, target, nodeId](QString dnsOutcome) {
            if (!dnsOutcome.isEmpty())
                setNodeDeleteStatus(dnsOutcome);
            setNodeDeleteStatus(
                QString::fromUtf8("3/3 \xE2\x80\x94 Removing \"%1\" from "
                                  "the mesh\xE2\x80\xA6")
                    .arg(target));
            sendMeshNodeDeleteRequest(target, nodeId);
        });
    });
}

void MainWindow::destroyVultrServerForNode(
    const QString &node, std::function<void(QString outcome)> onDone)
{
    if (!m_networkAccess) {
        onDone(QStringLiteral("No network access — skipped the Vultr server."));
        return;
    }
    QString apiKey =
        m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString();
    if (apiKey.isEmpty()) {
        apiKey = forkmesh::control::vultrApiKeyFromVariables(
            ActionStore::variables());
    }
    if (apiKey.isEmpty()) {
        onDone(QString::fromUtf8(
            "No Vultr API key is available (store one as VULTR_API_KEY), so "
            "no server was destroyed for \"%1\" \xE2\x80\x94 continuing with "
            "the mesh removal.").arg(node));
        return;
    }

    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QString recorded;
    QString address;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("name")).toString().trimmed().compare(
                node, Qt::CaseInsensitive) != 0)
            continue;
        recorded = forkmesh::control::savedHostVultrInstanceId(host);
        address = host.value(QStringLiteral("ip")).toString().trimmed();
        break;
    }
    if (!recorded.isEmpty()) {
        sendNodeVultrDestroy(apiKey, recorded, node, std::move(onDone));
        return;
    }
    vultrApiCall(
        apiKey, QStringLiteral("/v2/instances?per_page=500"),
        QByteArrayLiteral("GET"), {},
        [this, apiKey, node, address, onDone](QJsonObject result,
                                              QString error) mutable {
            if (!error.isEmpty()) {
                onDone(QString::fromUtf8(
                    "Could not reach Vultr to destroy \"%1\" (%2) \xE2\x80\x94 "
                    "continuing with the mesh removal.").arg(node, error));
                return;
            }
            const QJsonArray instances =
                result.value(QStringLiteral("instances")).toArray();
            QString instanceId =
                forkmesh::control::vultrInstanceIdForAddress(instances, node);
            if (instanceId.isEmpty() && !address.isEmpty()) {
                instanceId = forkmesh::control::vultrInstanceIdForAddress(
                    instances, address);
            }
            if (instanceId.isEmpty()) {
                onDone(QString::fromUtf8(
                    "No single Vultr instance matches \"%1\", so no server was "
                    "destroyed \xE2\x80\x94 continuing with the mesh removal.")
                           .arg(node));
                return;
            }
            sendNodeVultrDestroy(apiKey, instanceId, node, onDone);
        });
}

void MainWindow::sendNodeVultrDestroy(
    const QString &apiKey, const QString &instanceId, const QString &node,
    std::function<void(QString outcome)> onDone)
{
    const QString invalid =
        forkmesh::control::validateVultrDestroyRequest(apiKey, instanceId);
    if (!invalid.isEmpty()) {
        onDone(invalid);
        return;
    }
    setNodeDeleteStatus(
        QString::fromUtf8("1/3 \xE2\x80\x94 Destroying the Vultr server "
                          "behind \"%1\"\xE2\x80\xA6")
            .arg(node));
    vultrApiCall(
        apiKey, QStringLiteral("/v2/instances/") + instanceId,
        QByteArrayLiteral("DELETE"), {},
        [node, instanceId, onDone](QJsonObject, QString error) mutable {
            if (!error.isEmpty()) {
                onDone(QString::fromUtf8(
                    "Vultr refused to destroy %1 (%2) \xE2\x80\x94 continuing "
                    "with the mesh removal.").arg(instanceId, error));
                return;
            }
            onDone(QString::fromUtf8("Destroyed Vultr instance %1 (\"%2\").")
                       .arg(instanceId, node));
        });
}

void MainWindow::removeVultrMirrorDns(
    const QString &node, std::function<void(QString outcome)> onDone)
{
    auto skip = [onDone](const QString &reason) {
        onDone(QStringLiteral("Left the DNS record alone: ") + reason);
    };
    if (!m_networkAccess) {
        skip(QStringLiteral("network access is unavailable"));
        return;
    }
    const QMap<QString, QString> variables = ActionStore::variables();
    const QString apiToken =
        forkmesh::control::cloudflareApiTokenFromVariables(variables);
    if (apiToken.isEmpty()) {
        skip(QStringLiteral("no CLOUDFLARE_API_TOKEN device variable"));
        return;
    }
    QSettings settings;
    QString zone = settings.value(QStringLiteral("control/cloudflareZone"))
                       .toString()
                       .trimmed();
    if (zone.isEmpty())
        zone = forkmesh::control::cloudflareZoneNameFromVariables(variables);
    const QString hostname =
        forkmesh::control::vultrMirrorDnsHostname(node, zone);
    if (hostname.isEmpty()) {
        skip(zone.isEmpty() ? QStringLiteral("no Cloudflare zone is configured")
                            : QStringLiteral("\"%1\" and \"%2\" do not form a "
                                             "valid hostname")
                                  .arg(node, zone));
        return;
    }
    cloudflareApiCall(
        apiToken,
        QStringLiteral("/zones?name=%1")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(zone))),
        QByteArrayLiteral("GET"), {},
        [this, apiToken, zone, hostname, skip, onDone](QJsonObject result,
                                                       QString error) mutable {
            if (!error.isEmpty()) {
                skip(error);
                return;
            }
            const QString zoneId = forkmesh::control::cloudflareZoneId(
                result.value(QStringLiteral("result")).toArray(), zone);
            if (zoneId.isEmpty()) {
                skip(QStringLiteral(
                         "this API token does not see exactly one \"%1\" zone")
                         .arg(zone));
                return;
            }
            cloudflareApiCall(
                apiToken,
                QStringLiteral("/zones/%1/dns_records?type=A&name=%2")
                    .arg(zoneId, QString::fromLatin1(
                                     QUrl::toPercentEncoding(hostname))),
                QByteArrayLiteral("GET"), {},
                [this, apiToken, zoneId, hostname, skip, onDone](
                    QJsonObject existing, QString listError) mutable {
                    if (!listError.isEmpty()) {
                        skip(listError);
                        return;
                    }
                    const QString recordId =
                        forkmesh::control::cloudflareDnsRecordId(
                            existing.value(QStringLiteral("result")).toArray(),
                            hostname, QStringLiteral("A"));
                    if (recordId.isEmpty()) {
                        onDone(QStringLiteral("No %1 DNS record to remove.")
                                   .arg(hostname));
                        return;
                    }
                    cloudflareApiCall(
                        apiToken,
                        QStringLiteral("/zones/%1/dns_records/%2")
                            .arg(zoneId, recordId),
                        QByteArrayLiteral("DELETE"), {},
                        [hostname, skip, onDone](QJsonObject,
                                                 QString deleteError) mutable {
                            if (!deleteError.isEmpty()) {
                                skip(deleteError);
                                return;
                            }
                            onDone(QStringLiteral("Removed the %1 DNS record.")
                                       .arg(hostname));
                        });
                });
        });
}

void MainWindow::sendMeshNodeDeleteRequest(const QString &node,
                                           const QString &nodeId)
{
    if (!m_networkAccess) {
        const QString message = QStringLiteral(
            "Could not delete \"%1\": network access is unavailable.").arg(node);
        m_nodeDeleteInProgress = false;
        m_nodeDeleteTarget.clear();
        setNodeDeleteStatus(message);
        flashMessage(message, true);
        refreshNodeActionButtons();
        return;
    }
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/world/admin/nodes/delete"));
    url.setQuery(QString());
    QNetworkRequest request;
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QString sessionToken = accountSessionTokenForUrl(url);
    if (!sessionToken.isEmpty()) {
        request.setRawHeader(
            "Authorization",
            QByteArrayLiteral("Bearer ") + sessionToken.toUtf8());
    } else {
        // The ordinary launch is authenticateSilently(): this install owns the
        // account's key and holds no session token, so sign the proof the relay
        // accepts for this one deletion. It names the target node, so it cannot
        // be replayed to delete a different one. Must match
        // _world_node_delete_signed_actor's canonical string byte for byte.
        const QString actor = accountOwner().trimmed().toLower();
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString canonical =
            QStringLiteral("forkmesh-world-node-delete-v1\n") + actor +
            QLatin1Char('\n') + node + QLatin1Char('\n') + ts;
        const QString sig = m_profileIdentity.signData(canonical.toUtf8());
        if (actor.isEmpty() || sig.isEmpty()) {
            const QString message = QString::fromUtf8(
                "Could not sign the deletion of \"%1\" with this desktop's "
                "account key.").arg(node);
            m_nodeDeleteInProgress = false;
            m_nodeDeleteTarget.clear();
            setNodeDeleteStatus(message);
            flashMessage(message, true);
            refreshNodeActionButtons();
            return;
        }
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("node"), actor);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"), sig);
        url.setQuery(query);
    }
    request.setUrl(url);

    const QJsonObject body{
        {QStringLiteral("nodeName"), node},
        {QStringLiteral("nodeId"), nodeId},
        {QStringLiteral("confirmation"), QStringLiteral("DELETE ") + node},
    };
    setNodeDeleteStatus(
        QString::fromUtf8("3/3 \xE2\x80\x94 Removing \"%1\" from the "
                          "mesh\xE2\x80\xA6")
            .arg(node));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, node] {
        const QByteArray payload = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        const QString transportError = reply->errorString();
        reply->deleteLater();
        const QJsonObject response = QJsonDocument::fromJson(payload).object();
        if (!ok || !response.value(QStringLiteral("ok")).toBool()) {
            QString detail =
                response.value(QStringLiteral("error")).toString().trimmed();
            if (detail.isEmpty())
                detail = transportError;
            const QString message = QString::fromUtf8(
                "Could not delete \"%1\": %2").arg(node, detail);
            m_nodeDeleteInProgress = false;
            m_nodeDeleteTarget.clear();
            setNodeDeleteStatus(message);
            flashMessage(message, true);
            refreshNodeActionButtons();
            return;
        }
        const QString deletedName = response.value(QStringLiteral("nodeDeleted"))
                                        .toString().trimmed().toLower();
        QSet<QString> deletedNames;
        for (const QJsonValue &value :
             response.value(QStringLiteral("identifiers")).toArray()) {
            const QString identifier = value.toString().trimmed().toLower();
            if (!identifier.isEmpty())
                deletedNames.insert(identifier);
        }
        if (!deletedName.isEmpty())
            deletedNames.insert(deletedName);
        if (deletedName.isEmpty() || !deletedNames.contains(node)) {
            const QString message = QStringLiteral(
                "The relay did not confirm deletion of \"%1\"; it remains visible.")
                                        .arg(node);
            m_nodeDeleteInProgress = false;
            m_nodeDeleteTarget.clear();
            setNodeDeleteStatus(message);
            flashMessage(message, true);
            refreshNodeActionButtons();
            return;
        }
        // Before forgetSavedHostNamed: the saved host maps this node to the
        // address a remote may name it by.
        for (const QString &deleted : std::as_const(deletedNames))
            removeSshMirrorRemotesForNode(deleted);
        forgetSavedHostNamed(node);
        m_deletedMeshNodeNames.unite(deletedNames);
        for (const QString &deleted : std::as_const(deletedNames)) {
            // The relay directory/catalog can keep serving its older snapshot
            // after this process exits. Persist every relay-confirmed alias so
            // a restart cannot resurrect the deleted row; a genuinely new
            // catalog publication under that name clears the suppression.
            recordDeletedMeshNode(deleted);
            // The Mirror nodes page has its own stale relay sources and its
            // own ledger; suppress the confirmed aliases there as well.
            recordDecommissionedMirrorNode(deleted);
            m_relayOnlineNodes.remove(deleted);
            m_nodesCatalogInfo.remove(deleted);
        }
        for (int i = m_nodeMenuEntries.size() - 1; i >= 0; --i) {
            if (deletedNames.contains(
                    m_nodeMenuEntries.at(i).name.trimmed().toLower()))
                m_nodeMenuEntries.removeAt(i);
        }
        m_homeRoster.removeIf([&deletedNames](const MemberInfo &member) {
            return deletedNames.contains(
                nodeListIdentityKey(member).trimmed().toLower());
        });
        for (MemberInfo &user : m_chatDirectoryUsers) {
            QStringList nodes = user.nodeName.split(QStringLiteral(", "),
                                                    Qt::SkipEmptyParts);
            nodes.removeIf([&deletedNames](const QString &name) {
                return deletedNames.contains(name.trimmed().toLower());
            });
            user.nodeName = nodes.join(QStringLiteral(", "));
        }
        m_relayOnlineNodesFetchedMs = 0;
        m_nodesCatalogFetchedMs = 0;
        m_chatDirectoryFetchedMs = 0;
        m_nodeDeleteInProgress = false;
        m_nodeDeleteTarget.clear();
        refreshChatUserDirectory();
        fetchRelayOnlineNodes(true);
        fetchNodesCatalogInfo(true);
        refreshNodesTable();
        refreshNodeActionButtons();
        const bool alreadyAbsent =
            response.value(QStringLiteral("alreadyAbsent")).toBool();
        const QString message = alreadyAbsent
            ? QStringLiteral("Relay confirmed \"%1\" was already absent; removed its stale local entry.")
                  .arg(node)
            : QStringLiteral("3/3 complete — Relay confirmed deletion of \"%1\"; it has been removed from this list.")
                  .arg(node);
        setNodeDeleteStatus(message);
        flashMessage(message);
    });
}

void MainWindow::forgetSavedHostNamed(const QString &node)
{
    QSettings settings;
    QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    bool changed = false;
    for (int i = hosts.size() - 1; i >= 0; --i) {
        const QJsonObject host = hosts.at(i).toObject();
        if (host.value(QStringLiteral("name")).toString().trimmed().compare(
                node, Qt::CaseInsensitive) != 0)
            continue;
        const QString credentialKey = forkmesh::control::savedHostCredentialKey(
            host.value(QStringLiteral("name")).toString(),
            host.value(QStringLiteral("ip")).toString(),
            host.value(QStringLiteral("user")).toString());
        QString oldPassword = m_hostSessionPasswords.take(credentialKey);
        oldPassword.fill(QChar::Null);
        hosts.removeAt(i);
        changed = true;
    }
    if (!changed)
        return;
    forkmesh::control::saveSavedHosts(settings, kHostsSetting, hosts);
    // Automatic fleet scale-down lands here, not in forgetHostAtRow: tombstone
    // the node so the Mirror nodes panel drops it now instead of listing a
    // destroyed server for the rest of its relay freshness lease.
    recordDecommissionedMirrorNode(node);
    refreshHostsTable();
}

QWidget *MainWindow::buildHomeSection()
{
    auto *page = new QWidget;
    auto *empty = new QLabel(
        QStringLiteral("Select a repository to explore its code and activity."));
    empty->setObjectName(QStringLiteral("emptyState"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    m_repoDetailSection = empty;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_repoDetailSection);
    return page;
}

static QPushButton *makeProfileActionButton(const QString &icon, const QString &label,
                                            const QString &tooltip)
{
    auto *button = new QPushButton(label);
    button->setObjectName("profileActionButton");
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tooltip);
    button->setFixedHeight(36);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setOcticon(button, icon, 16);
    return button;
}

static QLabel *makeProfileSection(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName("sectionLabel");
    return label;
}

static QString withSoftBreaks(const QString &text, int chunkSize = 4)
{
    QString out;
    out.reserve(text.size() + text.size() / chunkSize);
    for (int i = 0; i < text.size(); ++i) {
        out += text.at(i);
        if ((i + 1) % chunkSize == 0 && i + 1 < text.size())
            out += QChar(0x200B);
    }
    return out;
}

static void fillProfileNextDetails(QFrame *list,
                                   const QList<QPair<QString, QString>> &rows,
                                   bool online)
{
    QLayout *layout = list->layout();
    while (QLayoutItem *old = layout->takeAt(0)) {
        if (QWidget *widget = old->widget())
            widget->deleteLater();
        delete old;
    }
    for (int index = 0; index < rows.size(); ++index) {
        const QString &key = rows.at(index).first;
        const QString &value = rows.at(index).second;
        auto *row = new QWidget(list);
        row->setObjectName(QStringLiteral("profileNextDetailRow"));
        row->setAttribute(Qt::WA_StyledBackground, true);
        row->setProperty("profileNextLast", index + 1 == rows.size());
        row->setMinimumHeight(44);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(20, 0, 20, 0);
        line->setSpacing(8);
        auto *keyLabel = new QLabel(key, row);
        keyLabel->setObjectName(QStringLiteral("profileNextDetailKey"));
        keyLabel->setTextFormat(Qt::PlainText);
        keyLabel->setFixedWidth(180);
        line->addWidget(keyLabel);
        if (key == QLatin1String("Status")) {
            auto *dot = new QLabel(row);
            dot->setFixedSize(6, 6);
            dot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 3px;")
                                   .arg(Theme::hex(online ? Theme::Role::Success
                                                          : Theme::Role::FgSubtle)));
            line->addWidget(dot, 0, Qt::AlignVCenter);
        }
        auto *valueLabel = new QLabel(value, row);
        valueLabel->setObjectName(QStringLiteral("profileNextDetailValue"));
        valueLabel->setTextFormat(Qt::PlainText);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (key == QLatin1String("Version") || key == QLatin1String("Key")) {
            QFont mono = valueLabel->font();
            mono.setFamily(Theme::monoFontFamily());
            mono.setPixelSize(13);
            valueLabel->setFont(mono);
        }
        line->addWidget(valueLabel, 1);
        layout->addWidget(row);
    }
    list->setVisible(!rows.isEmpty());
}

QWidget *MainWindow::buildNodeProfileSection()
{
    auto *page = new QWidget;
    auto *outer = new QHBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addStretch(1);
    outer->addWidget(buildNodeProfilePanel(), 0);
    outer->addStretch(1);
    m_nodeProfileSectionHost = page;
    return page;
}

QWidget *MainWindow::buildNodeProfilePanel()
{
    auto *scroll = new QScrollArea;
    scroll->setObjectName("nodeProfilePanel");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(920);
    scroll->setMaximumWidth(1440);
    m_nodeProfilePanel = scroll;

    auto *content = new QWidget;
    content->setObjectName("nodeProfileContent");

    auto *closeButton = new QPushButton(QString());
    closeButton->setObjectName("ghostButton");
    closeButton->setCursor(Qt::PointingHandCursor);
    closeButton->setToolTip("Close");
    setOcticon(closeButton, "x", 16);
    connect(closeButton, &QPushButton::clicked, this, &MainWindow::hideNodeProfile);
    m_profileCloseButton = closeButton;
    auto *titleLabel = new QLabel("User profile");
    titleLabel->setObjectName("sectionLabel");
    auto *topRow = new QHBoxLayout;
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->addWidget(titleLabel);
    topRow->addStretch();
    topRow->addWidget(closeButton);

    m_profileAvatar = new QLabel;
    m_profileAvatar->setObjectName("profileBanner");
    m_profileAvatar->setFixedSize(96, 96);
    m_profileAvatar->setAlignment(Qt::AlignCenter);

    m_profileName = new QLabel;
    m_profileName->setObjectName("profileName");
    m_profileName->setAlignment(Qt::AlignHCenter);
    m_profileName->setWordWrap(true);
    m_profileStatus = new QLabel;
    m_profileStatus->setObjectName("statusLine");
    m_profileStatus->setAlignment(Qt::AlignHCenter);
    m_profileStatus->setTextFormat(Qt::RichText);
    m_profileNote = new QLabel;
    m_profileNote->setObjectName("statusLine");
    m_profileNote->setAlignment(Qt::AlignHCenter);
    m_profileNote->setWordWrap(true);

    m_profileMessageButton = new QPushButton("Message");
    m_profileMessageButton->setObjectName("ghostButton");
    m_profileMessageButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileMessageButton, "comment", 16);
    connect(m_profileMessageButton, &QPushButton::clicked, this, [this] {
        QString account;
        for (const MemberInfo &member : std::as_const(m_homeRoster)) {
            if (member.id != m_profileNodeId)
                continue;
            account = member.ownerUser.trimmed();
            break;
        }
        if (account.isEmpty())
            account = verifiedChatAccount(m_profileNodeId, QString());
        startDirectMessageWith(account, QString(), QString());
    });

    // Admin-only: request ownership of someone else's node. This
    // only parks a pending marker on the target — the transfer only completes
    // once that node's own owner approves the prompt it gets on its own
    // heartbeat, so a hostile/compromised admin account still can't silently
    // seize a node.
    m_profileTakeOwnershipButton = new QPushButton("Take ownership");
    m_profileTakeOwnershipButton->setObjectName("ghostButton");
    m_profileTakeOwnershipButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileTakeOwnershipButton, "shield-check", 16);
    m_profileTakeOwnershipButton->setToolTip(
        "Request ownership of this node. It only transfers once the node's "
        "current owner approves the confirmation prompt it receives.");
    connect(m_profileTakeOwnershipButton, &QPushButton::clicked, this,
            &MainWindow::requestNodeOwnership);

    m_profileGetPaidButton = new QPushButton(
        QString::fromUtf8("Mirror reward settings"));
    m_profileGetPaidButton->setObjectName("primaryButton");
    m_profileGetPaidButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileGetPaidButton, "credit-card", 16);
    m_profileGetPaidButton->setToolTip(
        "Configure a public self-custodial payout address. A healthy mirror may "
        "be eligible for voluntary community rewards; selection and payment are "
        "not guaranteed. Cloning, mirroring, issues and PRs work without it.");
    connect(m_profileGetPaidButton, &QPushButton::clicked, this,
            &MainWindow::enablePaidMirroring);

    m_profileOnlineSection = new QWidget;
    auto *onlineSectionLabel = makeProfileSection("THIS MACHINE'S POWER SWITCH");
    auto *nodeOnlineSwitch = new ToggleSwitch;
    m_nodeOnlineToggle = nodeOnlineSwitch;
    connect(nodeOnlineSwitch, &QAbstractButton::clicked, this,
            [this](bool checked) { setNodeOffline(!checked); });
    m_nodeOnlineStatusLabel = new QLabel;
    m_nodeRewardStatus = new QLabel;
    m_nodeRewardStatus->setObjectName("nodeRewardStatus");
    m_nodeRewardStatus->setWordWrap(true);
    m_nodeUptimeLabel = new QLabel;
    m_nodeUptimeLabel->setObjectName("nodeUptimeLabel");
    m_nodeUptimeLabel->setStyleSheet(Theme::remapSheet(
        QStringLiteral("color:#8b949e; font-size:10px; font-weight:600;")));
    m_nodeUptimeLabel->setToolTip(
        QStringLiteral("How long this machine has been online this session"));
    auto *onlineStatusColumn = new QVBoxLayout;
    onlineStatusColumn->setContentsMargins(0, 0, 0, 0);
    onlineStatusColumn->setSpacing(0);
    onlineStatusColumn->addWidget(m_nodeOnlineStatusLabel);
    onlineStatusColumn->addWidget(m_nodeRewardStatus);
    onlineStatusColumn->addWidget(m_nodeUptimeLabel);
    auto *onlineSwitchRow = new QHBoxLayout;
    onlineSwitchRow->setContentsMargins(0, 0, 0, 0);
    onlineSwitchRow->setSpacing(10);
    onlineSwitchRow->addWidget(nodeOnlineSwitch);
    onlineSwitchRow->addLayout(onlineStatusColumn, 1);
    auto *onlineSectionLayout = new QVBoxLayout(m_profileOnlineSection);
    onlineSectionLayout->setContentsMargins(0, 0, 0, 0);
    onlineSectionLayout->setSpacing(6);
    onlineSectionLayout->addWidget(onlineSectionLabel);
    onlineSectionLayout->addLayout(onlineSwitchRow);

    m_profileSelfActions = new QWidget;
    auto *selfLabel = makeProfileSection("THIS MACHINE");
    m_profileRebuildButton = makeProfileActionButton(
        "sync", "Rebuild",
        "Rebuild from the local source checkout and relaunch (fast; no update); "
        "click again to cancel");
    connect(m_profileRebuildButton, &QPushButton::clicked, this, [this] {
        if (cancelRestartIfRunning())
            return;
        startRestartSpin(m_profileRebuildButton);
        quickRebuildRestart();
    });
    m_profileUpdateButton = makeProfileActionButton(
        "download", "Update",
        "Pull the latest source, then rebuild and relaunch; click again to "
        "cancel");
    connect(m_profileUpdateButton, &QPushButton::clicked, this, [this] {
        if (cancelRestartIfRunning())
            return;
        startRestartSpin(m_profileUpdateButton, RestartKind::Update);
        updateRebuildRestart();
    });
    auto *selfAddRepoButton = makeProfileActionButton(
        "file-directory", "Add repo",
        "Choose a local Git repository on this computer, mirror it, and publish "
        "it as public or private");
    connect(selfAddRepoButton, &QPushButton::clicked, this,
            &MainWindow::promptAddRepository);
    auto *selfSettingsButton = makeProfileActionButton("gear", "Settings",
                                                        "Open settings");
    m_profileSettingsButton = selfSettingsButton;
    connect(selfSettingsButton, &QPushButton::clicked, this,
            [this] { showSection(1); });
    auto *selfEditProfileButton = makeProfileActionButton(
        "pencil", "Edit profile",
        "Edit the bio, location, time zone, Mastodon handle and privacy "
        "switches on your public profile");
    connect(selfEditProfileButton, &QPushButton::clicked, this,
            &MainWindow::showEditAccountProfileDialog);
    // Two buttons in the app said only "Logout"/"Log out" while doing very
    // different things. This one disconnects the mesh session and goes back to
    // the setup screen; the account stays signed in on this machine. Settings
    // holds the other one, which signs the account out. Name each for what it
    // actually does.
    auto *selfLogoutButton = makeProfileActionButton(
        "sign-out", "Disconnect",
        "Disconnect this machine from the mesh and return to the setup "
        "screen. Your ForkMesh account stays signed in here \xE2\x80\x94 to "
        "sign the account out, use Settings \xE2\x80\xBA \"Log out of "
        "account\".");
    connect(selfLogoutButton, &QPushButton::clicked, this,
            [this] { leaveSession(); });
    m_profileActionGrid = new QGridLayout;
    m_profileActionGrid->setContentsMargins(0, 0, 0, 0);
    m_profileActionGrid->setSpacing(6);
    m_profileActionButtons = {m_profileRebuildButton, m_profileUpdateButton,
                              selfAddRepoButton,      selfEditProfileButton,
                              selfSettingsButton,     selfLogoutButton};
    layoutProfileActions(false);
    auto *selfLayout = new QVBoxLayout(m_profileSelfActions);
    selfLayout->setContentsMargins(0, 0, 0, 0);
    selfLayout->setSpacing(6);
    selfLayout->addWidget(selfLabel);
    selfLayout->addLayout(m_profileActionGrid);

    m_profileStatGrid = new QWidget;
    auto makeTile = [](QLabel *&tile) {
        tile = new QLabel;
        tile->setObjectName("statTile");
        tile->setTextFormat(Qt::RichText);
        tile->setAlignment(Qt::AlignCenter);
        tile->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    };
    makeTile(m_profileTileRepos);
    makeTile(m_profileTileMirrored);
    makeTile(m_profileTileOnline);
    makeTile(m_profileTileChats);
    auto *tileRow = new QHBoxLayout(m_profileStatGrid);
    tileRow->setContentsMargins(0, 0, 0, 0);
    tileRow->setSpacing(6);
    tileRow->addWidget(m_profileTileRepos);
    tileRow->addWidget(m_profileTileMirrored);
    tileRow->addWidget(m_profileTileOnline);
    tileRow->addWidget(m_profileTileChats);

    auto *detailsLabel = makeProfileSection("DETAILS");
    m_profileDetails = new QLabel;
    m_profileDetails->setObjectName("profileCard");
    m_profileDetails->setTextFormat(Qt::RichText);
    m_profileDetails->setWordWrap(true);
    m_profileDetails->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_profileMirrorsLabel = makeProfileSection("MIRRORS");
    m_profileMirrors = new QLabel;
    m_profileMirrors->setObjectName("profileCard");
    m_profileMirrors->setTextFormat(Qt::RichText);
    m_profileMirrors->setWordWrap(true);
    m_profileMirrors->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_profileAccountSection = new QWidget;
    m_profileAccountSection->setObjectName("profileUserCard");
    m_profileAccountLabel = makeProfileSection("NODES");
    m_profileAccountStatus = new QLabel;
    m_profileAccountStatus->setObjectName("statusLine");
    m_profileAccountStatus->setWordWrap(true);
    m_profileAccountStatus->setTextFormat(Qt::RichText);
    m_profileUserNodesList = new QListWidget;
    m_profileUserNodesList->setObjectName("profileNodeList");
    m_profileUserNodesList->setIconSize(QSize(32, 32));
    m_profileUserNodesList->setSelectionMode(QAbstractItemView::NoSelection);
    m_profileUserNodesList->setFocusPolicy(Qt::NoFocus);
    m_profileUserNodesList->setFrameShape(QFrame::NoFrame);
    m_profileUserNodesList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_profileUserNodesList->setCursor(Qt::PointingHandCursor);
    m_profileUserNodesList->setToolTip(
        QStringLiteral("Nodes owned by this user account. Click one to open "
                       "its node profile."));
    connect(m_profileUserNodesList, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) {
                const QString node = item->data(Qt::UserRole).toString();
                if (!node.isEmpty())
                    showNodeProfile(QString(), node);
            });
    m_profileLinkUserButton = new QPushButton("Log in as a user");
    m_profileLinkUserButton->setObjectName("ghostButton");
    m_profileLinkUserButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileLinkUserButton, "person", 14);
    m_profileLinkUserButton->setToolTip(
        "Link this machine's node to your user account by signing in. One user "
        "can own many nodes.");
    connect(m_profileLinkUserButton, &QPushButton::clicked, this,
            &MainWindow::promptLinkNodeToUser);
    auto *accountLayout = new QVBoxLayout(m_profileAccountSection);
    accountLayout->setContentsMargins(10, 10, 10, 10);
    accountLayout->setSpacing(6);
    accountLayout->addWidget(m_profileAccountLabel);
    accountLayout->addWidget(m_profileAccountStatus);
    accountLayout->addWidget(m_profileUserNodesList);
    accountLayout->addWidget(m_profileLinkUserButton, 0, Qt::AlignLeft);

    m_profileHostingLabel = makeProfileSection("LOCAL REPOSITORIES");
    m_profileHosting = new QLabel;
    m_profileHosting->setObjectName("profileCard");
    m_profileHosting->setWordWrap(true);
    m_profileHosting->setTextFormat(Qt::RichText);
    m_profileHosting->setTextInteractionFlags(Qt::TextSelectableByMouse);

    // --- Node ID = the node's Ed25519 public key. Selectable so it can be copied.
    auto *nodeKeyLabel = makeProfileSection("NODE ID (PUBLIC KEY)");
    m_profileNodeKey = new QLabel;
    m_profileNodeKey->setObjectName("profileMono");
    m_profileNodeKey->setWordWrap(true);
    m_profileNodeKey->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *copyKey = new QPushButton("Copy node ID");
    copyKey->setObjectName("ghostButton");
    copyKey->setCursor(Qt::PointingHandCursor);
    setOcticon(copyKey, "copy", 14);
    connect(copyKey, &QPushButton::clicked, this, [this] {
        if (!m_profileNodeId.isEmpty()) {
            QApplication::clipboard()->setText(m_profileNodeId);
            logSystem("Copied node ID to clipboard.");
        }
    });
    // Browser-based linking: opens a node-signed grant URL in the
    // default browser so the user logged in on forkmesh.com takes ownership of
    // this node without typing anything.
    m_profileLinkBrowserButton =
        new QPushButton("Link this machine to your account");
    m_profileLinkBrowserButton->setObjectName("ghostButton");
    m_profileLinkBrowserButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileLinkBrowserButton, "link", 14);
    m_profileLinkBrowserButton->setToolTip(
        "Open forkmesh.com in your browser and attach this machine's node to "
        "the user account you're logged in as there. One user can own many "
        "nodes.");
    connect(m_profileLinkBrowserButton, &QPushButton::clicked, this,
            &MainWindow::openLinkNodeInBrowser);

    m_profileSolanaSection = new QWidget;
    auto *solanaLabel = makeProfileSection("SOLANA");
    auto *custodyNotice = new QLabel(
        "Non-custodial payout address \xE2\x80\x94 ForkMesh reads the public "
        "address and on-chain balance only. Private keys stay in the external "
        "wallet you control.");
    custodyNotice->setObjectName("statusLine");
    custodyNotice->setWordWrap(true);
    m_profileSolanaAddr = new QLabel;
    m_profileSolanaAddr->setObjectName("profileMono");
    m_profileSolanaAddr->setWordWrap(true);
    m_profileSolanaAddr->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *copyAddr = new QPushButton("Copy address");
    copyAddr->setObjectName("ghostButton");
    copyAddr->setCursor(Qt::PointingHandCursor);
    setOcticon(copyAddr, "copy", 14);
    connect(copyAddr, &QPushButton::clicked, this, [this] {
        if (!m_profileSolanaValue.isEmpty()) {
            QApplication::clipboard()->setText(m_profileSolanaValue);
            logSystem("Copied Solana address to clipboard.");
        }
    });
    m_profileQr = new QLabel;
    m_profileQr->setObjectName("profileQr");
    m_profileQr->setAlignment(Qt::AlignCenter);

    m_profileBalance = new QLabel("\xE2\x80\x94"); // em dash until checked
    m_profileBalance->setObjectName("channelTitle");
    m_profileBalanceButton = new QPushButton("Check balance");
    m_profileBalanceButton->setObjectName("ghostButton");
    m_profileBalanceButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileBalanceButton, "sync", 14);
    m_profileBalanceButton->setToolTip(
        "Query the Solana network through public JSON-RPC for this wallet's "
        "balance. This sends the address to the endpoint it connects to.");
    connect(m_profileBalanceButton, &QPushButton::clicked, this,
            &MainWindow::checkNodeBalance);
    addRefreshSpin(m_profileBalanceButton);
    auto *balanceRow = new QHBoxLayout;
    balanceRow->setContentsMargins(0, 0, 0, 0);
    balanceRow->addWidget(m_profileBalance, 1);
    balanceRow->addWidget(m_profileBalanceButton);

    // Tipping. On someone else's profile this is the send button; on your own
    // it is where the machine-local wallet gets linked. The key never leaves
    // this desktop — see MainWindowTips.cpp.
    m_profileTipButton = new QPushButton("Send tip");
    m_profileTipButton->setObjectName("ghostButton");
    m_profileTipButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileTipButton, "credit-card", 14);
    m_profileTipButton->setVisible(false);
    connect(m_profileTipButton, &QPushButton::clicked, this,
            &MainWindow::sendTipToProfileMember);

    m_profileTipWalletSection = new QWidget;
    m_profileTipWalletLabel = new QLabel;
    m_profileTipWalletLabel->setObjectName("statusLine");
    m_profileTipWalletLabel->setWordWrap(true);
    m_profileTipWalletLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_profileTipLinkButton = new QPushButton("Link a wallet\xE2\x80\xA6");
    m_profileTipLinkButton->setObjectName("ghostButton");
    m_profileTipLinkButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileTipLinkButton, "key", 14);
    m_profileTipLinkButton->setToolTip(
        "Generate a new Solana wallet, or import an existing secret key, into "
        "an encrypted owner-only vault on this machine so you can tip other "
        "members. The key is never uploaded.");
    connect(m_profileTipLinkButton, &QPushButton::clicked, this,
            &MainWindow::linkTipWallet);
    m_profileTipExportButton = new QPushButton("Export secret key\xE2\x80\xA6");
    m_profileTipExportButton->setObjectName("ghostButton");
    m_profileTipExportButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileTipExportButton, "download", 14);
    m_profileTipExportButton->setVisible(false);
    m_profileTipExportButton->setToolTip(
        "Unlock the vault with your passphrase and show the wallet's secret "
        "key, so you can back it up or manage the same wallet in Phantom or "
        "the Solana CLI.");
    connect(m_profileTipExportButton, &QPushButton::clicked, this,
            &MainWindow::exportTipWalletSecret);
    m_profileTipUnlinkButton = new QPushButton("Remove");
    m_profileTipUnlinkButton->setObjectName("ghostButton");
    m_profileTipUnlinkButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_profileTipUnlinkButton, "trash", 14);
    m_profileTipUnlinkButton->setVisible(false);
    connect(m_profileTipUnlinkButton, &QPushButton::clicked, this,
            &MainWindow::unlinkTipWallet);
    auto *tipWalletButtons = new QHBoxLayout;
    tipWalletButtons->setContentsMargins(0, 0, 0, 0);
    tipWalletButtons->addWidget(m_profileTipLinkButton);
    tipWalletButtons->addWidget(m_profileTipExportButton);
    tipWalletButtons->addWidget(m_profileTipUnlinkButton);
    tipWalletButtons->addStretch(1);
    auto *tipWalletLayout = new QVBoxLayout(m_profileTipWalletSection);
    tipWalletLayout->setContentsMargins(0, 8, 0, 0);
    tipWalletLayout->setSpacing(6);
    tipWalletLayout->addWidget(makeProfileSection("TIP WALLET"));
    tipWalletLayout->addWidget(m_profileTipWalletLabel);
    tipWalletLayout->addLayout(tipWalletButtons);
    m_profileTipWalletSection->setVisible(false);

    auto *solanaLayout = new QVBoxLayout(m_profileSolanaSection);
    solanaLayout->setContentsMargins(0, 8, 0, 0);
    solanaLayout->setSpacing(6);
    solanaLayout->addWidget(solanaLabel);
    solanaLayout->addWidget(custodyNotice);
    solanaLayout->addWidget(m_profileSolanaAddr);
    solanaLayout->addWidget(copyAddr, 0, Qt::AlignLeft);
    solanaLayout->addWidget(m_profileQr, 0, Qt::AlignCenter);
    auto *balLabel = makeProfileSection("BALANCE");
    solanaLayout->addWidget(balLabel);
    solanaLayout->addLayout(balanceRow);
    solanaLayout->addWidget(m_profileTipButton, 0, Qt::AlignLeft);

    m_profileEligibility = new QLabel;
    m_profileEligibility->setObjectName("statusLine");
    m_profileEligibility->setWordWrap(true);
    m_profileEligibility->setTextFormat(Qt::RichText);
    m_profileVerifyButton = new QPushButton("Check reward settings");
    m_profileVerifyButton->setToolTip(
        "Validate the public payout address and publish the existing signed "
        "heartbeat. This never requests a deposit or guarantees a reward.");
    m_profileVerifyButton->setObjectName("ghostButton");
    m_profileVerifyButton->setCursor(Qt::PointingHandCursor);
    connect(m_profileVerifyButton, &QPushButton::clicked, this,
            &MainWindow::verifyWallet);
    solanaLayout->addWidget(m_profileEligibility);
    solanaLayout->addWidget(m_profileVerifyButton, 0, Qt::AlignLeft);

    const bool next = Theme::isNext();
    auto *leftColumn = new QVBoxLayout;
    leftColumn->setContentsMargins(0, 0, 0, 0);
    leftColumn->setSpacing(6);
    QFrame *nextUserCard = nullptr;
    QLabel *nextMachineHeading = nullptr;
    QFrame *nextMachineCard = nullptr;
    QFrame *nextDetails = nullptr;
    if (next) {
        titleLabel->setText(QStringLiteral("USER PROFILE"));
        selfLabel->hide();
        m_profileAvatar->setFixedSize(64, 64);
        m_profileName->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_profileName->setWordWrap(false);
        m_profileStatus->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_profileNote->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        for (QPushButton *button : std::as_const(m_profileActionButtons)) {
            button->setFixedHeight(30);
            button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
            button->setProperty("profileNext", true);
        }
        for (QPushButton *button : {m_profileMessageButton, m_profileTakeOwnershipButton})
            button->setProperty("profileNext", true);
        selfLayout->setAlignment(m_profileActionGrid, Qt::AlignLeft);

        nextUserCard = new QFrame;
        nextUserCard->setObjectName(QStringLiteral("profileNextUserCard"));
        nextUserCard->setAttribute(Qt::WA_StyledBackground, true);
        auto *userRow = new QHBoxLayout(nextUserCard);
        userRow->setContentsMargins(20, 16, 20, 16);
        userRow->setSpacing(20);
        userRow->addWidget(m_profileAvatar, 0, Qt::AlignTop);
        auto *userColumn = new QVBoxLayout;
        userColumn->setContentsMargins(0, 0, 0, 0);
        userColumn->setSpacing(4);
        auto *nameRow = new QHBoxLayout;
        nameRow->setContentsMargins(0, 0, 0, 0);
        nameRow->setSpacing(8);
        nameRow->addWidget(m_profileName);
        auto *youBadge = new kit::Badge(QStringLiteral("you"), kit::Tone::Accent);
        youBadge->setProperty("profileNextYou", true);
        youBadge->hide();
        nameRow->addWidget(youBadge, 0, Qt::AlignVCenter);
        nameRow->addStretch(1);
        userColumn->addLayout(nameRow);
        auto *metaRow = new QHBoxLayout;
        metaRow->setContentsMargins(0, 0, 0, 0);
        metaRow->setSpacing(6);
        metaRow->addWidget(m_profileStatus);
        auto *meta = new QLabel;
        meta->setObjectName(QStringLiteral("profileNextMeta"));
        meta->setTextFormat(Qt::PlainText);
        metaRow->addWidget(meta, 1);
        userColumn->addLayout(metaRow);
        userColumn->addWidget(m_profileNote);
        userColumn->addSpacing(8);
        auto *actionsRow = new QHBoxLayout;
        actionsRow->setContentsMargins(0, 0, 0, 0);
        actionsRow->setSpacing(6);
        actionsRow->addWidget(m_profileSelfActions);
        actionsRow->addWidget(m_profileMessageButton);
        actionsRow->addWidget(m_profileTakeOwnershipButton);
        actionsRow->addStretch(1);
        userColumn->addLayout(actionsRow);
        userRow->addLayout(userColumn, 1);

        nextMachineHeading = makeProfileSection(QStringLiteral("THIS MACHINE"));
        nextMachineHeading->setProperty("profileNextMachine", true);
        nextMachineCard = new QFrame;
        nextMachineCard->setObjectName(QStringLiteral("profileNextMachineCard"));
        nextMachineCard->setAttribute(Qt::WA_StyledBackground, true);
        auto *machineLayout = new QVBoxLayout(nextMachineCard);
        machineLayout->setContentsMargins(0, 0, 0, 0);
        machineLayout->setSpacing(0);
        auto *machineHead = new QWidget;
        machineHead->setObjectName(QStringLiteral("profileNextMachineHead"));
        machineHead->setAttribute(Qt::WA_StyledBackground, true);
        auto *machineHeadRow = new QHBoxLayout(machineHead);
        machineHeadRow->setContentsMargins(20, 12, 20, 12);
        machineHeadRow->setSpacing(12);
        auto *machineTitles = new QVBoxLayout;
        machineTitles->setContentsMargins(0, 0, 0, 0);
        machineTitles->setSpacing(2);
        for (const QString &name : {QStringLiteral("profileNextMachineName"),
                                    QStringLiteral("profileNextMachineSpec")}) {
            auto *text = new QLabel;
            text->setObjectName(name);
            text->setTextFormat(Qt::PlainText);
            machineTitles->addWidget(text);
        }
        machineHeadRow->addLayout(machineTitles, 1);
        machineHeadRow->addWidget(
            new kit::Badge(QStringLiteral("This device"), kit::Tone::Neutral, true), 0,
            Qt::AlignVCenter);
        machineLayout->addWidget(machineHead);
        tileRow->setSpacing(0);
        for (QLabel *tile : {m_profileTileRepos, m_profileTileMirrored, m_profileTileOnline,
                             m_profileTileChats})
            tile->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_profileTileChats->setProperty("profileNextLast", true);
        machineLayout->addWidget(m_profileStatGrid);

        nextDetails = new QFrame;
        nextDetails->setObjectName(QStringLiteral("profileNextDetails"));
        nextDetails->setAttribute(Qt::WA_StyledBackground, true);
        auto *detailsLayout = new QVBoxLayout(nextDetails);
        detailsLayout->setContentsMargins(0, 0, 0, 0);
        detailsLayout->setSpacing(0);
        m_profileDetails->hide();
        m_profileDetails->setProperty("profileNextHidden", true);
        m_profileGetPaidButton->setProperty("profileNextLink", true);

        leftColumn->addWidget(m_profileMirrorsLabel);
        leftColumn->addWidget(m_profileMirrors);
        leftColumn->addStretch();
    } else {
        leftColumn->addWidget(m_profileAvatar, 0, Qt::AlignHCenter);
        leftColumn->addWidget(m_profileName);
        leftColumn->addWidget(m_profileGetPaidButton, 0, Qt::AlignHCenter);
        leftColumn->addWidget(m_profileOnlineSection);
        leftColumn->addWidget(m_profileStatus);
        leftColumn->addWidget(m_profileNote);
        leftColumn->addWidget(m_profileMessageButton, 0, Qt::AlignHCenter);
        leftColumn->addWidget(m_profileTakeOwnershipButton, 0, Qt::AlignHCenter);
        leftColumn->addWidget(m_profileStatGrid);
        leftColumn->addWidget(detailsLabel);
        leftColumn->addWidget(m_profileDetails);
        leftColumn->addWidget(m_profileMirrorsLabel);
        leftColumn->addWidget(m_profileMirrors);
        leftColumn->addStretch();
    }

    auto *rightColumn = new QVBoxLayout;
    rightColumn->setContentsMargins(0, 0, 0, 0);
    rightColumn->setSpacing(6);
    rightColumn->addWidget(m_profileHostingLabel);
    rightColumn->addWidget(m_profileHosting);
    rightColumn->addWidget(nodeKeyLabel);
    rightColumn->addWidget(m_profileNodeKey);
    auto *nodeKeyActions = new QHBoxLayout;
    nodeKeyActions->setContentsMargins(0, 0, 0, 0);
    nodeKeyActions->setSpacing(6);
    nodeKeyActions->addWidget(copyKey);
    nodeKeyActions->addWidget(m_profileLinkBrowserButton);
    nodeKeyActions->addStretch();
    rightColumn->addLayout(nodeKeyActions);
    rightColumn->addWidget(m_profileAccountSection);
    rightColumn->addWidget(m_profileSolanaSection);
    rightColumn->addWidget(m_profileTipWalletSection);
    rightColumn->addStretch();

    auto *columnsRow = new QHBoxLayout;
    m_profileColumnsRow = columnsRow;
    columnsRow->setContentsMargins(0, 0, 0, 0);
    columnsRow->setSpacing(28);
    columnsRow->addLayout(leftColumn, 1);
    columnsRow->addLayout(rightColumn, 1);

    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(6);
    layout->addLayout(topRow);
    if (next) {
        layout->setSpacing(8);
        layout->addWidget(nextUserCard);
        layout->addWidget(m_profileGetPaidButton, 0, Qt::AlignLeft);
        layout->addSpacing(12);
        layout->addWidget(nextMachineHeading);
        layout->addWidget(nextMachineCard);
        layout->addSpacing(12);
        layout->addWidget(m_profileOnlineSection);
        layout->addSpacing(12);
        layout->addWidget(detailsLabel);
        layout->addWidget(nextDetails);
        layout->addWidget(m_profileDetails);
        layout->addSpacing(12);
    } else {
        layout->addWidget(m_profileSelfActions); // "THIS NODE" actions pinned up top
    }
    layout->addLayout(columnsRow);

    scroll->setWidget(content);
    updateNodeOnlineControls();
    return scroll;
}

void MainWindow::hostNodeProfilePanel(bool inSettings)
{
    if (!m_nodeProfilePanel)
        return;
    QWidget *host = inSettings ? m_settingsProfileHost : m_nodeProfileSectionHost;
    if (!host || m_nodeProfilePanel->parentWidget() == host)
        return;
    auto *layout = qobject_cast<QBoxLayout *>(host->layout());
    if (!layout)
        return;
    if (QWidget *old = m_nodeProfilePanel->parentWidget()) {
        if (old->layout())
            old->layout()->removeWidget(m_nodeProfilePanel);
    }
    // The panel is a scroll area of its own. On the Settings page it grows to
    // its content instead of scrolling inside the page that scrolls.
    SettingsSectionsView::setScrollAreaFlattened(
        qobject_cast<QScrollArea *>(m_nodeProfilePanel), inSettings);
    // Settings shares its width with the section list, so the two columns
    // stack there, and its own "Settings" button would only point back here.
    if (m_profileColumnsRow)
        m_profileColumnsRow->setDirection(inSettings ? QBoxLayout::TopToBottom
                                                     : QBoxLayout::LeftToRight);
    layoutProfileActions(inSettings);
    if (inSettings) {
        m_nodeProfilePanel->setMinimumWidth(0);
        layout->addWidget(m_nodeProfilePanel, 1);
    } else {
        m_nodeProfilePanel->setMinimumWidth(920);
        layout->insertWidget(1, m_nodeProfilePanel, 0); // between the stretchers
    }
    if (m_profileCloseButton)
        m_profileCloseButton->setVisible(!inSettings);
    m_nodeProfilePanel->show();
}

void MainWindow::layoutProfileActions(bool inSettings)
{
    if (!m_profileActionGrid)
        return;
    if (m_profileSettingsButton)
        m_profileSettingsButton->setVisible(!inSettings);
    const bool next = Theme::isNext();
    const int columns =
        inSettings && !next ? 3 : int(m_profileActionButtons.size());
    for (QPushButton *button : std::as_const(m_profileActionButtons))
        m_profileActionGrid->removeWidget(button);
    int slot = 0;
    for (QPushButton *button : std::as_const(m_profileActionButtons)) {
        if (inSettings && button == m_profileSettingsButton)
            continue;
        m_profileActionGrid->addWidget(button, slot / columns, slot % columns);
        ++slot;
    }
    for (int column = 0; column < m_profileActionGrid->columnCount(); ++column)
        m_profileActionGrid->setColumnStretch(column,
                                              next ? 0 : (column < columns ? 1 : 0));
    if (next)
        m_profileActionGrid->setColumnStretch(columns, 1);
}

void MainWindow::syncSettingsProfileTab()
{
    // Settings is one page, so its Profile section borrows the panel whenever
    // Settings is shown, not only while a Profile tab was open.
    if (!m_settingsSections || m_profileSettingsTabIndex < 0 ||
        !m_settingsSections->isExpanded(m_profileSettingsTabIndex))
        return;
    ensureSectionBuilt(10); // the panel is built with the profile section
    hostNodeProfilePanel(true);
    showNodeProfile(m_profileIdentity.publicKey(), topBarUserName(),
                    /*navigate=*/false);
}

void MainWindow::hideNodeProfile()
{
    m_profileNodeId.clear();
    m_profileNodeName.clear();
    m_profileSolanaValue.clear();
    showSection(0);
}

void MainWindow::rescaleProfileAvatar()
{
    if (!m_profileAvatar)
        return;
    const int side = qMax(m_profileAvatar->width(), 1);
    QPixmap src = m_profileAvatarSource;
    if (src.isNull())
        src = nodeMachineFavicon(m_profileNodeName, 96);
    m_profileAvatar->setPixmap(roundedRectPixmap(src, side, side / 2.0));
}

void MainWindow::showNodeProfile(const QString &nodeId, const QString &nodeName,
                                 bool navigate)
{
    if (!m_nodeProfilePanel)
        return;

    MemberInfo info;
    bool found = false;
    for (const MemberInfo &m : std::as_const(m_homeRoster)) {
        if ((!nodeId.isEmpty() && m.id == nodeId) ||
            (nodeId.isEmpty() && m.name == nodeName)) {
            info = m;
            found = true;
            break;
        }
    }
    if (!found) {
        info.id = nodeId;
        info.name = nodeName;
        info.self = !nodeId.isEmpty() && nodeId == m_profileIdentity.publicKey();
    }
    QString solana = info.solanaAddress.trimmed();
    if (info.self && solana.isEmpty())
        solana = savedSolanaAddress();

    m_profileNodeId = info.id;
    m_profileNodeName = info.name;
    m_profileSolanaValue = solana;
    m_profileIsSelf = info.self;

    QPixmap avatar = m_avatars.value(info.id);
    if (avatar.isNull())
        avatar = nodeMachineFavicon(info.id.isEmpty() ? info.name : info.id, 96);
    m_profileAvatarSource = avatar;
    rescaleProfileAvatar();

    const QString crown =
        (info.self && m_isAdmin) ? QString::fromUtf8(" \xF0\x9F\x91\x91") : QString();
    const bool nextProfile = Theme::isNext();
    m_profileName->setText(info.name.toHtmlEscaped() +
                           (info.self && !nextProfile ? " (you)" : QString()) + crown);
    if (nextProfile) {
        for (QLabel *badge : m_nodeProfilePanel->findChildren<QLabel *>(QStringLiteral("badge")))
            if (badge->property("profileNextYou").toBool())
                badge->setVisible(info.self);
        QStringList meta;
        if (info.self)
            meta << QStringLiteral("This machine");
        else if (!info.ownerUser.trimmed().isEmpty())
            meta << QStringLiteral("Owned by %1").arg(info.ownerUser.trimmed());
        if (!info.platform.trimmed().isEmpty())
            meta << info.platform.trimmed();
        if (!info.version.trimmed().isEmpty())
            meta << QStringLiteral("v%1").arg(info.version.trimmed());
        if (auto *label = m_nodeProfilePanel->findChild<QLabel *>(
                QStringLiteral("profileNextMeta")))
            label->setText(meta.isEmpty()
                               ? QString()
                               : QString::fromUtf8("\xC2\xB7 ") +
                                     meta.join(QString::fromUtf8(" \xC2\xB7 ")));
        if (auto *card = m_nodeProfilePanel->findChild<QFrame *>(
                QStringLiteral("profileNextMachineCard")))
            card->setVisible(info.self);
        for (QLabel *heading :
             m_nodeProfilePanel->findChildren<QLabel *>(QStringLiteral("sectionLabel")))
            if (heading->property("profileNextMachine").toBool())
                heading->setVisible(info.self);
        if (auto *label = m_nodeProfilePanel->findChild<QLabel *>(
                QStringLiteral("profileNextMachineName")))
            label->setText(info.name);
        QStringList spec;
        if (!info.platform.trimmed().isEmpty())
            spec << info.platform.trimmed();
        if (!info.version.trimmed().isEmpty())
            spec << QStringLiteral("ForkMesh v%1").arg(info.version.trimmed());
        if (info.self && m_connectedAtMs > 0)
            spec << QStringLiteral("online %1").arg(formatDuration(
                        QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs));
        if (auto *label = m_nodeProfilePanel->findChild<QLabel *>(
                QStringLiteral("profileNextMachineSpec")))
            label->setText(spec.join(QString::fromUtf8(" \xC2\xB7 ")));
    }
    const bool online = info.self ? (m_backend != nullptr) : info.online;
    m_profileStatus->setText(
        QString::fromUtf8("<span style='color:%1'>\xE2\x97\x8F</span> %2")
            .arg(online ? "#3fb950" : "#8b949e", online ? "Online" : "Offline"));

    // Node ID = Ed25519 public key. For yourself, fall back to our own key when
    // the roster entry has no id yet.
    QString nodeKey = info.id;
    if (info.self && nodeKey.isEmpty())
        nodeKey = m_profileIdentity.publicKey();
    m_profileNodeId = nodeKey; // keep the copy button in sync with what's shown
    m_profileNodeKey->setText(nodeKey.isEmpty() ? QStringLiteral("unknown")
                                                : withSoftBreaks(nodeKey));

    if (info.self) {
        int repos = 0, mirrored = 0, onlineRepos = 0;
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (repo.previewOnly)
                continue;
            ++repos;
            if (repo.lastSyncMs > 0 ||
                (!repo.mirrorPath.isEmpty() && QDir(repo.mirrorPath).exists()))
                ++mirrored;
            if (repo.publishToNetwork)
                ++onlineRepos;
        }
        auto tile = [nextProfile](QLabel *label, int n, const QString &caption) {
            if (nextProfile) {
                label->setText(
                    QStringLiteral("<span style='font-size:11px;color:%1'>%2</span>"
                                   "<br><span style='font-size:20px;font-weight:600;"
                                   "color:%3'>%4</span>")
                        .arg(Theme::hex(Theme::Role::FgSubtle), caption,
                             Theme::hex(Theme::Role::Fg))
                        .arg(n));
                return;
            }
            label->setText(
                QStringLiteral(
                    "<span style='font-size:18px;font-weight:700'>%1</span>"
                    "<br><span style='font-size:9px;letter-spacing:1px;"
                    "color:#8b949e'>%2</span>")
                    .arg(n)
                    .arg(caption));
        };
        tile(m_profileTileRepos, repos, QStringLiteral("REPOS"));
        tile(m_profileTileMirrored, mirrored, QStringLiteral("MIRRORED"));
        tile(m_profileTileOnline, onlineRepos, QStringLiteral("NETWORK"));
        tile(m_profileTileChats, m_channels.size(), QStringLiteral("CHATS"));
    }
    m_profileStatGrid->setVisible(info.self);

    auto detailRow = [](const QString &key, const QString &value) {
        return QStringLiteral(
                   "<tr><td style='color:#8b949e;padding:1px 14px 1px 0;"
                   "white-space:nowrap'>%1</td>"
                   "<td style='padding:1px 0'>%2</td></tr>")
            .arg(key, value);
    };
    QStringList rows;
    QList<QPair<QString, QString>> plainRows;
    rows << detailRow(QStringLiteral("Status"),
                      online ? QStringLiteral("Online") : QStringLiteral("Offline"));
    plainRows.append({QStringLiteral("Status"),
                      online ? QStringLiteral("Online") : QStringLiteral("Offline")});
    if (!info.platform.isEmpty()) {
        rows << detailRow(QStringLiteral("Platform"), info.platform.toHtmlEscaped());
        plainRows.append({QStringLiteral("Platform"), info.platform});
    }
    if (!info.version.isEmpty()) {
        rows << detailRow(QStringLiteral("Version"),
                          QStringLiteral("v%1").arg(info.version.toHtmlEscaped()));
        plainRows.append({QStringLiteral("Version"), QStringLiteral("v%1").arg(info.version)});
    }
    if (info.self) {
        const qint64 sessionMs =
            m_connectedAtMs > 0
                ? QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs
                : 0;
        rows << detailRow(QStringLiteral("Uptime"), formatDuration(sessionMs));
        rows << detailRow(QStringLiteral("Total uptime"),
                          formatDuration(m_totalConnectionMs + sessionMs));
        rows << detailRow(QStringLiteral("Channels"),
                          QString::number(m_channels.size()));
        plainRows.append({QStringLiteral("Uptime"), formatDuration(sessionMs)});
        plainRows.append({QStringLiteral("Total uptime"),
                          formatDuration(m_totalConnectionMs + sessionMs)});
        plainRows.append({QStringLiteral("Channels"), QString::number(m_channels.size())});
    }
    const QString shortKey =
        nodeKey.size() > 16
            ? nodeKey.left(8) + QString::fromUtf8("\xE2\x80\xA6") + nodeKey.right(6)
            : nodeKey;
    if (!shortKey.isEmpty()) {
        rows << detailRow(QStringLiteral("Key"), shortKey.toHtmlEscaped());
        plainRows.append({QStringLiteral("Key"), shortKey});
    }
    if (nextProfile)
        if (auto *list = m_nodeProfilePanel->findChild<QFrame *>(
                QStringLiteral("profileNextDetails")))
            fillProfileNextDetails(list, plainRows, online);
    m_profileDetails->setText(
        QStringLiteral("<table cellspacing='0' cellpadding='0'>%1</table>")
            .arg(rows.join(QString())));

    if (info.mirrors.isEmpty()) {
        m_profileMirrorsLabel->setVisible(false);
        m_profileMirrors->setVisible(false);
    } else {
        m_profileMirrorsLabel->setText(
            QStringLiteral("MIRRORS (%1)").arg(formatCount(info.mirrors.size())));
        QHash<QString, MirrorAdvert> byKey;
        for (const MirrorAdvert &d : std::as_const(info.mirrorDetails)) {
            if (!d.source.isEmpty())
                byKey.insert(d.source, d);
            if (!d.ownerName.isEmpty())
                byKey.insert(d.ownerName, d);
        }
        QStringList lines;
        for (const QString &m : std::as_const(info.mirrors)) {
            const MirrorAdvert d = byKey.value(m);
            QString detail;
            if (!d.branch.isEmpty() || !d.commit.isEmpty()) {
                QStringList parts;
                if (!d.branch.isEmpty())
                    parts << d.branch.toHtmlEscaped();
                if (!d.commit.isEmpty())
                    parts << QStringLiteral("@ %1").arg(d.commit.left(7));
                if (d.updatedMs > 0)
                    parts << formatRepoDate(d.updatedMs);
                detail = QStringLiteral(
                             "<br><span style='color:#8b949e'>%1</span>")
                             .arg(parts.join(QString::fromUtf8(" \xC2\xB7 ")));
            }
            lines << QStringLiteral("<b>%1</b>%2").arg(m.toHtmlEscaped(), detail);
        }
        m_profileMirrors->setText(lines.join(QStringLiteral("<br>")));
        m_profileMirrorsLabel->setVisible(true);
        m_profileMirrors->setVisible(true);
    }

    if (m_profileLinkBrowserButton)
        m_profileLinkBrowserButton->setVisible(info.self);
    if (m_profileAccountSection) {
        m_profileAccountSection->setVisible(info.self);
        if (info.self)
            refreshProfileAccountStatus();
    }

    refreshProfileHostingStats();

    m_profileNote->setText(info.note.toHtmlEscaped());
    m_profileNote->setVisible(!info.note.trimmed().isEmpty());

    m_profileMessageButton->setVisible(
        !info.self && !info.id.isEmpty() &&
        !(info.ownerUser.trimmed().isEmpty()
              ? verifiedChatAccount(info.id, QString())
              : info.ownerUser.trimmed())
             .isEmpty());
    // Admin-only takeover request; hidden entirely for non-admins and for your
    // own profile (nothing to take ownership of there).
    if (m_profileTakeOwnershipButton)
        m_profileTakeOwnershipButton->setVisible(
            !info.self && m_isAdmin && !info.name.isEmpty());
    if (m_profileSelfActions)
        m_profileSelfActions->setVisible(info.self);
    if (m_profileOnlineSection)
        m_profileOnlineSection->setVisible(info.self);

    if (m_profileGetPaidButton) {
        const bool earning = hasOwnerSigningCapability(accountOwner()) &&
                             !solana.isEmpty();
        m_profileGetPaidButton->setVisible(info.self);
        m_profileGetPaidButton->setText(
            earning
                ? QString::fromUtf8(
                      "\xE2\x9C\x93 Reward eligibility configured")
                : QString::fromUtf8("Mirror reward settings"));
    }

    if (m_profileVerifyButton)
        m_profileVerifyButton->setVisible(info.self);
    if (m_profileEligibility) {
        m_profileEligibility->setVisible(info.self);
        if (info.self)
            m_profileEligibility->setText(
                m_accountSolanaVerified
                    ? QString::fromUtf8(
                          "<span style='color:#3fb950'>Reward eligibility "
                          "configured \xC2\xB7 may be eligible</span>")
                    : QString::fromUtf8(
                          "<span style='color:#d29922'>Not yet eligible "
                          "\xE2\x80\x94 configure and verify a public "
                          "self-custodial address.</span>"));
    }

    if (solana.isEmpty()) {
        m_profileSolanaSection->hide();
    } else {
        m_profileSolanaSection->show();
        m_profileSolanaAddr->setText(withSoftBreaks(solana));
        const QImage qr = QrCode::encodeToImage(QStringLiteral("solana:%1").arg(solana), 4, 3);
        if (!qr.isNull())
            m_profileQr->setPixmap(QPixmap::fromImage(qr));
        m_profileQr->setVisible(!qr.isNull());
        m_profileBalance->setText(info.solanaBalance.trimmed().isEmpty()
                                      ? QString::fromUtf8("\xE2\x80\x94")
                                      : info.solanaBalance.trimmed());
        m_profileBalanceButton->setEnabled(true);
        m_profileBalanceButton->setText("Check balance");
    }

    // Runs after m_profileSolanaValue/m_profileIsSelf are settled above: it
    // decides both the "Send tip" button and the self-only wallet section from
    // them.
    refreshTipWalletControls();

    if (navigate) {
        hostNodeProfilePanel(false);
        showSection(10);
    }
}

void MainWindow::refreshProfileHostingStats()
{
    if (!m_profileHosting || !m_profileHostingLabel)
        return;
    if (!m_profileIsSelf) {
        m_profileHosting->clear();
        m_profileHosting->setVisible(false);
        m_profileHostingLabel->setVisible(false);
        return;
    }
    QStringList lines;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly)
            continue;
        lines << QString::fromUtf8(
                     "<b>%1</b> \xC2\xB7 %2<br>"
                     "<span style='color:#8b949e'>added %3 \xC2\xB7 "
                     "last sync %4</span>")
                     .arg(repo.name.toHtmlEscaped())
                     .arg(repo.publishToNetwork
                              ? QStringLiteral("managed mirror enabled")
                              : QStringLiteral("local only"),
                          formatRepoDate(repo.hostedSinceMs),
                          formatRepoDate(repo.lastSyncMs));
    }
    m_profileHosting->setText(
        lines.isEmpty() ? QStringLiteral("No local repositories yet.")
                        : lines.join(QStringLiteral("<br>")));
    m_profileHosting->setVisible(true);
    m_profileHostingLabel->setVisible(true);
}

static QStringList profileNodesFromJson(const QJsonValue &value)
{
    QStringList out;
    for (const QJsonValue &v : value.toArray()) {
        const QString name = v.toString().trimmed();
        if (!name.isEmpty())
            out << name;
    }
    return out;
}

QString MainWindow::linkedNodesHtml() const
{
    if (m_profileLinkedNodes.isEmpty())
        return QString();
    const QString machine = machineNodeName();
    QStringList parts;
    for (const QString &n : m_profileLinkedNodes) {
        QString label = QStringLiteral("<b>%1</b>").arg(n.toHtmlEscaped());
        if (n.compare(machine, Qt::CaseInsensitive) == 0)
            label += QString::fromUtf8(" <span style='color:#8b949e'>(this "
                                       "machine)</span>");
        parts << label;
    }
    return parts.join(QStringLiteral(", "));
}

void MainWindow::renderProfileAccountStatus()
{
    if (!m_profileAccountStatus || !m_profileLinkUserButton)
        return;
    // The browser-link button is ALWAYS offered on your own profile — even when
    // this node is already linked or is itself a user account — because the
    // grant flow overrides the current association: whoever authenticates in
    // the browser takes possession of this node.
    if (m_profileLinkBrowserButton) {
        m_profileLinkBrowserButton->setVisible(true);
        m_profileLinkBrowserButton->setEnabled(true);
    }
    QString userName = m_nodeOwnerUser.trimmed().toLower();
    if (userName.isEmpty() && (m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty()))
        userName = accountOwner().trimmed().toLower();
    const bool hasUserProfile = !userName.isEmpty();
    int nodeCount = 0;
    if (m_profileUserNodesList) {
        QStringList nodes = m_profileLinkedNodes;
        const QString machine = machineNodeName();
        if (hasUserProfile && !machine.isEmpty() &&
            !nodes.contains(machine, Qt::CaseInsensitive))
            nodes.prepend(machine);
        nodeCount = nodes.size();

        auto rosterInfo = [this](const QString &name) -> MemberInfo {
            MemberInfo best;
            bool found = false;
            for (const MemberInfo &m : std::as_const(m_homeRoster)) {
                if (nodeListIdentityKey(m) != name)
                    continue;
                if (!found || (m.online && !best.online)) {
                    best = m;
                    found = true;
                    continue;
                }
                if (best.version.trimmed().isEmpty())
                    best.version = m.version;
                if (best.platform.trimmed().isEmpty())
                    best.platform = m.platform;
                if (best.mirrors.isEmpty())
                    best.mirrors = m.mirrors;
            }
            return best;
        };
        const QString dot = QString::fromUtf8(" \xC2\xB7 ");
        m_profileUserNodesList->clear();
        for (const QString &nodeName : std::as_const(nodes)) {
            const MemberInfo mi = rosterInfo(nodeName);
            const NodeMenuEntry *menu = nullptr;
            for (const NodeMenuEntry &e : std::as_const(m_nodeMenuEntries)) {
                if (e.name.compare(nodeName, Qt::CaseInsensitive) == 0) {
                    menu = &e;
                    break;
                }
            }
            const bool isThisMachine =
                nodeName.compare(machine, Qt::CaseInsensitive) == 0;
            const bool rosterLive = mi.online || (menu && menu->online);
            const bool relayLive =
                m_relayOnlineNodes.contains(nodeName.trimmed().toLower());
            const bool online = isThisMachine
                                    ? (m_backend != nullptr)
                                    : (m_relayOnlineNodesFetched
                                           ? relayLive
                                           : (rosterLive || relayLive));

            QStringList meta;
            meta << (online ? QStringLiteral("Online") : QStringLiteral("Offline"));
            const QString platform =
                mi.platform.trimmed().isEmpty() && menu ? menu->platform
                                                        : mi.platform.trimmed();
            if (!platform.isEmpty())
                meta << platform;
            if (!mi.version.trimmed().isEmpty())
                meta << QStringLiteral("v%1").arg(mi.version.trimmed());
            if (menu && menu->repoCount > 0)
                meta << QStringLiteral("%1 repo%2")
                            .arg(menu->repoCount)
                            .arg(menu->repoCount == 1 ? QString()
                                                      : QStringLiteral("s"));
            if (!mi.mirrors.isEmpty())
                meta << QStringLiteral("%1 mirror%2")
                            .arg(mi.mirrors.size())
                            .arg(mi.mirrors.size() == 1 ? QString()
                                                        : QStringLiteral("s"));

            auto *item = new QListWidgetItem;
            item->setIcon(QIcon(
                roundedRectPixmap(nodeMachineFavicon(nodeName, 32), 32, 8)));
            item->setText(
                nodeName +
                (isThisMachine ? QStringLiteral("  (this machine)") : QString()) +
                QLatin1Char('\n') + meta.join(dot));
            item->setData(Qt::UserRole, nodeName);
            item->setToolTip(QString::fromUtf8("%1%2\nOwned by %3 \xE2\x80\x94 "
                                            "click to open this node's profile")
                                 .arg(nodeName,
                                      isThisMachine
                                          ? QStringLiteral(" (this machine)")
                                          : QString(),
                                      userName.isEmpty()
                                          ? QStringLiteral("this account")
                                          : userName));
            m_profileUserNodesList->addItem(item);
        }
        const bool showNodes = !nodes.isEmpty();
        m_profileUserNodesList->setVisible(showNodes);
        if (showNodes)
            m_profileUserNodesList->setFixedHeight(
                qMin(324, qMax(60, nodes.size() * 52 + 8)));
    }
    if (!m_nodeOwnerUser.trimmed().isEmpty()) {
        if (m_profileAccountLabel)
            m_profileAccountLabel->setText(
                QStringLiteral("NODES (%1)").arg(nodeCount));
        m_profileAccountStatus->setText(
            QString::fromUtf8(
                "<span style='color:#3fb950'>\xE2\x9C\x94 Linked to user "
                "<b>%1</b></span>")
                .arg(m_nodeOwnerUser.toHtmlEscaped()));
        m_profileAccountStatus->setVisible(true);
        m_profileLinkUserButton->setText("Linked to a user");
        m_profileLinkUserButton->setVisible(false);
    } else if (m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty()) {
        if (m_profileAccountLabel)
            m_profileAccountLabel->setText(
                QStringLiteral("NODES (%1)").arg(nodeCount));
        if (m_profileLinkedNodes.isEmpty()) {
            m_profileAccountStatus->setText(QString::fromUtf8(
                "No other nodes are linked yet \xE2\x80\x94 open "
                "another node's app and use \"Log in as a user\" or "
                "\"Link this node to your account\" there to attach it "
                "to this account."));
            m_profileAccountStatus->setVisible(true);
        } else {
            m_profileAccountStatus->clear();
            m_profileAccountStatus->setVisible(false);
        }
        m_profileLinkUserButton->setVisible(false);
    } else {
        if (m_profileAccountLabel)
            m_profileAccountLabel->setText(QStringLiteral("USER ACCOUNT"));
        m_profileAccountStatus->setText(
            m_linkGrantPollsLeft > 0
                ? QString::fromUtf8(
                      "Finishing in your browser \xE2\x80\xA6 this machine's "
                      "node will be attached to the user logged in on the "
                      "website.")
                : QString::fromUtf8(
                      "This machine's node isn't linked to a user account yet. "
                      "One user can own many nodes \xE2\x80\x94 log in to "
                      "attach it."));
        m_profileAccountStatus->setVisible(true);
        m_profileLinkUserButton->setText("Log in as a user");
        m_profileLinkUserButton->setEnabled(true);
        m_profileLinkUserButton->setVisible(true);
    }
}

// Paint the "USER ACCOUNT" section from an account record: who owns this node,
// whether it is a user account, and which nodes are linked to it.
void MainWindow::applyProfileAccountRecord(const QString &node,
                                           const QJsonObject &record)
{
    if (!record.value(QStringLiteral("exists")).toBool()) {
        renderProfileAccountStatus();
        return;
    }
    m_nodeOwnerUser = record.value(QStringLiteral("owner")).toString();
    m_profileIsUserAccount =
        record.value(QStringLiteral("kind")).toString() ==
        QStringLiteral("user");
    m_profileLinkedNodes =
        profileNodesFromJson(record.value(QStringLiteral("nodes")));
    updateUserSwitcher();
    if (m_profileLinkedNodes.isEmpty() && !m_nodeOwnerUser.trimmed().isEmpty())
        fetchLinkedNodesFromOwner(node, m_nodeOwnerUser);
    renderProfileAccountStatus();
}

void MainWindow::refreshProfileAccountStatus(bool force)
{
    if (!m_profileAccountStatus || !m_profileLinkUserButton)
        return;
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node)) {
        m_nodeOwnerUser.clear();
        m_profileLinkedNodes.clear();
        m_profileIsUserAccount = false;
        updateUserSwitcher();
        if (m_profileAccountLabel)
            m_profileAccountLabel->setText(QStringLiteral("USER ACCOUNT"));
        if (m_profileUserNodesList) {
            m_profileUserNodesList->clear();
            m_profileUserNodesList->setVisible(false);
        }
        m_profileAccountStatus->setText(QString::fromUtf8(
            "Register this machine's node first (see Account settings) to "
            "link it to a user account."));
        m_profileAccountStatus->setVisible(true);
        m_profileLinkUserButton->setText("Log in as a user");
        m_profileLinkUserButton->setEnabled(false);
        m_profileLinkUserButton->setVisible(true);
        if (m_profileLinkBrowserButton) {
            m_profileLinkBrowserButton->setVisible(true);
            m_profileLinkBrowserButton->setEnabled(false);
        }
        return;
    }
    renderProfileAccountStatus();

    // Every repaint of the profile lands here, so the record login read is good
    // enough. Only the callers that just changed it — linking a node, an
    // ownership transfer, the link-grant poll — pass force. They drop every
    // cached record, not just this node's: the linked-node list this section
    // shows is read out of the owner's record, and that is what they changed.
    if (force)
        invalidateAccountRecord();
    const QJsonObject cached = cachedAccountRecord(node);
    if (!cached.isEmpty()) {
        applyProfileAccountRecord(node, cached);
        return;
    }
    if (m_profileAccountFetchInFlight)
        return;

    m_profileAccountFetchInFlight = true;
    QNetworkReply *reply =
        m_networkAccess->get(QNetworkRequest(accountsApiUrl(node)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, node]() {
        m_profileAccountFetchInFlight = false;
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (!m_profileIsSelf || accountOwner() != node)
            return;
        cacheAccountRecord(node, resp);
        applyProfileAccountRecord(node, resp);
    });
}

void MainWindow::fetchLinkedNodesFromOwner(const QString &node,
                                           const QString &owner)
{
    const QJsonObject cached = cachedAccountRecord(owner);
    if (!cached.isEmpty()) {
        m_profileLinkedNodes =
            profileNodesFromJson(cached.value(QStringLiteral("nodes")));
        renderProfileAccountStatus();
        return;
    }
    QNetworkReply *reply =
        m_networkAccess->get(QNetworkRequest(accountsApiUrl(owner)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, node]() {
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (!m_profileIsSelf || accountOwner() != node)
            return;
        if (resp.value(QStringLiteral("exists")).toBool()) {
            cacheAccountRecord(
                resp.value(QStringLiteral("name")).toString(), resp);
            m_profileLinkedNodes =
                profileNodesFromJson(resp.value(QStringLiteral("nodes")));
        }
        renderProfileAccountStatus();
    });
}

void MainWindow::promptLinkNodeToUser()
{
    if (accountOwner().isEmpty() ||
        !hasOwnerSigningCapability(accountOwner())) {
        logSystem("Register this node before linking it to a user account.");
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setWindowTitle(QStringLiteral("Log in as a user"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(QString::fromUtf8(
        "Log in with your ForkMesh user account to attach this node "
        "(<b>%1</b>) to it. One user can own many nodes.")
        .arg(accountOwner().toHtmlEscaped()));
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *idEdit = new QLineEdit;
    idEdit->setPlaceholderText(QStringLiteral("Email or username"));
    layout->addWidget(idEdit);
    auto *pwEdit = new QLineEdit;
    pwEdit->setEchoMode(QLineEdit::Password);
    pwEdit->setPlaceholderText(QStringLiteral("Password"));
    layout->addWidget(pwEdit);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *linkButton =
        buttons->addButton(QStringLiteral("Link node"), QDialogButtonBox::AcceptRole);
    linkButton->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, dialog,
            [this, dialog, idEdit, pwEdit] {
                const QString identifier = idEdit->text().trimmed();
                const QString password = pwEdit->text();
                if (identifier.isEmpty() || password.isEmpty())
                    return;
                submitLinkNodeToUser(identifier, password);
                dialog->accept();
            });
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void MainWindow::submitLinkNodeToUser(const QString &identifier,
                                      const QString &password)
{
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        !m_profileIdentity.isValid())
        return;
    const QString id = identifier.trimmed().toLower();
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    // Sign with THIS node's key: the relay checks the signature against the
    // node's recorded pubkey and the password against the user, so holding both
    // secrets is the whole authorization (no confirmation code needed).
    const QByteArray canonical =
        ("forkmesh-link-self-v1\n" + node + "\n" + id + "\n" + ts).toUtf8();
    const QJsonObject body{{"nodeName", node},
                           {"identifier", id},
                           {"password", password},
                           {"ts", ts},
                           {"sig", m_profileIdentity.signData(canonical)}};
    QNetworkRequest request(accountsApiUrl("link-self"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    if (m_profileLinkUserButton) {
        m_profileLinkUserButton->setEnabled(false);
        m_profileLinkUserButton->setText(QString::fromUtf8("Linking\xE2\x80\xA6"));
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QJsonObject resp =
            QJsonDocument::fromJson(reply->readAll()).object();
        reply->deleteLater();
        if (resp.value(QStringLiteral("linked")).toBool()) {
            m_nodeOwnerUser = resp.value(QStringLiteral("user")).toString();
            m_profileLinkedNodes =
                profileNodesFromJson(resp.value(QStringLiteral("nodes")));
            updateUserSwitcher();
            logSystem("Account: this node is now linked to user \"" +
                      m_nodeOwnerUser + "\".");
            refreshProfileAccountStatus(true);
            return;
        }
        const QString code = resp.value(QStringLiteral("error"))
                                 .toString(QStringLiteral("network error"));
        m_profileAccountStatus->setText(QString::fromUtf8(
            "<span style='color:#f85149'>\xE2\x9C\x98 Couldn't link this node: "
            "%1</span>").arg(linkErrorMessage(code)));
        logSystem("Account: could not link this node to a user (" + code + ").");
        if (m_profileLinkUserButton) {
            m_profileLinkUserButton->setText("Log in as a user");
            m_profileLinkUserButton->setEnabled(true);
            m_profileLinkUserButton->setVisible(true);
        }
    });
}

QString MainWindow::linkErrorMessage(const QString &code) const
{
    if (code == QStringLiteral("invalid_credentials"))
        return QStringLiteral("wrong email/username or password.");
    if (code == QStringLiteral("cannot_link_self"))
        return QString::fromUtf8("this node is already your user account \xE2\x80\x94 "
                              "no linking needed.");
    if (code == QStringLiteral("node_already_owned"))
        return QStringLiteral("this node is already linked to another account.");
    if (code == QStringLiteral("not_a_node"))
        return QStringLiteral("that account can log in on its own, so it can't be "
                              "attached as a node.");
    if (code == QStringLiteral("no_such_node"))
        return QStringLiteral("this node isn't registered with the relay yet.");
    if (code == QStringLiteral("bad_signature") ||
        code == QStringLiteral("unauthorized"))
        return QStringLiteral("this node's key couldn't be verified.");
    return code + QStringLiteral(".");
}

// "Link this node to your account": sign a short-lived grant with
// this node's key and open it as a dashboard URL in the default browser. The
// signature proves node-key control and consents to the link, so whichever
// user is logged in on forkmesh.com there takes ownership without typing
// anything (the in-browser counterpart of "Log in as a user").
void MainWindow::openLinkNodeInBrowser()
{
    const QString node = accountOwner();
    if (node.isEmpty() || !hasOwnerSigningCapability(node) ||
        (!m_profileIdentity.isValid() && !m_profileIdentity.load())) {
        logSystem("Register this node first (see Account settings) to "
                  "link it to a user account.");
        return;
    }
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QByteArray canonical =
        ("forkmesh-link-grant-v1\n" + node + "\n" + ts).toUtf8();
    QUrl url = catalogApiUrl(); // http(s) on the mainnode host
    url.setPath(QStringLiteral("/dashboard"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("link_node"), node);
    query.addQueryItem(QStringLiteral("link_ts"), ts);
    query.addQueryItem(QStringLiteral("link_sig"),
                       m_profileIdentity.signData(canonical));
    url.setQuery(query);
    QDesktopServices::openUrl(url);
    logSystem("Account: opened the browser to link node \"" + node +
              "\" to the user logged in there.");
    m_linkGrantBaselineOwner = m_nodeOwnerUser.trimmed();
    m_linkGrantPollsLeft = 24;
    pollLinkNodeGrant();
}

void MainWindow::pollLinkNodeGrant()
{
    if (m_linkGrantPollsLeft <= 0)
        return;
    if (m_nodeOwnerUser.trimmed() != m_linkGrantBaselineOwner ||
        !m_profileIsSelf) {
        m_linkGrantPollsLeft = 0;
        return;
    }
    --m_linkGrantPollsLeft;
    refreshProfileAccountStatus(true);
    QTimer::singleShot(5000, this, &MainWindow::pollLinkNodeGrant);
}

void MainWindow::checkNodeBalance()
{
    const QString addr = m_profileSolanaValue.trimmed();
    if (addr.isEmpty())
        return;
    m_profileBalanceButton->setEnabled(false);
    m_profileBalanceButton->setText("Checking\xE2\x80\xA6");
    m_profileBalance->setText(QString::fromUtf8("\xE2\x80\xA6"));

    if (!isLikelySolanaAddress(addr)) {
        m_profileBalanceButton->setEnabled(true);
        m_profileBalanceButton->setText("Check balance");
        m_profileBalance->setText("Invalid address");
        return;
    }
    querySolanaBalance(addr, 0);
}

void MainWindow::querySolanaBalance(const QString &addr, int endpointIndex)
{
    const int count = int(sizeof(kSolanaRpcEndpoints) / sizeof(kSolanaRpcEndpoints[0]));
    if (endpointIndex >= count) {
        if (m_profileSolanaValue.trimmed() == addr) {
            m_profileBalanceButton->setEnabled(true);
            m_profileBalanceButton->setText("Refresh balance");
            m_profileBalance->setText("Unavailable");
        }
        return;
    }

    QNetworkRequest request(QUrl(QString::fromLatin1(kSolanaRpcEndpoints[endpointIndex])));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QJsonObject body{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "getBalance"},
        {"params", QJsonArray{addr}},
    };
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, addr, endpointIndex]() {
        const QByteArray raw = reply->readAll();
        const QNetworkReply::NetworkError netError = reply->error();
        reply->deleteLater();
        if (m_profileSolanaValue.trimmed() != addr)
            return;  // panel moved to another node meanwhile
        const QJsonObject root = QJsonDocument::fromJson(raw).object();
        const QJsonObject result = root.value("result").toObject();
        if (netError != QNetworkReply::NoError || !result.contains("value")) {
            querySolanaBalance(addr, endpointIndex + 1);
            return;
        }
        const qint64 lamports = result.value("value").toVariant().toLongLong();
        m_profileBalanceButton->setEnabled(true);
        m_profileBalanceButton->setText("Refresh balance");
        m_profileBalance->setText(formatSolanaBalance(lamports));
    });
}

void MainWindow::selectNode(const QString &node)
{
    if (m_selectedNode == node)
        return;
    m_selectedNode = node;
    m_nodeSwitching = true;
    m_repoMenuEntries.clear();
    startRepoSwitchSpin();
    updateRepoSwitcher();
    refreshRepositoryList();
    const QString label = node.isEmpty() ? QStringLiteral("nodes") : node;
    logSystem(QStringLiteral("Switching to %1 — loading its repositories…")
                  .arg(label));
    startNodeSwitchSpin();
    showLoadStatus(QStringLiteral("Switching to %1…").arg(label));
    QApplication::setOverrideCursor(Qt::BusyCursor);
    QTimer::singleShot(0, this, [this, node, label] {
        if (m_selectedNode != node) {
            QApplication::restoreOverrideCursor();
            dismissLoadStatus();
            return;
        }
        m_repoLoadActive = true;
        QElapsedTimer timer;
        timer.start();
        int firstRepo = -1;
        int repoCount = 0;
        for (const RepoMenuEntry &entry : std::as_const(m_repoMenuEntries))
            if (entry.index >= 0) {
                ++repoCount;
                if (firstRepo < 0)
                    firstRepo = entry.index;
            }
        if (firstRepo >= 0)
            openRepoDetail(firstRepo);
        else
            clearRepoDetail();
        m_nodeSwitching = false;
        m_repoLoadActive = false;
        finishLoadStepTiming(); // log the final step's duration
        stopRepoSwitchSpin();
        stopNodeSwitchSpin();
        QApplication::restoreOverrideCursor();
        flashMessage(firstRepo >= 0
                         ? QStringLiteral("Switched to %1 (%2 repos) in %3 ms.")
                               .arg(label)
                               .arg(repoCount)
                               .arg(timer.elapsed())
                         : QStringLiteral("Switched to %1 — no repositories (%2 ms).")
                               .arg(label)
                               .arg(timer.elapsed()));
    });
}
