
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

#include <QBrush>
#include <QGridLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
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
constexpr int kRepoActionsColumnWidth =
    2 * kRepoCellPadX + 2 * 4 + kRepoActionColumns * kRepoActionSlotWidth +
    (kRepoActionColumns - 1) * kRepoActionSlotSpacing;

inline QFont repoCellLeadFont(const QFont &base, bool bold)
{
    QFont font(base);
    font.setBold(bold);
    return font;
}

inline QFont repoCellBodyFont(const QFont &base)
{
    QFont font(base);
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
};

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
    horizontal->resizeSection(kRepoColActions, kRepoActionsColumnWidth);

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
    grid->setContentsMargins(4, kRepoCellPadY, 4, 2);
    grid->setHorizontalSpacing(kRepoActionSlotSpacing);
    grid->setVerticalSpacing(5);
    for (int columnIndex = 0; columnIndex < kRepoActionColumns;
         ++columnIndex)
        grid->setColumnMinimumWidth(columnIndex, kRepoActionSlotWidth);
    column->addLayout(grid);
    column->addStretch();
    return grid;
}

void addNetworkRepoAction(QGridLayout *grid, QPushButton *button, int slot)
{
    if (!grid || !button)
        return;
    button->setFixedWidth(kRepoActionSlotWidth);
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
} // namespace

QWidget *MainWindow::buildNetworkReposSection()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("Repos"));
    title->setObjectName("sectionTitle");
    QFont titleFont = title->font();
    titleFont = forkmesh::resizedFont(titleFont, 4);
    titleFont.setBold(true);
    title->setFont(titleFont);
    header->addWidget(title);
    header->addStretch();

    m_networkReposStatus = new QLabel(QStringLiteral("Not loaded"));
    m_networkReposStatus->setObjectName("mutedLabel");
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
    header->addWidget(importLocalButton);

    auto *viewButton = new QPushButton(QStringLiteral("Simple list"));
    viewButton->setObjectName("ghostButton");
    viewButton->setCheckable(true);
    viewButton->setCursor(Qt::PointingHandCursor);
    viewButton->setToolTip(QStringLiteral(
        "Switch between a one-line repository list and the full information grid"));
    setOcticon(viewButton, "list-unordered", 14);
    header->addWidget(viewButton);

    m_networkReposRefreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_networkReposRefreshButton->setObjectName("ghostButton");
    m_networkReposRefreshButton->setCursor(Qt::PointingHandCursor);
    m_networkReposRefreshButton->setToolTip(QStringLiteral("Refresh network repositories"));
    setOcticon(m_networkReposRefreshButton, "sync", 14);
    connect(m_networkReposRefreshButton, &QPushButton::clicked, this,
            &MainWindow::refreshNetworkReposPage);
    header->addWidget(m_networkReposRefreshButton);
    outer->addLayout(header);

    // QStringLiteral would widen these UTF-8 bytes one by one into mojibake.
    auto *subtitle = new QLabel(QString::fromUtf8(
        "Repositories grouped by user or organization across the active relay. "
        "Mirror hosts are combined into one repository row \xE2\x80\x94 who and "
        "what it is, what it contains, and the machine serving it, each on its "
        "own line. Hover a cell for the URLs, digests and exact timestamps "
        "behind it, or use a column's \xE2\x8B\xAF menu to hide or move it."));
    subtitle->setObjectName("mutedLabel");
    subtitle->setWordWrap(true);
    outer->addWidget(subtitle);

    auto *repoTabs = new QTabWidget;
    auto *remotePage = new QWidget;
    auto *remoteLayout = new QVBoxLayout(remotePage);
    remoteLayout->setContentsMargins(0, 8, 0, 0);
    remoteLayout->setSpacing(8);
    remoteLayout->addWidget(m_networkReposStatus);

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
    repoTabs->addTab(remotePage, QStringLiteral("Remote repositories"));

    // A private repository is never advertised, and a repository that was never
    // published is not in the catalog either, so neither can appear in the table
    // above no matter how long you refresh it. They are on this machine all the
    // same, so they get their own table with the same columns.
    auto *localHeader = new QHBoxLayout;
    localHeader->setContentsMargins(0, 6, 0, 0);
    localHeader->setSpacing(10);
    auto *localTitle = new QLabel(QStringLiteral("Private & local"));
    localTitle->setObjectName("sectionTitle");
    QFont localTitleFont = localTitle->font();
    localTitleFont = forkmesh::resizedFont(localTitleFont, 1);
    localTitleFont.setBold(true);
    localTitle->setFont(localTitleFont);
    localHeader->addWidget(localTitle);
    localHeader->addStretch();
    m_localReposStatus = new QLabel(QStringLiteral("Not loaded"));
    m_localReposStatus->setObjectName("mutedLabel");
    localHeader->addWidget(m_localReposStatus);
    auto *localPage = new QWidget;
    auto *localLayout = new QVBoxLayout(localPage);
    localLayout->setContentsMargins(0, 8, 0, 0);
    localLayout->setSpacing(8);
    localLayout->addLayout(localHeader);

    auto *localSubtitle = new QLabel(QString::fromUtf8(
        "Repositories this machine holds that the relay does not list above "
        "\xE2\x80\x94 private ones, which are never advertised publicly, and "
        "local-only ones you have not published. Their contents are read from "
        "the copy on this disk, so there is no host or mirror line to show."));
    localSubtitle->setObjectName("mutedLabel");
    localSubtitle->setWordWrap(true);
    localLayout->addWidget(localSubtitle);

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
    repoTabs->addTab(localPage, QStringLiteral("Private / local"));
    outer->addWidget(repoTabs, 1);

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
                    table->verticalHeader()->setDefaultSectionSize(
                        compact ? qMax(30, table->fontMetrics().height() + 12)
                                : networkRepoRowHeight(table->font()));
                    table->verticalHeader()->setMinimumSectionSize(
                        compact ? 24 : networkRepoRowHeight(table->font()));
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
    m_networkReposTable->setCellWidget(row, kRepoColActions, actions);
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
        m_localReposTable->setCellWidget(row, kRepoColActions, actions);
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
