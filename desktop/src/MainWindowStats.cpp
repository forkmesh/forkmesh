// Stats: a BI-style board of every number the app already tracks, under Files
// in the rail. Each tile is one, two or four grid squares with a headline
// value, a line of context and a small chart; the reader adds, removes, drags
// and resizes tiles and the layout is remembered in QSettings. Every reading
// comes from state MainWindow already holds in memory (or a cheap host read),
// never from git or the network, so a refresh is safe on the GUI thread.
#include "MainWindow.h"
#include "AgentGuardrailLedger.h"
#include "AgentToolEfficiencyLedger.h"
#include "ContributorPayoutReceipt.h"
#include "IssueBurnup.h"
#include "MainWindowInternal.h"
#include "QtRuntimeDiagnostics.h"
#include "RepoStatsStore.h"
#include "RestartTiming.h"
#include "StatsDashboard.h"
#include "SystemStats.h"
#include "TeammateStore.h"

#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

using namespace forkmesh::ui;

namespace {

const char *kStatsLayoutSetting = "stats/layout";
// Bumped when the catalog grows a batch of tiles every board should carry:
// a layout saved under an older version has the new tiles appended once.
const char *kStatsLayoutVersionSetting = "stats/layoutVersion";
// Every catalog id the saved board has been offered; see mergeSavedLayout.
const char *kStatsOfferedTilesSetting = "stats/offeredTiles";
constexpr int kStatsLayoutVersion = 2;
constexpr int kStatsRefreshMs = 5'000;
constexpr int kStatsSampleCap = 60;
constexpr qint64 kStatsRatchetRefreshMs = 60'000;
constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;
constexpr double kLamportsPerSol = 1'000'000'000.0;

using Placement = StatsTilePlacement;

QString dayLabel(qint64 ms)
{
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("MMM d"));
}

// Counts (or sums) items into one bucket per day for the last `days` days,
// oldest first; `weight` returns what each item adds to its day.
template <typename Item, typename Stamp, typename Weight>
StatsTileSeries dailySeries(const QList<Item> &items, int days, Stamp stamp,
                            Weight weight)
{
    StatsTileSeries series;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 start = now - qint64(days - 1) * kDayMs;
    series.values = QVector<double>(days, 0.0);
    for (int d = 0; d < days; ++d)
        series.labels.append(dayLabel(start + d * kDayMs));
    for (const Item &item : items) {
        const qint64 at = stamp(item);
        if (at <= 0 || at < start - kDayMs)
            continue;
        const int bucket = int((at - start + kDayMs) / kDayMs) - 1;
        if (bucket >= 0 && bucket < days)
            series.values[bucket] += weight(item);
    }
    return series;
}

StatsTileSeries weeklySeries(const QJsonArray &weeks)
{
    StatsTileSeries series;
    const int n = weeks.size();
    for (int i = 0; i < n; ++i) {
        series.values.append(weeks.at(i).toDouble());
        const int ago = n - 1 - i;
        series.labels.append(ago == 0 ? QStringLiteral("this week")
                                      : QStringLiteral("%1 wk ago").arg(ago));
    }
    return series;
}

StatsTileSeries bucketSeries(const QVector<qint64> &buckets, qint64 bucketMs)
{
    StatsTileSeries series;
    const int n = buckets.size();
    for (int i = 0; i < n; ++i) {
        series.values.append(double(buckets.at(i)));
        const qint64 ago = qint64(n - 1 - i) * bucketMs;
        series.labels.append(ago == 0 ? QStringLiteral("now")
                                      : StatsDashboard::compactDuration(ago) +
                                            QStringLiteral(" ago"));
    }
    return series;
}

// Readings where lower is better (CPU, latency): under `warnAt` operates well,
// under `criticalAt` needs attention, anything above is critical.
StatsTileStatus levelStatus(double value, double warnAt, double criticalAt)
{
    if (value < warnAt)
        return StatsTileStatus::Good;
    return value < criticalAt ? StatsTileStatus::Warning : StatsTileStatus::Critical;
}

// Readings where higher is better (success rates).
StatsTileStatus floorStatus(double value, double goodAt, double warnAt)
{
    if (value >= goodAt)
        return StatsTileStatus::Good;
    return value >= warnAt ? StatsTileStatus::Warning : StatsTileStatus::Critical;
}

// Bars whose label names an outcome paint in its status colour: "Failed" red,
// "Stopped" amber, "Merged" green; every other label keeps the accent.
StatsTileStatus outcomeTone(const QString &label)
{
    const QString lower = label.toLower();
    for (const char *bad : {"fail", "error", "conflict", "denied", "reject", "block", "down"})
        if (lower.contains(QLatin1String(bad)))
            return StatsTileStatus::Critical;
    for (const char *warn : {"degraded", "stopped", "cancel", "waiting", "warn"})
        if (lower.contains(QLatin1String(warn)))
            return StatsTileStatus::Warning;
    for (const char *good : {"success", "succeeded", "merged", "operational", "clean",
                             "approved", "paid"})
        if (lower == QLatin1String(good))
            return StatsTileStatus::Good;
    return StatsTileStatus::None;
}

QVector<StatsTileStatus> outcomeTones(const QStringList &labels)
{
    QVector<StatsTileStatus> tones;
    bool any = false;
    for (const QString &label : labels) {
        tones.append(outcomeTone(label));
        any = any || tones.last() != StatsTileStatus::None;
    }
    return any ? tones : QVector<StatsTileStatus>();
}

// Categorical bars from a label -> value tally, largest first, at most `cap`.
StatsTileSeries tallySeries(const QList<QPair<QString, double>> &tally, int cap = 8)
{
    QList<QPair<QString, double>> sorted = tally;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const QPair<QString, double> &a, const QPair<QString, double> &b) {
                         return a.second > b.second;
                     });
    StatsTileSeries series;
    series.categorical = true;
    for (int i = 0; i < sorted.size() && i < cap; ++i) {
        if (sorted.at(i).second <= 0)
            continue;
        series.labels.append(sorted.at(i).first);
        series.values.append(sorted.at(i).second);
    }
    series.tones = outcomeTones(series.labels);
    return series;
}

StatsTileSeries orderedSeries(const QStringList &labels, const QVector<double> &values)
{
    StatsTileSeries series;
    series.categorical = true;
    series.labels = labels;
    series.values = values;
    series.tones = outcomeTones(labels);
    return series;
}

StatsTileData number(double value, const QString &note = QString())
{
    StatsTileData data;
    data.value = StatsDashboard::compactNumber(value);
    data.note = note;
    return data;
}

StatsTileData unavailable(const QString &note)
{
    StatsTileData data;
    data.available = false;
    data.note = note;
    return data;
}

QString pluralNote(qint64 count, const char *singular, const char *plural)
{
    return QStringLiteral("%1 %2")
        .arg(StatsDashboard::compactNumber(double(count)),
             QLatin1String(count == 1 ? singular : plural));
}

QString solText(double lamports)
{
    const double sol = lamports / kLamportsPerSol;
    return QStringLiteral("%1 SOL").arg(sol, 0, 'f', sol < 10 ? 3 : 1);
}

QString percentText(double value)
{
    return QStringLiteral("%1%").arg(value, 0, 'f', value < 10 ? 1 : 0);
}

// Series from a rolling sample vector and the shared tick labels; the two
// are pushed together, so only their tails need aligning.
StatsTileSeries sampledSeries(const QVector<double> &samples, const QStringList &labels)
{
    StatsTileSeries series;
    const int n = qMin(samples.size(), labels.size());
    series.values = samples.mid(samples.size() - n);
    series.labels = labels.mid(labels.size() - n);
    return series;
}

QVariantMap nextChartColors()
{
    if (!Theme::isNext())
        return {};
    const struct {
        const char *key;
        Theme::Role role;
    } roles[] = {
        {"surface", Theme::Role::Surface},       {"surfaceRaised", Theme::Role::SurfaceRaised},
        {"border", Theme::Role::Border},         {"borderStrong", Theme::Role::BorderStrong},
        {"fg", Theme::Role::Fg},                 {"fgMuted", Theme::Role::FgMuted},
        {"fgSubtle", Theme::Role::FgSubtle},     {"accent", Theme::Role::Accent},
        {"accentSubtle", Theme::Role::AccentSubtle}, {"success", Theme::Role::Success},
        {"warning", Theme::Role::Warning},       {"danger", Theme::Role::Danger},
    };
    QVariantMap map;
    for (const auto &entry : roles)
        map.insert(QLatin1String(entry.key), Theme::color(entry.role));
    map.insert(QStringLiteral("edges"), Theme::edgeIndicators());
    return map;
}

void applyNextChartColors(QWidget *widget)
{
    if (!widget)
        return;
    const QVariantMap map = nextChartColors();
    if (widget->property("nextColors").toMap() != map)
        widget->setProperty("nextColors", map);
}

class NextChartColorSync : public QObject
{
public:
    explicit NextChartColorSync(QWidget *target) : QObject(target), m_target(target)
    {
        target->installEventFilter(this);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_target &&
            (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ||
             event->type() == QEvent::Show))
            applyNextChartColors(m_target);
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_target;
};

QString hostOf(const QString &url)
{
    return QUrl(url).host().trimmed().toLower();
}

} // namespace

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testStatsNavButtonVisible() const
{
    return m_statsNavButton && !m_statsNavButton->isHidden();
}

QStringList MainWindow::testNavButtonsMissingFromRail() const
{
    QStringList missing;
    if (!m_navGroup || !m_appNavigationRailLayout)
        return missing;
    const QList<QAbstractButton *> buttons = m_navGroup->buttons();
    for (QAbstractButton *button : buttons) {
        if (!button)
            continue;
        if (m_appNavigationRailLayout->indexOf(button) >= 0)
            continue;
        // Some destinations are reached from elsewhere (the breadcrumb, a
        // repo tab) and are deliberately not rail items; those have a parent
        // already. A button with no parent at all is the broken case.
        if (button->parentWidget())
            continue;
        missing.append(button->text().isEmpty() ? button->objectName()
                                                : button->text());
    }
    missing.sort();
    return missing;
}

// The rail's items top to bottom by their applyRailOrder key, with "|" for
// each spacer, so a test can see both the order and where the stretch sits.
QStringList MainWindow::testRailOrderKeys() const
{
    QStringList keys;
    if (!m_appNavigationRailLayout)
        return keys;
    for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
        QLayoutItem *item = m_appNavigationRailLayout->itemAt(i);
        if (item->spacerItem())
            keys.append(QStringLiteral("|"));
        else if (auto *separator =
                     dynamic_cast<RailGroupSeparator *>(item->widget()))
            keys.append(RailGroups::separatorKey(separator->id()));
        else if (QWidget *widget = item->widget())
            keys.append(widget->property("forkmeshRailKey").toString());
    }
    return keys;
}

QString MainWindow::testCreateRailGroup(const QString &name)
{
    const QString id = RailGroups::create(railGroupState(), name);
    saveRailGroupState();
    applyRailOrder();
    return id;
}

QLayout *MainWindow::testRailLayout() const
{
    return m_appNavigationRailLayout;
}

QPushButton *MainWindow::testRailSortButton() const
{
    return m_railSortButton;
}

QStringList MainWindow::testRailSortMenuActions()
{
    QStringList names;
    QMenu *menu = buildRailSortMenu(this);
    for (QAction *action : menu->actions())
        if (!action->objectName().isEmpty())
            names.append(action->objectName() +
                         (action->isChecked() ? QStringLiteral("*") : QString()));
    delete menu;
    return names;
}

QString MainWindow::testCreateRailFolder(const QString &name)
{
    const QString id = RailFolders::create(railFolderState(), name);
    saveRailFolderState();
    applyRailOrder();
    return id;
}

QStringList MainWindow::testRailFolderMemberKeys(const QString &folderId) const
{
    QStringList keys;
    if (RailFolderWidget *folder = m_railFolderWidgets.value(folderId))
        for (QWidget *member : folder->members())
            keys.append(member->property("forkmeshRailKey").toString());
    return keys;
}

bool MainWindow::testRailFolderExpanded(const QString &folderId) const
{
    RailFolderWidget *folder = m_railFolderWidgets.value(folderId);
    return folder && folder->isExpanded();
}

bool MainWindow::testRailFolderShowsActive(const QString &folderId) const
{
    RailFolderWidget *folder = m_railFolderWidgets.value(folderId);
    return folder && folder->header()->isChecked();
}

void MainWindow::testExpandRailFolder(const QString &folderId, bool expanded)
{
    if (RailFolderWidget *folder = m_railFolderWidgets.value(folderId))
        folder->setExpanded(expanded);
}

bool MainWindow::testClickRailFolderMember(const QString &folderId,
                                           const QString &key)
{
    RailFolderWidget *folder = m_railFolderWidgets.value(folderId);
    if (!folder || !m_navGroup || !m_sectionStack)
        return false;
    for (QWidget *member : folder->members()) {
        auto *button = qobject_cast<QAbstractButton *>(member);
        if (!button || member->property("forkmeshRailKey").toString() != key)
            continue;
        button->click();
        return m_navGroup->id(button) == m_sectionStack->currentIndex();
    }
    return false;
}

QWidget *MainWindow::testRailContent() const
{
    return m_appNavigationRailLayout ? m_appNavigationRailLayout->parentWidget()
                                     : nullptr;
}

bool MainWindow::testCanPopOutRailItem(const QString &key) const
{
    for (QWidget *widget : findChildren<QWidget *>()) {
        if (widget->property("forkmeshRailKey").toString() == key)
            return canPopOutRailItem(widget);
    }
    return false;
}

void MainWindow::testPopOutRailItem(const QString &key)
{
    for (QWidget *widget : findChildren<QWidget *>()) {
        if (widget->property("forkmeshRailKey").toString() == key) {
            popOutRailItem(widget);
            return;
        }
    }
}

QWidget *MainWindow::testPoppedOutWindow(const QString &key) const
{
    for (const PoppedOutPage &out : m_poppedOutPages) {
        if (out.key == key)
            return out.window;
    }
    return nullptr;
}

int MainWindow::testRepoDetailTabIndex() const
{
    return m_repoDetailStack ? m_repoDetailStack->currentIndex() : -1;
}

int MainWindow::testSectionIndex() const
{
    return m_sectionStack ? m_sectionStack->currentIndex() : -1;
}

QStringList MainWindow::testRepoTabRowKeys() const
{
    QStringList keys;
    if (!m_repoTabRowLayout)
        return keys;
    for (int i = 0; i < m_repoTabRowLayout->count(); ++i) {
        QLayoutItem *item = m_repoTabRowLayout->itemAt(i);
        if (item->spacerItem())
            keys.append(QStringLiteral("|"));
        else if (QWidget *widget = item->widget())
            keys.append(widget->property("forkmeshButtonKey").toString());
    }
    return keys;
}

QStringList MainWindow::testRepoCodeBarKeys() const
{
    QStringList keys;
    for (int i = 0; m_repoCodeBarLayout && i < m_repoCodeBarLayout->count(); ++i) {
        if (QWidget *widget = m_repoCodeBarLayout->itemAt(i)->widget())
            keys.append(widget->property("forkmeshButtonKey").toString());
    }
    return keys;
}

QStringList MainWindow::testButtonPlacementMenuActions(const QString &key)
{
    QStringList texts;
    QWidget *button = nullptr;
    for (QLayout *layout : {static_cast<QLayout *>(m_appNavigationRailLayout),
                            static_cast<QLayout *>(m_repoTabRowLayout),
                            static_cast<QLayout *>(m_repoCodeBarLayout)}) {
        for (int i = 0; layout && !button && i < layout->count(); ++i) {
            QWidget *widget = layout->itemAt(i)->widget();
            if (widget &&
                widget->property("forkmeshButtonKey").toString() == key)
                button = widget;
        }
    }
    if (!button)
        return texts;
    QMenu menu;
    addButtonPlacementActions(&menu, button);
    for (QAction *action : menu.actions()) {
        if (!action->isSeparator())
            texts << action->text();
    }
    return texts;
}
#endif

QWidget *MainWindow::buildStatsSection()
{
    const bool next = Theme::isNext();
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("statsSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);
    if (next) {
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
    }

    auto *heading = new QHBoxLayout;
    QVBoxLayout *titleColumn = nullptr;
    if (next) {
        heading->setContentsMargins(24, 12, 24, 12);
        heading->setSpacing(6);
        outer->addLayout(heading);
        titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        heading->addLayout(titleColumn, 1);
    }
    auto *title = new QLabel(QStringLiteral("Stats"));
    title->setObjectName(QStringLiteral("pageTitle"));
    if (next)
        titleColumn->addWidget(title);
    else
        heading->addWidget(title);
    // The helper that makes a "?" button lives in MainWindowChat.cpp's file
    // scope, so the explanation rides on the title's tooltip instead.
    title->setToolTip(QStringLiteral(
        "Every number the app already tracks, as tiles you arrange like a "
        "BI dashboard. Drag a tile to move it; hover for its resize and "
        "remove controls, or right-click for the same menu. A tile is one "
        "square, two squares (wide or tall) or four squares. Add tile "
        "lists what is not on the board yet, grouped by area; Reset "
        "puts every statistic back. Values refresh every few seconds "
        "while the page is open and are read from the app's own memory "
        "and this machine, never from git or the network; the Realtime "
        "group samples its right-now numbers on each refresh so they carry "
        "a trend."));
    m_statsStatus = new QLabel;
    m_statsStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_statsStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (next) {
        m_statsStatus->setProperty("statsRole", QStringLiteral("summary"));
        titleColumn->addWidget(m_statsStatus);
        heading->addSpacing(12);
    } else {
        heading->addWidget(m_statsStatus);
        heading->addStretch();
    }

    auto *addButton = new QPushButton(QStringLiteral("Add tile"));
    addButton->setObjectName(QStringLiteral("repoAction"));
    addButton->setCursor(Qt::PointingHandCursor);
    addButton->setToolTip(
        QStringLiteral("Pick a statistic to add to the board; stats already "
                       "placed are ticked"));
    if (next) {
        addButton->setProperty("statsRole", QStringLiteral("create"));
        addButton->setProperty("nextIconRole", int(Theme::Role::Bg));
    }
    setOcticon(addButton, "plus", 14);
    connect(addButton, &QPushButton::clicked, this, [this, addButton] {
        if (!m_statsDashboard)
            return;
        QMenu *menu = m_statsDashboard->buildAddMenu(addButton);
        menu->setAttribute(Qt::WA_DeleteOnClose);
        menu->popup(addButton->mapToGlobal(addButton->rect().bottomLeft()));
    });
    heading->addWidget(addButton);

    auto *resetButton = new QPushButton(QStringLiteral("Reset"));
    resetButton->setObjectName(QStringLiteral("ghostButton"));
    resetButton->setCursor(Qt::PointingHandCursor);
    resetButton->setToolTip(QStringLiteral("Put every statistic back on the board"));
    if (next) {
        resetButton->setProperty("statsRole", QStringLiteral("action"));
        resetButton->setProperty("nextIconRole", int(Theme::Role::FgMuted));
    }
    setOcticon(resetButton, "history", 14);
    connect(resetButton, &QPushButton::clicked, this, [this] {
        if (m_statsDashboard)
            m_statsDashboard->resetLayout();
    });
    if (next)
        heading->insertWidget(heading->indexOf(addButton), resetButton);
    else
        heading->addWidget(resetButton);

    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    refreshButton->setObjectName(QStringLiteral("ghostButton"));
    refreshButton->setCursor(Qt::PointingHandCursor);
    refreshButton->setToolTip(QStringLiteral("Re-read every tile now"));
    if (next) {
        refreshButton->setProperty("statsRole", QStringLiteral("action"));
        refreshButton->setProperty("nextIconRole", int(Theme::Role::FgMuted));
    }
    setOcticon(refreshButton, "sync", 14);
    connect(refreshButton, &QPushButton::clicked, this,
            [this] { refreshStatsPage(); });
    if (next) {
        heading->insertWidget(heading->indexOf(addButton), refreshButton);
        auto *divider = new QFrame;
        divider->setObjectName(QStringLiteral("statsHeaderDivider"));
        divider->setFrameShape(QFrame::NoFrame);
        divider->setFixedHeight(1);
        outer->addWidget(divider);
    } else {
        heading->addWidget(refreshButton);
        outer->addLayout(heading);
    }

    m_statsDashboard = new StatsDashboard;
    new NextChartColorSync(m_statsDashboard);
    applyNextChartColors(m_statsDashboard);
    m_statsDashboard->setCatalog(statsTileCatalog());
    m_statsDashboard->setDefaultLayout(defaultStatsLayout());
    m_statsDashboard->setDataProvider(
        [this](const QString &id) { return statsTileData(id); });
    m_statsDashboard->layoutChanged = [this] { saveStatsLayout(); };
    // A destination bar on Network requests opens the log filtered to that
    // host, so its hits and what the firewall did with them can be read.
    m_statsDashboard->pointClicked = [this](const QString &id, int point) {
        if (id != QLatin1String("network-requests"))
            return;
        const StatsTileData data = statsTileData(id);
        if (!data.series.categorical)
            return;
        const QString host = data.series.labels.value(point);
        if (!host.isEmpty())
            openNetworkLogForUrl(QStringLiteral("://") + host);
    };

    QSettings settings;
    const QString saved = settings.value(QLatin1String(kStatsLayoutSetting)).toString();
    // A saved board keeps its arrangement and gains every statistic it has
    // never been offered. The old one-shot version stamp was written without
    // saving the enlarged board, so the next launch re-read the short list,
    // saw the stamp and dropped every new tile for good (2026-09-14). The
    // offered set is saved with the layout, so the merge survives restarts
    // and a tile the user removed stays removed.
    const StatsDashboard::LayoutMerge merge = StatsDashboard::mergeSavedLayout(
        StatsDashboard::layoutFromJson(
            QJsonDocument::fromJson(saved.toUtf8()).array()),
        !saved.isEmpty(), defaultStatsLayout(), statsTileCatalog(),
        settings.value(QLatin1String(kStatsOfferedTilesSetting)).toStringList());
    if (merge.changed) {
        settings.setValue(
            QLatin1String(kStatsLayoutSetting),
            QString::fromUtf8(
                QJsonDocument(StatsDashboard::layoutToJson(merge.layout))
                    .toJson(QJsonDocument::Compact)));
        settings.setValue(QLatin1String(kStatsOfferedTilesSetting),
                          merge.offered);
    }
    settings.setValue(QLatin1String(kStatsLayoutVersionSetting), kStatsLayoutVersion);
    m_statsDashboard->setLayoutModel(merge.layout);

    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    if (next)
        bodyLayout->setContentsMargins(24, 24, 24, 24);
    bodyLayout->setSpacing(0);
    bodyLayout->addWidget(m_statsDashboard);
    bodyLayout->addStretch();

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("statsScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    m_statsRefreshTimer = new QTimer(this);
    m_statsRefreshTimer->setInterval(kStatsRefreshMs);
    connect(m_statsRefreshTimer, &QTimer::timeout, this, [this] {
        if (!m_statsDashboard || !m_sectionStack ||
            m_sectionStack->currentIndex() != kStatsSectionIndex ||
            !m_statsDashboard->isVisible()) {
            m_statsRefreshTimer->stop();
            return;
        }
        refreshStatsPage();
    });
    return page;
}

void MainWindow::saveStatsLayout()
{
    if (!m_statsDashboard)
        return;
    // Keep tiles this build's catalog does not know: a newer build placed
    // them, and saving only what is on screen would erase them.
    QSettings settings;
    QVector<StatsTilePlacement> layout = m_statsDashboard->layoutModel();
    QSet<QString> known;
    for (const StatsTileCatalogEntry &entry : m_statsDashboard->catalog())
        known.insert(entry.id);
    for (const StatsTilePlacement &placement : StatsDashboard::layoutFromJson(
             QJsonDocument::fromJson(
                 settings.value(QLatin1String(kStatsLayoutSetting))
                     .toString()
                     .toUtf8())
                 .array())) {
        if (!known.contains(placement.id))
            layout.append(placement);
    }
    settings.setValue(
        QLatin1String(kStatsLayoutSetting),
        QString::fromUtf8(QJsonDocument(StatsDashboard::layoutToJson(layout))
                              .toJson(QJsonDocument::Compact)));
    if (m_statsStatus)
        m_statsStatus->setText(pluralNote(m_statsDashboard->tileIds().size(),
                                          "tile", "tiles") +
                               QString::fromUtf8(" \xC2\xB7 layout saved"));
}

void MainWindow::sampleStatsHistory()
{
    auto push = [](QVector<double> &samples, double value) {
        samples.append(value);
        while (samples.size() > kStatsSampleCap)
            samples.removeFirst();
    };
    push(m_statsCpuSamples, qMax(0.0, SystemStats::cpuPercent()));
    push(m_statsRssSamples, SystemStats::residentBytes() / (1024.0 * 1024.0));
    const qint64 total = SystemStats::totalMemoryBytes();
    const qint64 available = SystemStats::availableMemoryBytes();
    push(m_statsHostMemorySamples,
         total > 0 ? 100.0 * double(total - available) / double(total) : 0.0);
    m_statsSampleLabels.append(
        QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    while (m_statsSampleLabels.size() > kStatsSampleCap)
        m_statsSampleLabels.removeFirst();

    // Realtime readings: one sample per tick for every live tile, so each
    // right-now number has the same trend window as CPU and memory.
    auto live = [this, &push](const char *id, double value) {
        push(m_statsLiveSamples[QLatin1String(id)], value);
    };
    int running = 0, queued = 0, background = 0;
    for (const AgentSession &session : m_agentSessions) {
        if (session.merged)
            continue;
        if (session.status == AgentStatus::Running)
            ++running;
        else if (session.status == AgentStatus::Queued ||
                 session.status == AgentStatus::Waiting)
            ++queued;
        background += session.backgroundProcesses;
    }
    live("agents-live", running);
    live("agent-queue", queued);
    live("agent-background", background);
    live("compilers", m_agentProcessMetaCompilerCount);
    live("tool-in-flight",
         m_agentToolLedger && m_agentToolLedger->hasSamples()
             ? double(m_agentToolLedger->totals(QDateTime::currentMSecsSinceEpoch()).inFlight)
             : 0.0);
    int pendingRuns = 0;
    for (const ActionRun &run : m_actionRuns)
        if (run.status == ActionStatus::AwaitingApproval ||
            run.status == ActionStatus::Queued || run.status == ActionStatus::Running)
            ++pendingRuns;
    live("action-queue", pendingRuns);
    const RelayLatencySample relay =
        m_activeServer >= 0 && m_activeServer < m_servers.size()
            ? m_relayHostLatency.value(hostOf(m_servers.at(m_activeServer).url))
            : RelayLatencySample{};
    live("relay-latency", relay.ms > 0 ? relay.ms : 0);
    live("network-requests",
         m_statsLastNetworkRequestTotal >= 0
             ? double(qMax<qint64>(0, m_debugNetworkRequestTotal -
                                          m_statsLastNetworkRequestTotal))
             : 0.0);
    m_statsLastNetworkRequestTotal = m_debugNetworkRequestTotal;
    int tickets = 0;
    for (int count : m_backgroundTaskCounts)
        tickets += count;
    live("background-tasks", tickets);
    live("loop-busy", m_qtDiagnostics ? m_qtDiagnostics->snapshot().dispatcherBusyPercent : 0.0);
}

const RepoRatchetReading *MainWindow::statsRatchetReading(const QString &dir)
{
    StatsRatchetEntry &entry = m_statsRatchet[dir];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!entry.inFlight && now - entry.sampledAtMs > kStatsRatchetRefreshMs) {
        entry.inFlight = true;
        runOffThread<RepoRatchetReading>(
            [dir] { return RepoStatsStore::ratchetReading(dir); },
            [this, dir](RepoRatchetReading reading) {
                StatsRatchetEntry &landed = m_statsRatchet[dir];
                const bool first = landed.sampledAtMs <= 0;
                landed.reading = reading;
                landed.sampledAtMs = QDateTime::currentMSecsSinceEpoch();
                landed.inFlight = false;
                if (first && m_statsDashboard && m_statsDashboard->isVisible())
                    m_statsDashboard->refresh();
            });
    }
    return entry.sampledAtMs > 0 ? &entry.reading : nullptr;
}

void MainWindow::refreshStatsPage()
{
    if (!m_statsDashboard)
        return;
    sampleStatsHistory();
    applyNextChartColors(m_statsDashboard);
    m_statsDashboard->refresh();
    if (m_statsStatus)
        m_statsStatus->setText(
            pluralNote(m_statsDashboard->tileIds().size(), "tile", "tiles") +
            QString::fromUtf8(" \xC2\xB7 updated ") +
            QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
    if (m_statsRefreshTimer && !m_statsRefreshTimer->isActive())
        m_statsRefreshTimer->start();
}

QVector<StatsTileCatalogEntry> MainWindow::statsTileCatalog() const
{
    const QString workspace = QStringLiteral("Workspace");
    const QString ratchet = QStringLiteral("Ratchet");
    const QString work = QStringLiteral("Issues & pulls");
    const QString chat = QStringLiteral("Chat & pings");
    const QString agents = QStringLiteral("Agents");
    const QString tools = QStringLiteral("Agent tools");
    const QString actions = QStringLiteral("Actions");
    const QString realtime = QStringLiteral("Realtime");
    const QString network = QStringLiteral("Network & logs");
    const QString machine = QStringLiteral("This machine");
    const QString users = QStringLiteral("Users");
    const QString market = QStringLiteral("Market & rewards");
    QVector<StatsTileCatalogEntry> catalog{
        {QStringLiteral("repos"), QStringLiteral("Repositories"), workspace,
         QStringLiteral("Repositories on this node, split by how they are served")},
        {QStringLiteral("repo-commits"), QStringLiteral("Commits"), workspace,
         QStringLiteral("Commits on the open repository's served branch, per week "
                        "for the last year (from the last mirror advert)")},
        {QStringLiteral("repo-merges"), QStringLiteral("Merges"), workspace,
         QStringLiteral("Merge commits on the open repository, per week")},
        {QStringLiteral("repo-branches"), QStringLiteral("Branches"), workspace,
         QStringLiteral("Local branches the open repository's mirror holds")},
        {QStringLiteral("repo-worktrees"), QStringLiteral("Worktrees"), workspace,
         QStringLiteral("Worktrees the open repository's mirror advertises, with "
                        "its registered pulls")},
        {QStringLiteral("repo-size"), QStringLiteral("Repository size"), workspace,
         QStringLiteral("Tracked bytes in the open repository, per day, from its "
                        "generated statistics")},
        {QStringLiteral("repo-lines"), QStringLiteral("Lines of code"), workspace,
         QStringLiteral("Tracked lines in the open repository, per day")},
        {QStringLiteral("repo-files"), QStringLiteral("Tracked files"), workspace,
         QStringLiteral("Tracked files in the open repository, per day")},
        {QStringLiteral("scm-changes"), QStringLiteral("Working-tree changes"), workspace,
         QStringLiteral("Uncommitted rows Source Control lists for the open repository")},
        {QStringLiteral("teammates"), QStringLiteral("Special agents"), workspace,
         QStringLiteral("AI special agents configured for the open repository")},

        {QStringLiteral("ratchet"), QStringLiteral("Ratchet mode"), ratchet,
         QStringLiteral("Whether Ratchet Mode is on for the open repository: commits "
                        "that grow the tracked bytes past the day's ceiling are refused")},
        {QStringLiteral("ratchet-ceiling"), QStringLiteral("Ratchet ceiling"), ratchet,
         QStringLiteral("Today's size ceiling, with the repository's tracked bytes "
                        "per day beneath it")},
        {QStringLiteral("ratchet-headroom"), QStringLiteral("Ratchet headroom"), ratchet,
         QStringLiteral("Bytes left under the ceiling before a commit is refused, per day")},

        {QStringLiteral("issues-open"), QStringLiteral("Open issues"), work,
         QStringLiteral("Open issues in the open repository, with the last 30 "
                        "days of the burn-up")},
        {QStringLiteral("issues-closed"), QStringLiteral("Closed issues"), work,
         QStringLiteral("Closed issues in the open repository, over 30 days")},
        {QStringLiteral("issues-priority"), QStringLiteral("Issues by priority"), work,
         QStringLiteral("Open issues grouped by priority band")},
        {QStringLiteral("issues-labels"), QStringLiteral("Issues by label"), work,
         QStringLiteral("Labels on open issues, most used first")},
        {QStringLiteral("issues-assigned"), QStringLiteral("Assigned issues"), work,
         QStringLiteral("Open issues with an assignee, by assignee")},
        {QStringLiteral("issues-progress"), QStringLiteral("Issue progress"), work,
         QStringLiteral("Average reported progress across open issues, by quarter")},
        {QStringLiteral("issues-votes"), QStringLiteral("Issue votes"), work,
         QStringLiteral("Uptime-credit votes across open issues, by issue")},
        {QStringLiteral("issues-bounties"), QStringLiteral("Issue bounties"), work,
         QStringLiteral("Bounties posted on open issues, by issue")},
        {QStringLiteral("issues-rewards"), QStringLiteral("Reward estimates"), work,
         QStringLiteral("Contributor reward estimates on issues, by status")},
        {QStringLiteral("pulls"), QStringLiteral("Pull requests"), work,
         QStringLiteral("Pull requests in the open repository: open, merged, closed")},
        {QStringLiteral("pulls-conflicts"), QStringLiteral("Conflicting pulls"), work,
         QStringLiteral("Open pull requests whose last conflict probe found conflicts")},
        {QStringLiteral("discussions"), QStringLiteral("Discussions"), work,
         QStringLiteral("Discussions in the open repository, by status, with comments")},

        {QStringLiteral("chat-unread"), QStringLiteral("Unread messages"), chat,
         QStringLiteral("Unread chat messages, by channel")},
        {QStringLiteral("chat-channels"), QStringLiteral("Channels"), chat,
         QStringLiteral("Chat channels this node follows")},
        {QStringLiteral("pings"), QStringLiteral("Pings"), chat,
         QStringLiteral("Pings received, per day over two weeks")},
        {QStringLiteral("pings-kinds"), QStringLiteral("Pings by kind"), chat,
         QStringLiteral("Pings grouped by what raised them")},
        {QStringLiteral("roster-versions"), QStringLiteral("App versions"), chat,
         QStringLiteral("Roster members by the ForkMesh version they advertise")},
        {QStringLiteral("tasks"), QStringLiteral("Organization tasks"), chat,
         QStringLiteral("Tasks loaded from the organization board")},

        {QStringLiteral("agents"), QStringLiteral("Agent sessions"), agents,
         QStringLiteral("Every agent session on this node, by status")},
        {QStringLiteral("agents-running"), QStringLiteral("Running agents"), agents,
         QStringLiteral("Sessions running right now, with sessions started per "
                        "day over two weeks")},
        {QStringLiteral("agent-success"), QStringLiteral("Agent success rate"), agents,
         QStringLiteral("Finished sessions that succeeded or merged")},
        {QStringLiteral("agent-tokens"), QStringLiteral("Tokens used"), agents,
         QStringLiteral("Tokens across every agent session, per day")},
        {QStringLiteral("agent-spend"), QStringLiteral("Agent spend"), agents,
         QStringLiteral("Estimated cost of agent sessions, per day; the note "
                        "carries the provider bills when known")},
        {QStringLiteral("agent-turns"), QStringLiteral("Agent turns"), agents,
         QStringLiteral("Model turns across every session, per day")},
        {QStringLiteral("agent-runtime"), QStringLiteral("Agent run time"), agents,
         QStringLiteral("Time agents have spent running, with the last twenty sessions")},
        {QStringLiteral("agent-prs"), QStringLiteral("Agent pull requests"), agents,
         QStringLiteral("Sessions that opened a pull request, and how many merged")},
        {QStringLiteral("agent-errors"), QStringLiteral("Agent errors"), agents,
         QStringLiteral("Sessions that ended with an error, with the newest message")},
        {QStringLiteral("agents-providers"), QStringLiteral("Agents by provider"), agents,
         QStringLiteral("Sessions grouped by the provider that ran them")},
        {QStringLiteral("agents-models"), QStringLiteral("Agents by model"), agents,
         QStringLiteral("Sessions grouped by model")},
        {QStringLiteral("agent-skills"), QStringLiteral("Skills used"), agents,
         QStringLiteral("Skills sessions reported using, most used first")},
        {QStringLiteral("agent-grades"), QStringLiteral("Task grades"), agents,
         QStringLiteral("Grades given to finished agent tasks")},
        {QStringLiteral("agent-teammates"), QStringLiteral("Special agent sessions"), agents,
         QStringLiteral("Sessions started by AI special agents, by special agent")},
        {QStringLiteral("claude-usage"), QStringLiteral("Claude usage"), agents,
         QStringLiteral("Claude subscription usage for the 5-hour and weekly windows")},

        {QStringLiteral("tool-calls"), QStringLiteral("Tool calls"), tools,
         QStringLiteral("Agent tool calls recorded by the efficiency ledger, per "
                        "five minutes over the last two hours")},
        {QStringLiteral("tool-errors"), QStringLiteral("Tool errors"), tools,
         QStringLiteral("Tool calls that returned an error, by family")},
        {QStringLiteral("tokens-saved"), QStringLiteral("Tokens saved"), tools,
         QStringLiteral("Context tokens the tool layer avoided injecting")},
        {QStringLiteral("context-injected"), QStringLiteral("Context injected"), tools,
         QStringLiteral("Tokens tool results actually put into context")},
        {QStringLiteral("tool-baseline"), QStringLiteral("Baseline tokens"), tools,
         QStringLiteral("What the naive alternative to each measured call would have "
                        "cost, against what was injected")},
        {QStringLiteral("tool-latency"), QStringLiteral("Tool latency"), tools,
         QStringLiteral("Average measured tool call latency")},
        {QStringLiteral("tool-coverage"), QStringLiteral("Tool coverage"), tools,
         QStringLiteral("Tool families observed, with calls per family")},
        {QStringLiteral("tool-runtimes"), QStringLiteral("Runtime tools"), tools,
         QStringLiteral("Runtime tool names the agents called, most used first")},
        {QStringLiteral("tool-while-off"), QStringLiteral("Calls while off"), tools,
         QStringLiteral("Calls that arrived while their family was switched off")},
        {QStringLiteral("improvement-signals"), QStringLiteral("Improvement signals"), tools,
         QStringLiteral("Potential tool-use improvements from the realtime log, by kind")},
        {QStringLiteral("improvement-stake"), QStringLiteral("Context at stake"), tools,
         QStringLiteral("Context tokens the logged improvement patterns cost, by family")},
        {QStringLiteral("guardrail-blocked"), QStringLiteral("Blocked tool calls"), tools,
         QStringLiteral("Tool calls the guardrails blocked, per hour over the last day")},
        {QStringLiteral("guardrail-decisions"), QStringLiteral("Guardrail decisions"), tools,
         QStringLiteral("Approvals, denials and agents stopped by the guardrails")},

        {QStringLiteral("workflows"), QStringLiteral("Workflows"), actions,
         QStringLiteral("Workflow files in the open repository, by trigger")},
        {QStringLiteral("action-runs"), QStringLiteral("Action runs"), actions,
         QStringLiteral("Workflow runs on this node, by status")},
        {QStringLiteral("action-history"), QStringLiteral("Runs per day"), actions,
         QStringLiteral("Workflow runs started per day over two weeks")},
        {QStringLiteral("action-failed"), QStringLiteral("Failed runs"), actions,
         QStringLiteral("Workflow runs that failed, per day over two weeks")},
        {QStringLiteral("action-success"), QStringLiteral("Action success rate"), actions,
         QStringLiteral("Finished workflow runs that succeeded")},
        {QStringLiteral("action-duration"), QStringLiteral("Action duration"), actions,
         QStringLiteral("Average finished run time, with the last twenty runs")},
        {QStringLiteral("action-wait"), QStringLiteral("Action queue wait"), actions,
         QStringLiteral("Average time from a run being created to it starting, with "
                        "the last twenty runs")},
        {QStringLiteral("action-workflow-times"), QStringLiteral("Time per workflow"), actions,
         QStringLiteral("Average finished run time of each workflow")},
        {QStringLiteral("action-slowest"), QStringLiteral("Slowest run"), actions,
         QStringLiteral("The longest finished workflow run on record")},
        {QStringLiteral("action-last"), QStringLiteral("Last run time"), actions,
         QStringLiteral("How long the most recent finished run took")},
        {QStringLiteral("action-steps"), QStringLiteral("Action steps"), actions,
         QStringLiteral("Workflow steps executed across every run, by outcome")},
        {QStringLiteral("action-pull-runs"), QStringLiteral("Pull request runs"), actions,
         QStringLiteral("Runs triggered by pull requests, and how many were held for "
                        "a first-time author")},

        {QStringLiteral("agents-live"), QStringLiteral("Agents running now"), realtime,
         QStringLiteral("Sessions running at this moment, sampled every refresh")},
        {QStringLiteral("agent-queue"), QStringLiteral("Agent queue"), realtime,
         QStringLiteral("Sessions queued or waiting to run, sampled every refresh")},
        {QStringLiteral("agent-context"), QStringLiteral("Context in use"), realtime,
         QStringLiteral("How full each running session's context window is")},
        {QStringLiteral("agent-background"), QStringLiteral("Agent subprocesses"), realtime,
         QStringLiteral("Background processes running sessions still own, sampled "
                        "every refresh")},
        {QStringLiteral("compilers"), QStringLiteral("Compilers"), realtime,
         QStringLiteral("Compiler processes under the agents, with their memory")},
        {QStringLiteral("tool-in-flight"), QStringLiteral("Tool calls in flight"), realtime,
         QStringLiteral("Tool calls started and not yet answered, sampled every refresh")},
        {QStringLiteral("action-queue"), QStringLiteral("Action queue"), realtime,
         QStringLiteral("Workflow runs awaiting approval, queued or running right now")},
        {QStringLiteral("relay-latency"), QStringLiteral("Relay latency"), realtime,
         QStringLiteral("Round trip to the active relay, sampled every refresh, with "
                        "every relay measured")},
        {QStringLiteral("network-requests"), QStringLiteral("Network requests"), realtime,
         QStringLiteral("HTTP requests this session by destination, live: green is "
                        "allowed by a firewall rule, red is denied by one, amber has "
                        "no decision yet, blue stays on this machine or the "
                        "firewall is off")},
        {QStringLiteral("background-tasks"), QStringLiteral("Background tasks"), realtime,
         QStringLiteral("Open background work tickets, sampled every refresh")},
        {QStringLiteral("loop-busy"), QStringLiteral("Event loop busy"), realtime,
         QStringLiteral("Share of time the GUI event dispatcher was busy, with wakeups "
                        "per second")},

        {QStringLiteral("nodes-online"), QStringLiteral("Nodes online"), network,
         QStringLiteral("Roster members with a live link, by platform")},
        {QStringLiteral("linked-nodes"), QStringLiteral("Linked nodes"), network,
         QStringLiteral("Nodes linked to your account")},
        {QStringLiteral("network-catalog"), QStringLiteral("Network catalog"), network,
         QStringLiteral("Relays, nodes and hosts the Network page lists")},
        {QStringLiteral("servers"), QStringLiteral("Relays configured"), network,
         QStringLiteral("Relay servers saved on this node, and which is active")},
        {QStringLiteral("services"), QStringLiteral("Service status"), network,
         QStringLiteral("Website and desktop service checks that are operational")},
        {QStringLiteral("web-alerts"), QStringLiteral("Web alerts"), network,
         QStringLiteral("Alerts the website raised for this account")},
        {QStringLiteral("firewall"), QStringLiteral("Request firewall"), network,
         QStringLiteral("Outbound requests the request firewall allowed or blocked")},
        {QStringLiteral("uptime"), QStringLiteral("Connection uptime"), network,
         QStringLiteral("How long this session has been connected, and the lifetime total")},
        {QStringLiteral("log-lines"), QStringLiteral("Log lines"), network,
         QStringLiteral("Network log lines this session, by category")},
        {QStringLiteral("log-history"), QStringLiteral("Log history"), network,
         QStringLiteral("Network log lines per day over the 30-day archive")},
        {QStringLiteral("stalls"), QStringLiteral("UI stalls"), network,
         QStringLiteral("Times the GUI thread stalled this session")},

        {QStringLiteral("cpu"), QStringLiteral("CPU"), machine,
         QStringLiteral("This app's CPU share, sampled while the page is open")},
        {QStringLiteral("memory"), QStringLiteral("Memory"), machine,
         QStringLiteral("This app's resident memory, sampled while the page is open")},
        {QStringLiteral("host-memory"), QStringLiteral("Host memory"), machine,
         QStringLiteral("Memory in use across the whole machine")},
        {QStringLiteral("swap"), QStringLiteral("Swap"), machine,
         QStringLiteral("Swap in use across the whole machine")},
        {QStringLiteral("disk"), QStringLiteral("Disk free"), machine,
         QStringLiteral("Free space on the volume holding the open repository")},
        {QStringLiteral("open-files"), QStringLiteral("Open files"), machine,
         QStringLiteral("File descriptors this app holds against its soft limit")},
        {QStringLiteral("threads"), QStringLiteral("Threads"), machine,
         QStringLiteral("Threads in this process")},
        {QStringLiteral("app-uptime"), QStringLiteral("App uptime"), machine,
         QStringLiteral("Time since this app instance launched")},
        {QStringLiteral("restarts"), QStringLiteral("Restarts"), machine,
         QStringLiteral("Rebuild-and-restart and update cycles this install has timed")},
        {QStringLiteral("restart-time"), QStringLiteral("Restart time"), machine,
         QStringLiteral("How long recent restarts took, from the click to the "
                        "relaunched window's first paint")},
        {QStringLiteral("loop-lag"), QStringLiteral("Event-loop lag"), machine,
         QStringLiteral("95th percentile GUI event-loop lag from the Qt runtime diagnostics")},
        {QStringLiteral("events-per-second"), QStringLiteral("Events per second"), machine,
         QStringLiteral("Qt events dispatched per second")},
        {QStringLiteral("widgets"), QStringLiteral("Widgets"), machine,
         QStringLiteral("Live QWidget count, with the QObject total")},
        {QStringLiteral("windows"), QStringLiteral("Windows"), machine,
         QStringLiteral("Top-level windows, and how many are visible")},
        {QStringLiteral("timers"), QStringLiteral("Qt timers"), machine,
         QStringLiteral("Active Qt timers against every registered timer")},
        {QStringLiteral("thread-pool"), QStringLiteral("Thread pool"), machine,
         QStringLiteral("Global thread pool workers busy, against its maximum")},
        {QStringLiteral("diff-cache"), QStringLiteral("Diff cache"), machine,
         QStringLiteral("Memory held by cached branch diffs")},

        {QStringLiteral("packages"), QStringLiteral("Package installs"), market,
         QStringLiteral("Installs and downloads across marketplace packages")},
        {QStringLiteral("release-downloads"), QStringLiteral("Release downloads"), market,
         QStringLiteral("Downloads across the open repository's release assets")},
        {QStringLiteral("rewards"), QStringLiteral("Contributor rewards"), market,
         QStringLiteral("Reward records fetched for the reward pool, by status")},
        {QStringLiteral("ledger"), QStringLiteral("Ledger records"), market,
         QStringLiteral("Payable ledger records from the relay, and this machine's own")},
        {QStringLiteral("wallets"), QStringLiteral("Wallet balances"), market,
         QStringLiteral("Public balances of the wallets this machine names")},
        {QStringLiteral("payouts"), QStringLiteral("Settled payouts"), market,
         QStringLiteral("Rewards paid from this machine and proven on-chain")},
    };
    if (m_isAdmin) {
        catalog.append({QStringLiteral("users"), QStringLiteral("Users"), users,
                        QStringLiteral("Accounts in the directory, by activity bucket")});
        catalog.append({QStringLiteral("users-activity"), QStringLiteral("User activity"),
                        users,
                        QStringLiteral("Commits, pulls, issues and discussions "
                                       "across every account")});
        catalog.append({QStringLiteral("users-world"), QStringLiteral("World steps"), users,
                        QStringLiteral("Steps taken in World across every account, "
                                       "with office visits")});
    }
    return catalog;
}

QVector<StatsTilePlacement> MainWindow::defaultStatsLayout() const
{
    // Every statistic the catalog knows, in catalog order; the long time
    // series get two squares so their charts are readable.
    static const QSet<QString> wide{
        QStringLiteral("issues-open"), QStringLiteral("agents"),
        QStringLiteral("tool-calls"),  QStringLiteral("cpu"),
        QStringLiteral("repo-commits"), QStringLiteral("action-history"),
        QStringLiteral("pings"),       QStringLiteral("agent-tokens"),
    };
    QVector<StatsTilePlacement> layout;
    for (const StatsTileCatalogEntry &entry : statsTileCatalog()) {
        if (entry.id == QLatin1String("log-history"))
            layout.append({entry.id, 2, 2});
        else
            layout.append({entry.id, wide.contains(entry.id) ? 2 : 1, 1});
    }
    return layout;
}

StatsTileData MainWindow::statsTileData(const QString &id)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const RepositoryRecord *repo =
        m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()
            ? &m_repositories.at(m_repoDetailIndex)
            : nullptr;
    const QString repoLabel = repo ? repo->name : QString();
    const QString noRepo = QStringLiteral("Open a repository first");
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    auto tallyOf = [](const QHash<QString, double> &counts) {
        QList<QPair<QString, double>> tally;
        for (auto it = counts.cbegin(); it != counts.cend(); ++it)
            tally.append({it.key(), it.value()});
        return tally;
    };

    // ---- Workspace -------------------------------------------------------
    if (id == QLatin1String("repos")) {
        int permanent = 0, mirrored = 0, online = 0, priv = 0, preview = 0;
        for (const RepositoryRecord &record : m_repositories) {
            if (record.previewOnly) {
                ++preview;
                continue;
            }
            ++permanent;
            if (record.lastSyncMs > 0 ||
                (!record.mirrorPath.isEmpty() && QDir(record.mirrorPath).exists()))
                ++mirrored;
            if (record.publishToNetwork)
                ++online;
            if (record.isPrivate)
                ++priv;
        }
        StatsTileData data = number(
            permanent, QString::fromUtf8("%1 mirrored \xC2\xB7 %2 online")
                           .arg(mirrored).arg(online));
        data.series = orderedSeries(
            {QStringLiteral("Mirrored"), QStringLiteral("Online"),
             QStringLiteral("Private"), QStringLiteral("Preview")},
            {double(mirrored), double(online), double(priv), double(preview)});
        data.unit = QStringLiteral("repos");
        return data;
    }
    if (id == QLatin1String("repo-commits") || id == QLatin1String("repo-merges") ||
        id == QLatin1String("repo-branches") || id == QLatin1String("repo-worktrees")) {
        if (!repo)
            return unavailable(noRepo);
        const MirrorAdvert advert =
            m_localRepoAdverts.value(repo->owner + QLatin1Char('/') + repo->name);
        if (advert.commit.isEmpty())
            return unavailable(QStringLiteral("%1: no mirror advert yet").arg(repoLabel));
        if (id == QLatin1String("repo-branches")) {
            if (advert.branchCount < 0)
                return unavailable(QStringLiteral("%1: not advertised").arg(repoLabel));
            QStringList notes{repoLabel};
            if (advert.worktreeCount >= 0)
                notes << pluralNote(advert.worktreeCount, "worktree", "worktrees");
            if (advert.pullCount >= 0)
                notes << pluralNote(advert.pullCount, "pull", "pulls");
            return number(advert.branchCount, notes.join(dot));
        }
        if (id == QLatin1String("repo-worktrees")) {
            if (advert.worktreeCount < 0)
                return unavailable(QStringLiteral("%1: not advertised").arg(repoLabel));
            QStringList notes{repoLabel};
            if (advert.pullCount >= 0)
                notes << pluralNote(advert.pullCount, "pull registered", "pulls registered");
            StatsTileData data = number(advert.worktreeCount, notes.join(dot));
            data.series = orderedSeries(
                {QStringLiteral("Branches"), QStringLiteral("Worktrees"),
                 QStringLiteral("Pulls")},
                {double(qMax(0, advert.branchCount)), double(advert.worktreeCount),
                 double(qMax(0, advert.pullCount))});
            data.unit = QStringLiteral("items");
            return data;
        }
        const bool merges = id == QLatin1String("repo-merges");
        const int count = merges ? advert.mergeCount : advert.commitCount;
        if (count < 0)
            return unavailable(QStringLiteral("%1: not advertised").arg(repoLabel));
        StatsTileData data = number(
            count, QString::fromUtf8("%1 \xC2\xB7 %2").arg(repoLabel, advert.branch));
        data.series = weeklySeries(merges ? advert.mergeActivityWeeks
                                          : advert.activityWeeks);
        data.unit = merges ? QStringLiteral("merges") : QStringLiteral("commits");
        data.trend = StatsTrendPolarity::UpIsGood;
        return data;
    }
    if (id == QLatin1String("repo-size") || id == QLatin1String("repo-lines") ||
        id == QLatin1String("repo-files")) {
        const QString dir = repoGitDir();
        if (dir.isEmpty())
            return unavailable(noRepo);
        const QVector<RepoStatsSample> samples = RepoStatsStore::load(dir);
        if (samples.isEmpty())
            return unavailable(
                QStringLiteral("%1: no generated statistics yet").arg(repoLabel));
        const RepoStatsSample &last = samples.last();
        StatsTileData data;
        const int first = qMax(0, samples.size() - 30);
        for (int i = first; i < samples.size(); ++i) {
            const RepoStatsSample &sample = samples.at(i);
            data.series.labels.append(sample.day);
            if (id == QLatin1String("repo-size"))
                data.series.values.append(sample.bytes / (1024.0 * 1024.0));
            else if (id == QLatin1String("repo-lines"))
                data.series.values.append(double(sample.lines));
            else
                data.series.values.append(double(sample.files));
        }
        if (id == QLatin1String("repo-size")) {
            data.value = StatsDashboard::compactBytes(last.bytes);
            data.unit = QStringLiteral("MB");
        } else if (id == QLatin1String("repo-lines")) {
            data.value = StatsDashboard::compactNumber(double(last.lines));
            data.unit = QStringLiteral("lines");
        } else {
            data.value = StatsDashboard::compactNumber(double(last.files));
            data.unit = QStringLiteral("files");
        }
        data.note = QString::fromUtf8("%1 \xC2\xB7 as of %2").arg(repoLabel, last.day);
        return data;
    }
    if (id == QLatin1String("scm-changes")) {
        if (!repo)
            return unavailable(noRepo);
        return number(m_scmPendingChangeCount,
                      QString::fromUtf8("%1 \xC2\xB7 uncommitted rows").arg(repoLabel));
    }
    if (id == QLatin1String("teammates")) {
        if (!repo)
            return unavailable(noRepo);
        const auto loaded = forkmesh::teammates::Store(teammatesDirPath()).load();
        int scheduled = 0;
        for (const forkmesh::teammates::Config &config : loaded.teammates)
            if (config.lastRunAtMs > 0)
                ++scheduled;
        return number(loaded.teammates.size(),
                      QString::fromUtf8("%1 \xC2\xB7 %2 have run")
                          .arg(repoLabel).arg(scheduled));
    }

    // ---- Ratchet -----------------------------------------------------------
    if (id.startsWith(QLatin1String("ratchet"))) {
        const QString dir = repoGitDir();
        if (dir.isEmpty())
            return unavailable(noRepo);
        const RepoRatchetReading *reading = statsRatchetReading(dir);
        if (!reading)
            return unavailable(QString::fromUtf8("%1: reading Ratchet Mode\xE2\x80\xA6").arg(repoLabel));
        if (!reading->enabled) {
            if (id == QLatin1String("ratchet")) {
                StatsTileData data;
                data.value = QStringLiteral("Off");
                data.note = QString::fromUtf8("%1 \xC2\xB7 commits are not size-checked").arg(repoLabel);
                return data;
            }
            return unavailable(QStringLiteral("%1: Ratchet Mode is off").arg(repoLabel));
        }
        const QVector<RepoStatsSample> samples = RepoStatsStore::load(dir);
        const qint64 ceiling = reading->ceilingBytes;
        const qint64 current = samples.isEmpty() ? 0 : samples.last().bytes;
        const qint64 headroom = ceiling - current;
        const QString since = reading->ceilingDay.isEmpty()
                                  ? QStringLiteral("today")
                                  : QStringLiteral("since %1").arg(reading->ceilingDay);
        if (id == QLatin1String("ratchet")) {
            StatsTileData data;
            data.value = QStringLiteral("On");
            data.status = StatsTileStatus::Good;
            data.note = QString::fromUtf8("ceiling %1 \xC2\xB7 %2")
                            .arg(StatsDashboard::compactBytes(ceiling), since);
            data.series = orderedSeries(
                {QStringLiteral("Used"), QStringLiteral("Headroom")},
                {current / (1024.0 * 1024.0),
                 qMax<qint64>(0, headroom) / (1024.0 * 1024.0)});
            data.unit = QStringLiteral("MB");
            return data;
        }
        StatsTileData data;
        const int first = qMax(0, samples.size() - 30);
        for (int i = first; i < samples.size(); ++i) {
            data.series.labels.append(samples.at(i).day);
            const qint64 bytes = id == QLatin1String("ratchet-ceiling")
                                     ? samples.at(i).bytes
                                     : ceiling - samples.at(i).bytes;
            data.series.values.append(bytes / (1024.0 * 1024.0));
        }
        data.unit = QStringLiteral("MB");
        if (id == QLatin1String("ratchet-ceiling")) {
            data.value = StatsDashboard::compactBytes(ceiling);
            data.note = QString::fromUtf8("repository at %1 \xC2\xB7 %2")
                            .arg(StatsDashboard::compactBytes(current), since);
            return data;
        }
        data.value = headroom >= 0
                         ? StatsDashboard::compactBytes(headroom)
                         : QString::fromUtf8("\xE2\x88\x92") + StatsDashboard::compactBytes(-headroom);
        data.note = ceiling > 0
                        ? QString::fromUtf8("%1% of the ceiling used \xC2\xB7 %2")
                              .arg(qRound(100.0 * double(current) / double(ceiling)))
                              .arg(repoLabel)
                        : QStringLiteral("no ceiling recorded yet");
        data.trend = StatsTrendPolarity::UpIsGood;
        if (ceiling > 0)
            data.status = levelStatus(100.0 * double(current) / double(ceiling), 80, 95);
        return data;
    }

    // ---- Issues & pulls ----------------------------------------------------
    if (id == QLatin1String("issues-open") || id == QLatin1String("issues-closed")) {
        if (!repo)
            return unavailable(noRepo);
        int open = 0, closed = 0;
        for (const Issue &issue : m_currentIssues) {
            if (issue.isDeleted())
                continue;
            if (issue.status == QLatin1String("closed"))
                ++closed;
            else
                ++open;
        }
        const bool wantOpen = id == QLatin1String("issues-open");
        StatsTileData data = number(
            wantOpen ? open : closed,
            QString::fromUtf8("%1 \xC2\xB7 %2 %3 \xC2\xB7 %4 total")
                .arg(repoLabel)
                .arg(wantOpen ? closed : open)
                .arg(wantOpen ? QStringLiteral("closed") : QStringLiteral("open"))
                .arg(open + closed));
        const QList<IssueBurnupPoint> points =
            buildIssueBurnupSeries(m_currentIssues, now - 30 * kDayMs, now, 30);
        for (const IssueBurnupPoint &point : points) {
            data.series.labels.append(dayLabel(point.timestampMs));
            data.series.values.append(double(wantOpen ? point.openCount
                                                      : point.closedCount));
        }
        data.unit = QStringLiteral("issues");
        data.trend = wantOpen ? StatsTrendPolarity::DownIsGood : StatsTrendPolarity::UpIsGood;
        return data;
    }
    if (id.startsWith(QLatin1String("issues-"))) {
        if (!repo)
            return unavailable(noRepo);
        int open = 0;
        QVector<double> bands(4, 0.0); // top, mid, low, unset
        QVector<double> quarters(4, 0.0);
        QHash<QString, double> labels, assignees, rewardStatus;
        QList<QPair<QString, double>> votes, bounties;
        int labelled = 0, assigned = 0, reporting = 0, bountied = 0, estimated = 0,
            paid = 0;
        double progress = 0, voteTotal = 0, bountyTotal = 0, rewardLamports = 0;
        for (const Issue &issue : m_currentIssues) {
            if (issue.isDeleted())
                continue;
            if (issue.rewardEstimateLamports > 0) {
                ++estimated;
                rewardLamports += double(issue.rewardEstimateLamports);
                const QString status = issue.rewardStatus.isEmpty()
                                           ? QStringLiteral("pending")
                                           : issue.rewardStatus;
                rewardStatus[status] += 1;
                if (status == QLatin1String("paid"))
                    ++paid;
            }
            if (issue.status == QLatin1String("closed"))
                continue;
            ++open;
            if (issue.priority <= 0)
                bands[3] += 1;
            else if (issue.priority <= 1000)
                bands[0] += 1;
            else if (issue.priority <= 4000)
                bands[1] += 1;
            else
                bands[2] += 1;
            if (!issue.labels.isEmpty())
                ++labelled;
            for (const QString &label : issue.labels)
                labels[label] += 1;
            if (!issue.assignees.isEmpty())
                ++assigned;
            for (const QString &assignee : issue.assignees)
                assignees[assignee] += 1;
            if (issue.progress > 0) {
                ++reporting;
                progress += issue.progress;
                quarters[qBound(0, (issue.progress - 1) / 25, 3)] += 1;
            }
            if (issue.votes > 0) {
                voteTotal += issue.votes;
                votes.append({QStringLiteral("#%1").arg(issue.number), double(issue.votes)});
            }
            if (issue.bountyUsd > 0) {
                ++bountied;
                bountyTotal += issue.bountyUsd;
                bounties.append({QStringLiteral("#%1").arg(issue.number), issue.bountyUsd});
            }
        }
        if (id == QLatin1String("issues-priority")) {
            StatsTileData data = number(open, QStringLiteral("%1 open").arg(repoLabel));
            data.series = orderedSeries({QStringLiteral("Top"), QStringLiteral("Mid"),
                                         QStringLiteral("Low"), QStringLiteral("Unset")},
                                        bands);
            data.series.tones = {StatsTileStatus::Critical, StatsTileStatus::Warning,
                                 StatsTileStatus::None, StatsTileStatus::None};
            data.unit = QStringLiteral("issues");
            return data;
        }
        if (id == QLatin1String("issues-labels")) {
            StatsTileData data = number(
                labels.size(), QStringLiteral("%1 of %2 open issues labelled")
                                   .arg(labelled).arg(open));
            data.series = tallySeries(tallyOf(labels), 8);
            data.unit = QStringLiteral("issues");
            return data;
        }
        if (id == QLatin1String("issues-assigned")) {
            StatsTileData data = number(
                assigned, QString::fromUtf8("%1 unassigned \xC2\xB7 %2 assignees")
                              .arg(open - assigned).arg(assignees.size()));
            data.series = tallySeries(tallyOf(assignees), 8);
            data.unit = QStringLiteral("issues");
            return data;
        }
        if (id == QLatin1String("issues-progress")) {
            if (reporting == 0)
                return unavailable(QStringLiteral("%1: no issue reports progress").arg(repoLabel));
            StatsTileData data;
            data.value = QStringLiteral("%1%").arg(qRound(progress / reporting));
            data.note = QStringLiteral("average over %1 issues reporting").arg(reporting);
            data.series = orderedSeries(
                {QStringLiteral("<25%"), QStringLiteral("<50%"),
                 QStringLiteral("<75%"), QStringLiteral("<100%")},
                quarters);
            data.unit = QStringLiteral("issues");
            return data;
        }
        if (id == QLatin1String("issues-votes")) {
            StatsTileData data = number(
                voteTotal, QString::fromUtf8("%1 \xC2\xB7 %2 open issues voted on")
                               .arg(repoLabel).arg(votes.size()));
            data.series = tallySeries(votes, 8);
            data.unit = QStringLiteral("votes");
            return data;
        }
        if (id == QLatin1String("issues-bounties")) {
            StatsTileData data;
            data.value = StatsDashboard::compactUsd(bountyTotal);
            data.note = QString::fromUtf8("%1 \xC2\xB7 %2 open issues with a bounty")
                            .arg(repoLabel).arg(bountied);
            data.series = tallySeries(bounties, 8);
            data.unit = QStringLiteral("USD");
            return data;
        }
        // issues-rewards
        if (estimated == 0)
            return unavailable(QStringLiteral("%1: no reward estimates yet").arg(repoLabel));
        StatsTileData data;
        data.value = solText(rewardLamports);
        data.note = QString::fromUtf8("%1 issues estimated \xC2\xB7 %2 paid").arg(estimated).arg(paid);
        data.series = tallySeries(tallyOf(rewardStatus), 6);
        data.unit = QStringLiteral("issues");
        return data;
    }
    if (id == QLatin1String("pulls") || id == QLatin1String("pulls-conflicts")) {
        if (!repo)
            return unavailable(noRepo);
        int open = 0, merged = 0, closed = 0, conflicting = 0, probed = 0;
        for (const PullRequest &pull : m_currentPulls) {
            if (pull.status == QLatin1String("merged")) {
                ++merged;
            } else if (pull.status == QLatin1String("closed")) {
                ++closed;
            } else {
                ++open;
                const auto probe = m_pullConflictByNumber.constFind(pull.number);
                if (probe != m_pullConflictByNumber.cend()) {
                    ++probed;
                    if (probe.value())
                        ++conflicting;
                }
            }
        }
        if (id == QLatin1String("pulls-conflicts")) {
            StatsTileData data = number(
                conflicting, QString::fromUtf8("%1 \xC2\xB7 %2 of %3 open pulls probed")
                                 .arg(repoLabel).arg(probed).arg(open));
            data.series = orderedSeries(
                {QStringLiteral("Clean"), QStringLiteral("Conflicts"),
                 QStringLiteral("Unprobed")},
                {double(probed - conflicting), double(conflicting), double(open - probed)});
            if (conflicting > 0)
                data.status = StatsTileStatus::Warning;
            else if (probed > 0)
                data.status = StatsTileStatus::Good;
            data.unit = QStringLiteral("pulls");
            return data;
        }
        StatsTileData data = number(
            open, QString::fromUtf8("%1 \xC2\xB7 %2 merged \xC2\xB7 %3 closed")
                      .arg(repoLabel).arg(merged).arg(closed));
        data.series = orderedSeries(
            {QStringLiteral("Open"), QStringLiteral("Merged"), QStringLiteral("Closed")},
            {double(open), double(merged), double(closed)});
        data.unit = QStringLiteral("pulls");
        return data;
    }
    if (id == QLatin1String("discussions")) {
        if (!repo)
            return unavailable(noRepo);
        int open = 0, closed = 0, archived = 0, comments = 0;
        for (const Discussion &discussion : m_currentDiscussions) {
            comments += discussion.commentCount();
            if (discussion.status == QLatin1String("closed"))
                ++closed;
            else if (discussion.status == QLatin1String("archived"))
                ++archived;
            else
                ++open;
        }
        StatsTileData data = number(
            m_currentDiscussions.size(),
            QString::fromUtf8("%1 \xC2\xB7 %2 open \xC2\xB7 %3")
                .arg(repoLabel).arg(open).arg(pluralNote(comments, "comment", "comments")));
        data.series = orderedSeries(
            {QStringLiteral("Open"), QStringLiteral("Closed"), QStringLiteral("Archived")},
            {double(open), double(closed), double(archived)});
        data.unit = QStringLiteral("discussions");
        return data;
    }

    // ---- Chat & pings ------------------------------------------------------
    if (id == QLatin1String("chat-unread")) {
        int total = 0;
        QList<QPair<QString, double>> tally;
        for (auto it = m_unreadCounts.cbegin(); it != m_unreadCounts.cend(); ++it) {
            total += it.value();
            tally.append({it.key(), double(it.value())});
        }
        StatsTileData data = number(
            total, pluralNote(m_channels.size(), "channel", "channels"));
        data.series = tallySeries(tally, 6);
        data.unit = QStringLiteral("unread");
        return data;
    }
    if (id == QLatin1String("chat-channels"))
        return number(m_channels.size(), pluralNote(m_homeRoster.size(), "member",
                                                    "members on the roster"));
    if (id == QLatin1String("pings") || id == QLatin1String("pings-kinds")) {
        int warnings = 0, pending = 0;
        QHash<QString, double> kinds;
        for (const AppNotification &item : m_notifications) {
            if (item.warning)
                ++warnings;
            if (item.dismissOnly && !item.acknowledged)
                ++pending;
            kinds[item.kind.isEmpty() ? QStringLiteral("other") : item.kind] += 1;
        }
        if (id == QLatin1String("pings-kinds")) {
            StatsTileData data = number(
                kinds.size(), QStringLiteral("kinds across %1").arg(
                                  pluralNote(m_notifications.size(), "ping", "pings")));
            data.series = tallySeries(tallyOf(kinds), 8);
            data.unit = QStringLiteral("pings");
            return data;
        }
        StatsTileData data = number(
            m_notifications.size(),
            QString::fromUtf8("%1 warnings \xC2\xB7 %2 awaiting dismiss").arg(warnings).arg(pending));
        data.series = dailySeries(
            m_notifications, 14,
            [](const AppNotification &item) { return item.timestampMs; },
            [](const AppNotification &) { return 1.0; });
        data.unit = QStringLiteral("pings");
        if (pending > 0)
            data.status = StatsTileStatus::Warning;
        return data;
    }
    if (id == QLatin1String("roster-versions")) {
        QHash<QString, double> versions;
        int online = 0;
        for (const MemberInfo &member : m_homeRoster) {
            if (member.online)
                ++online;
            versions[member.version.isEmpty() ? QStringLiteral("unknown") : member.version] += 1;
        }
        StatsTileData data = number(
            versions.size(), QString::fromUtf8("versions across %1 \xC2\xB7 %2 online")
                                 .arg(pluralNote(m_homeRoster.size(), "member", "members"))
                                 .arg(online));
        data.series = tallySeries(tallyOf(versions), 8);
        data.unit = QStringLiteral("members");
        return data;
    }
    if (id == QLatin1String("tasks")) {
        const QJsonArray catalog = m_organizationTaskStore
                                       ? m_organizationTaskStore->tasks()
                                       : QJsonArray();
        QHash<QString, int> byStatus;
        for (const QJsonValue &value : catalog) {
            QString status = value.toObject().value(QStringLiteral("status")).toString();
            if (status.isEmpty())
                status = QStringLiteral("open");
            byStatus[status] += 1;
        }
        QList<QPair<QString, double>> tally;
        for (auto it = byStatus.cbegin(); it != byStatus.cend(); ++it)
            tally.append({it.key(), double(it.value())});
        StatsTileData data = number(catalog.size(),
                                    QStringLiteral("organization board"));
        data.series = tallySeries(tally, 6);
        data.unit = QStringLiteral("tasks");
        return data;
    }

    // ---- Agents ------------------------------------------------------------
    if (id == QLatin1String("agents")) {
        int running = 0;
        QHash<QString, int> byStatus;
        for (const AgentSession &session : m_agentSessions) {
            const QString label = session.merged ? QStringLiteral("Merged")
                                                 : agentStatusText(session.status);
            byStatus[label] += 1;
            if (!session.merged && session.status == AgentStatus::Running)
                ++running;
        }
        QList<QPair<QString, double>> tally;
        for (auto it = byStatus.cbegin(); it != byStatus.cend(); ++it)
            tally.append({it.key(), double(it.value())});
        StatsTileData data = number(m_agentSessions.size(),
                                    QStringLiteral("%1 running").arg(running));
        data.series = tallySeries(tally, 7);
        data.unit = QStringLiteral("sessions");
        return data;
    }
    if (id == QLatin1String("agents-running")) {
        int running = 0, queued = 0;
        for (const AgentSession &session : m_agentSessions) {
            if (session.merged)
                continue;
            if (session.status == AgentStatus::Running)
                ++running;
            else if (session.status == AgentStatus::Queued)
                ++queued;
        }
        StatsTileData data = number(running, QStringLiteral("%1 queued").arg(queued));
        data.series = dailySeries(
            m_agentSessions, 14,
            [](const AgentSession &s) {
                return s.startedAtMs > 0 ? s.startedAtMs : s.createdAtMs;
            },
            [](const AgentSession &) { return 1.0; });
        data.unit = QStringLiteral("started");
        return data;
    }
    if (id == QLatin1String("agent-success")) {
        int finished = 0, succeeded = 0, failed = 0, stopped = 0;
        for (const AgentSession &session : m_agentSessions) {
            if (!session.merged && !AgentStatus::isTerminal(session.status))
                continue;
            ++finished;
            if (session.merged || session.status == AgentStatus::Success)
                ++succeeded;
            else if (session.status == AgentStatus::Failed)
                ++failed;
            else
                ++stopped;
        }
        if (finished == 0)
            return unavailable(QStringLiteral("No finished sessions yet"));
        StatsTileData data;
        data.value = QStringLiteral("%1%").arg(qRound(100.0 * succeeded / finished));
        data.status = floorStatus(100.0 * succeeded / finished, 80, 50);
        data.note = QStringLiteral("%1 of %2 finished sessions").arg(succeeded).arg(finished);
        data.series = orderedSeries(
            {QStringLiteral("Succeeded"), QStringLiteral("Failed"), QStringLiteral("Stopped")},
            {double(succeeded), double(failed), double(stopped)});
        data.unit = QStringLiteral("sessions");
        return data;
    }
    if (id == QLatin1String("agent-tokens")) {
        double total = 0;
        for (const AgentSession &session : m_agentSessions)
            total += double(sessionTokenTotal(session));
        StatsTileData data = number(
            total, pluralNote(m_agentSessions.size(), "session", "sessions"));
        data.series = dailySeries(
            m_agentSessions, 14,
            [](const AgentSession &s) { return s.createdAtMs; },
            [this](const AgentSession &s) { return double(sessionTokenTotal(s)); });
        data.unit = QStringLiteral("tokens");
        return data;
    }
    if (id == QLatin1String("agent-spend")) {
        double total = 0;
        for (const AgentSession &session : m_agentSessions)
            total += session.costUsd;
        StatsTileData data;
        data.value = StatsDashboard::compactUsd(total);
        QStringList notes{QStringLiteral("estimated from sessions")};
        const bool openAiKnown = !std::isnan(m_openAiSpendUsd);
        const bool claudeKnown = !std::isnan(m_claudeSpendUsd);
        if (openAiKnown || claudeKnown)
            notes = {QStringLiteral("%1 billed this month")
                         .arg(StatsDashboard::compactUsd(
                             (openAiKnown ? m_openAiSpendUsd : 0.0) +
                             (claudeKnown ? m_claudeSpendUsd : 0.0)))};
        data.note = notes.join(dot);
        data.series = dailySeries(
            m_agentSessions, 14,
            [](const AgentSession &s) { return s.createdAtMs; },
            [](const AgentSession &s) { return s.costUsd; });
        data.unit = QStringLiteral("USD");
        return data;
    }
    if (id == QLatin1String("agent-turns")) {
        double total = 0;
        int counted = 0;
        for (const AgentSession &session : m_agentSessions)
            if (session.numTurns > 0) {
                total += session.numTurns;
                ++counted;
            }
        if (counted == 0)
            return unavailable(QStringLiteral("No session has reported turns yet"));
        StatsTileData data = number(
            total, QStringLiteral("%1 per session over %2")
                       .arg(qRound(total / counted))
                       .arg(pluralNote(counted, "session", "sessions")));
        data.series = dailySeries(
            m_agentSessions, 14,
            [](const AgentSession &s) { return s.createdAtMs; },
            [](const AgentSession &s) { return double(qMax(0, s.numTurns)); });
        data.unit = QStringLiteral("turns");
        return data;
    }
    if (id == QLatin1String("agent-runtime")) {
        qint64 total = 0;
        int finished = 0;
        qint64 finishedTotal = 0;
        QVector<double> recent;
        QStringList recentLabels;
        for (const AgentSession &session : m_agentSessions) { // newest first
            const qint64 ran = session.totalRunMs(now);
            total += ran;
            if (session.merged || AgentStatus::isTerminal(session.status)) {
                ++finished;
                finishedTotal += ran;
                if (recent.size() < 20 && ran > 0) {
                    recent.prepend(ran / 1000.0);
                    recentLabels.prepend(QStringLiteral("#%1").arg(session.id));
                }
            }
        }
        if (total <= 0)
            return unavailable(QStringLiteral("No session has run yet"));
        StatsTileData data;
        data.value = StatsDashboard::compactDuration(total);
        data.note = finished > 0
                        ? QStringLiteral("%1 average over %2")
                              .arg(StatsDashboard::compactDuration(finishedTotal / finished))
                              .arg(pluralNote(finished, "finished session", "finished sessions"))
                        : QStringLiteral("no session has finished yet");
        data.series.labels = recentLabels;
        data.series.values = recent;
        data.unit = QStringLiteral("s");
        return data;
    }
    if (id == QLatin1String("agent-prs")) {
        int opened = 0, merged = 0, open = 0;
        for (const AgentSession &session : m_agentSessions) {
            if (session.prNumber <= 0)
                continue;
            ++opened;
            if (session.merged)
                ++merged;
            else
                ++open;
        }
        StatsTileData data = number(
            opened, QString::fromUtf8("%1 merged \xC2\xB7 %2 still open").arg(merged).arg(open));
        data.series = orderedSeries(
            {QStringLiteral("Opened"), QStringLiteral("Merged"), QStringLiteral("Open")},
            {double(opened), double(merged), double(open)});
        data.unit = QStringLiteral("pulls");
        return data;
    }
    if (id == QLatin1String("agent-errors")) {
        int errored = 0;
        QString newest;
        for (const AgentSession &session : m_agentSessions) { // newest first
            if (session.lastError.trimmed().isEmpty())
                continue;
            ++errored;
            if (newest.isEmpty())
                newest = session.lastError.simplified();
        }
        StatsTileData data = number(
            errored, newest.isEmpty() ? QStringLiteral("no session reports an error")
                                      : newest);
        data.series = dailySeries(
            m_agentSessions, 14,
            [](const AgentSession &s) {
                return s.finishedAtMs > 0 ? s.finishedAtMs : s.createdAtMs;
            },
            [](const AgentSession &s) {
                return s.lastError.trimmed().isEmpty() ? 0.0 : 1.0;
            });
        data.unit = QStringLiteral("errors");
        data.trend = StatsTrendPolarity::DownIsGood;
        double fortnight = 0;
        for (double count : data.series.values)
            fortnight += count;
        data.status = fortnight > 0 ? StatsTileStatus::Warning : StatsTileStatus::Good;
        return data;
    }
    if (id == QLatin1String("agents-providers") || id == QLatin1String("agents-models") ||
        id == QLatin1String("agent-skills") || id == QLatin1String("agent-grades") ||
        id == QLatin1String("agent-teammates")) {
        QHash<QString, double> providers, models, skills, grades, teammates;
        int graded = 0, teammateRuns = 0;
        double gradeTotal = 0;
        for (const AgentSession &session : m_agentSessions) {
            providers[session.provider.isEmpty() ? QStringLiteral("unknown")
                                                 : session.provider] += 1;
            models[session.model.isEmpty() ? QStringLiteral("unknown") : session.model] += 1;
            for (const QString &skill : session.skillsUsed)
                skills[skill] += 1;
            if (session.taskGrade > 0) {
                ++graded;
                gradeTotal += session.taskGrade;
                grades[QString::number(session.taskGrade)] += 1;
            }
            if (session.hasTeammateAttribution()) {
                ++teammateRuns;
                teammates[session.teammateName.isEmpty() ? QStringLiteral("special agent")
                                                         : session.teammateName] += 1;
            }
        }
        if (m_agentSessions.isEmpty())
            return unavailable(QStringLiteral("No agent sessions yet"));
        StatsTileData data;
        if (id == QLatin1String("agents-providers")) {
            data = number(providers.size(),
                          QStringLiteral("providers across %1")
                              .arg(pluralNote(m_agentSessions.size(), "session", "sessions")));
            data.series = tallySeries(tallyOf(providers), 6);
        } else if (id == QLatin1String("agents-models")) {
            data = number(models.size(),
                          QStringLiteral("models across %1")
                              .arg(pluralNote(m_agentSessions.size(), "session", "sessions")));
            data.series = tallySeries(tallyOf(models), 8);
        } else if (id == QLatin1String("agent-skills")) {
            if (skills.isEmpty())
                return unavailable(QStringLiteral("No session has reported a skill yet"));
            double uses = 0;
            for (double count : skills)
                uses += count;
            data = number(skills.size(), QString::fromUtf8("skills \xC2\xB7 %1 uses")
                                             .arg(StatsDashboard::compactNumber(uses)));
            data.series = tallySeries(tallyOf(skills), 8);
        } else if (id == QLatin1String("agent-grades")) {
            if (graded == 0)
                return unavailable(QStringLiteral("No task has been graded yet"));
            data.value = QString::number(gradeTotal / graded, 'f', 1);
            data.note = QStringLiteral("average over %1").arg(
                pluralNote(graded, "graded task", "graded tasks"));
            data.series = tallySeries(tallyOf(grades), 8);
        } else {
            data = number(teammateRuns,
                          QStringLiteral("sessions started by %1").arg(
                              pluralNote(teammates.size(), "special agent", "special agents")));
            data.series = tallySeries(tallyOf(teammates), 8);
        }
        data.unit = QStringLiteral("sessions");
        return data;
    }
    if (id == QLatin1String("claude-usage")) {
        if (m_claudeUsageLast5hPct < 0 && m_claudeUsageLastWeekPct < 0)
            return unavailable(QStringLiteral("No Claude usage reading yet"));
        StatsTileData data;
        const int five = qMax(0, m_claudeUsageLast5hPct);
        const int week = qMax(0, m_claudeUsageLastWeekPct);
        data.value = QStringLiteral("%1%").arg(m_claudeUsageLast5hPct >= 0 ? five : week);
        data.note = m_claudeUsageLast5hPct >= 0
                        ? QString::fromUtf8("5-hour window \xC2\xB7 %1% weekly").arg(week)
                        : QStringLiteral("weekly window");
        data.series = orderedSeries({QStringLiteral("5-hour"), QStringLiteral("Weekly")},
                                    {double(five), double(week)});
        data.series.tones = {levelStatus(five, 70, 90), levelStatus(week, 70, 90)};
        data.status = levelStatus(qMax(five, week), 70, 90);
        data.unit = QStringLiteral("%");
        return data;
    }

    // ---- Realtime ----------------------------------------------------------
    // Each of these reads its right-now number and charts the rolling samples
    // taken on every refresh (see sampleStatsHistory).
    if (id == QLatin1String("agents-live") || id == QLatin1String("agent-queue") ||
        id == QLatin1String("agent-background")) {
        int running = 0, queued = 0, waiting = 0, background = 0, busy = 0;
        for (const AgentSession &session : m_agentSessions) {
            if (session.merged)
                continue;
            if (session.status == AgentStatus::Running)
                ++running;
            else if (session.status == AgentStatus::Queued)
                ++queued;
            else if (session.status == AgentStatus::Waiting)
                ++waiting;
            background += session.backgroundProcesses;
            if (session.backgroundProcessesBusy)
                ++busy;
        }
        StatsTileData data;
        if (id == QLatin1String("agents-live")) {
            data = number(running, QString::fromUtf8("%1 queued \xC2\xB7 %2 waiting")
                                       .arg(queued).arg(waiting));
            data.unit = QStringLiteral("running");
        } else if (id == QLatin1String("agent-queue")) {
            data = number(queued + waiting,
                          QString::fromUtf8("%1 queued \xC2\xB7 %2 waiting \xC2\xB7 %3 running")
                              .arg(queued).arg(waiting).arg(running));
            data.unit = QStringLiteral("queued");
        } else {
            data = number(background, QStringLiteral("%1 sessions still burning CPU").arg(busy));
            data.unit = QStringLiteral("processes");
        }
        data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
        if (id != QLatin1String("agents-live"))
            data.trend = StatsTrendPolarity::DownIsGood;
        return data;
    }
    if (id == QLatin1String("agent-context")) {
        QList<QPair<QString, double>> tally;
        double fullest = 0;
        int window = 0;
        for (const AgentSession &session : m_agentSessions) {
            if (session.merged || session.status != AgentStatus::Running ||
                session.contextWindow <= 0)
                continue;
            const double pct = 100.0 * double(session.contextTokens) / double(session.contextWindow);
            if (pct > fullest) {
                fullest = pct;
                window = session.contextWindow;
            }
            tally.append({QStringLiteral("#%1").arg(session.id), pct});
        }
        if (tally.isEmpty())
            return unavailable(QStringLiteral("No running session reports its context"));
        StatsTileData data;
        data.value = percentText(fullest);
        data.note = QString::fromUtf8("fullest of %1 \xC2\xB7 window %2")
                        .arg(pluralNote(tally.size(), "running session", "running sessions"),
                             StatsDashboard::compactNumber(window));
        data.series = tallySeries(tally, 8);
        data.series.tones.clear();
        for (double pct : data.series.values)
            data.series.tones.append(levelStatus(pct, 60, 85));
        data.status = levelStatus(fullest, 60, 85);
        data.unit = QStringLiteral("% of window");
        return data;
    }
    if (id == QLatin1String("compilers")) {
        StatsTileData data = number(
            m_agentProcessMetaCompilerCount,
            m_agentProcessMetaSampledAtMs > 0
                ? QString::fromUtf8("%1 resident \xC2\xB7 under the agents")
                      .arg(StatsDashboard::compactBytes(m_agentProcessMetaCompilerResidentBytes))
                : QStringLiteral("no agent process sampled yet"));
        data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
        data.unit = QStringLiteral("compilers");
        return data;
    }
    if (id == QLatin1String("relay-latency")) {
        const QString active = m_activeServer >= 0 && m_activeServer < m_servers.size()
                                   ? hostOf(m_servers.at(m_activeServer).url)
                                   : QString();
        const RelayLatencySample current = m_relayHostLatency.value(active);
        QList<QPair<QString, double>> tally;
        for (auto it = m_relayHostLatency.cbegin(); it != m_relayHostLatency.cend(); ++it)
            if (it.value().ms > 0)
                tally.append({it.key(), double(it.value().ms)});
        if (current.stampMs <= 0 && tally.isEmpty())
            return unavailable(QStringLiteral("No relay measured yet"));
        StatsTileData data;
        data.value = current.ms > 0 ? QStringLiteral("%1 ms").arg(current.ms)
                                    : QString::fromUtf8("\xE2\x80\x94");
        data.note = active.isEmpty() ? QStringLiteral("no active relay")
                                     : (current.ms > 0 ? active
                                                       : active + dot + QStringLiteral("no answer"));
        data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
        data.unit = QStringLiteral("ms");
        data.trend = StatsTrendPolarity::DownIsGood;
        if (current.ms > 0)
            data.status = levelStatus(current.ms, 150, 500);
        else if (current.stampMs > 0)
            data.status = StatsTileStatus::Critical;
        return data;
    }
    if (id == QLatin1String("network-requests")) {
        using Destination = BackoffNetworkAccessManager::DestinationStats;
        const auto *manager =
            qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
        const QList<Destination> destinations =
            manager ? manager->destinationStats() : QList<Destination>();
        const bool firewallOn = manager && manager->firewallEnabled();
        qint64 allowed = 0, blocked = 0;
        for (const Destination &row : destinations) {
            allowed += row.allowed;
            blocked += row.blocked;
        }
        StatsTileData data = number(
            double(m_debugNetworkRequestTotal),
            QString::fromUtf8("this session \xC2\xB7 %1 allowed \xC2\xB7 %2 blocked \xC2\xB7 %3")
                .arg(StatsDashboard::compactNumber(double(allowed)),
                     StatsDashboard::compactNumber(double(blocked)),
                     pluralNote(destinations.size(), "destination", "destinations")));
        if (destinations.isEmpty()) {
            data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
            data.unit = QStringLiteral("per refresh");
            return data;
        }
        // The busiest destinations as bars, each coloured by what the firewall
        // says about it now: green is allowed by a rule, red is denied by one,
        // amber has no decision yet (it asks, or was denied just once), and the
        // accent is traffic the firewall does not judge.
        StatsTileSeries series;
        series.categorical = true;
        for (int i = 0; i < destinations.size() && i < 8; ++i) {
            const Destination &row = destinations.at(i);
            QString status;
            StatsTileStatus tone = StatsTileStatus::None;
            if (row.onThisMachine) {
                status = QStringLiteral("this machine");
            } else if (!firewallOn) {
                status = QStringLiteral("firewall off");
            } else if (row.match == BackoffNetworkAccessManager::FirewallDenied) {
                status = QStringLiteral("denied by rule");
                tone = StatsTileStatus::Critical;
            } else if (row.match == BackoffNetworkAccessManager::FirewallAllowed) {
                status = QStringLiteral("allowed by rule");
                tone = StatsTileStatus::Good;
            } else {
                status = row.blocked > 0 ? QStringLiteral("no rule, denied once")
                                         : QStringLiteral("no rule, asks");
                tone = StatsTileStatus::Warning;
            }
            series.labels.append(row.host);
            series.values.append(double(row.calls));
            series.tones.append(tone);
            series.notes.append(
                QString::fromUtf8("%1 \xC2\xB7 %2 allowed \xC2\xB7 %3 blocked \xC2\xB7 "
                                  "click for its log lines")
                    .arg(status).arg(row.allowed).arg(row.blocked));
        }
        data.series = series;
        data.unit = QStringLiteral("requests");
        return data;
    }
    if (id == QLatin1String("background-tasks")) {
        int tickets = 0, runs = 0, failures = 0;
        for (int count : m_backgroundTaskCounts)
            tickets += count;
        for (const BackgroundOutcomeTally &tally : m_backgroundTaskDone) {
            runs += tally.runs;
            failures += tally.failures;
        }
        StatsTileData data = number(
            tickets, QString::fromUtf8("open \xC2\xB7 %1 finished \xC2\xB7 %2 failed")
                         .arg(runs).arg(failures));
        data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
        data.unit = QStringLiteral("tickets");
        data.trend = StatsTrendPolarity::DownIsGood;
        if (failures > 0)
            data.status = StatsTileStatus::Warning;
        return data;
    }
    if (id == QLatin1String("loop-busy")) {
        if (!m_qtDiagnostics)
            return unavailable(QStringLiteral("Qt runtime diagnostics are off"));
        const auto snapshot = m_qtDiagnostics->snapshot();
        StatsTileData data;
        data.value = percentText(snapshot.dispatcherBusyPercent);
        data.note = QString::fromUtf8("dispatcher busy \xC2\xB7 %1 wakeups/s")
                        .arg(snapshot.wakeupsPerSecond);
        data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
        data.unit = QStringLiteral("%");
        data.trend = StatsTrendPolarity::DownIsGood;
        data.status = levelStatus(snapshot.dispatcherBusyPercent, 30, 70);
        return data;
    }

    // ---- Agent tools -------------------------------------------------------
    if (id.startsWith(QLatin1String("tool")) || id == QLatin1String("tokens-saved") ||
        id == QLatin1String("context-injected")) {
        if (!m_agentToolLedger || !m_agentToolLedger->hasSamples())
            return unavailable(QStringLiteral("No tool calls recorded yet"));
        const AgentToolLedgerTotals totals = m_agentToolLedger->totals(now);
        // Error share of every call: under 5% operates well, under 15% needs attention.
        const StatsTileStatus errorStatus =
            totals.calls > 0
                ? levelStatus(100.0 * double(totals.errors) / double(totals.calls), 5, 15)
                : StatsTileStatus::None;
        if (id == QLatin1String("tool-calls")) {
            StatsTileData data = number(
                double(totals.calls),
                QString::fromUtf8("%1 errors \xC2\xB7 %2 in flight")
                    .arg(totals.errors).arg(totals.inFlight));
            data.series = bucketSeries(totals.recentCalls, 5 * 60 * 1000);
            data.status = errorStatus;
            data.unit = QStringLiteral("calls");
            return data;
        }
        if (id == QLatin1String("tool-in-flight")) {
            StatsTileData data = number(
                double(totals.inFlight),
                QString::fromUtf8("of %1 calls \xC2\xB7 last call %2")
                    .arg(StatsDashboard::compactNumber(double(totals.calls)),
                         totals.lastCallMs > 0
                             ? StatsDashboard::compactDuration(qMax<qint64>(0, now - totals.lastCallMs)) +
                                   QStringLiteral(" ago")
                             : QStringLiteral("never")));
            data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
            data.unit = QStringLiteral("in flight");
            return data;
        }
        if (id == QLatin1String("tokens-saved")) {
            StatsTileData data = number(
                double(totals.savedTokens),
                QString::fromUtf8("\xE2\x89\x88 %1 saved")
                    .arg(StatsDashboard::compactUsd(totals.savedUsd)));
            data.series = bucketSeries(totals.recentSaved, 5 * 60 * 1000);
            data.trend = StatsTrendPolarity::UpIsGood;
            data.unit = QStringLiteral("tokens");
            return data;
        }
        if (id == QLatin1String("context-injected"))
            return number(double(totals.consumedTokens),
                          QString::fromUtf8("\xE2\x89\x88 %1 of context")
                              .arg(StatsDashboard::compactUsd(totals.consumedUsd)));
        const QVector<AgentToolFamilyStats> families = m_agentToolLedger->allStats();
        if (id == QLatin1String("tool-latency")) {
            qint64 totalMs = 0, samples = 0, slowest = 0;
            for (const AgentToolFamilyStats &family : families) {
                totalMs += family.latencyMsTotal;
                samples += family.latencySamples;
                slowest = qMax(slowest, family.latencyMsMax);
            }
            if (samples == 0)
                return unavailable(QStringLiteral("No measured calls yet"));
            const double average = double(totalMs) / double(samples);
            StatsTileData data;
            data.value = average < 1000 ? QStringLiteral("%1 ms").arg(qRound(average))
                                        : QStringLiteral("%1 s").arg(average / 1000.0, 0, 'f', 1);
            data.note = QString::fromUtf8("%1 measured \xC2\xB7 slowest %2")
                            .arg(StatsDashboard::compactNumber(double(samples)),
                                 StatsDashboard::compactDuration(slowest));
            QList<QPair<QString, double>> tally;
            for (const AgentToolFamilyStats &family : families)
                if (family.latencySamples > 0)
                    tally.append({family.family, family.averageLatencyMs()});
            data.series = tallySeries(tally, 8);
            data.unit = QStringLiteral("ms avg");
            data.status = levelStatus(average, 2000, 10000);
            return data;
        }
        if (id == QLatin1String("tool-errors")) {
            QList<QPair<QString, double>> tally;
            for (const AgentToolFamilyStats &family : families)
                if (family.errors > 0)
                    tally.append({family.family, double(family.errors)});
            StatsTileData data = number(
                double(totals.errors),
                totals.calls > 0
                    ? QStringLiteral("%1% of %2 calls")
                          .arg(100.0 * double(totals.errors) / double(totals.calls), 0, 'f', 1)
                          .arg(StatsDashboard::compactNumber(double(totals.calls)))
                    : QStringLiteral("no calls yet"));
            data.series = tallySeries(tally, 8);
            data.unit = QStringLiteral("errors");
            data.status = errorStatus;
            return data;
        }
        if (id == QLatin1String("tool-while-off")) {
            qint64 whileOff = 0;
            QList<QPair<QString, double>> tally;
            for (const AgentToolFamilyStats &family : families) {
                whileOff += family.callsWhileOff;
                if (family.callsWhileOff > 0)
                    tally.append({family.family, double(family.callsWhileOff)});
            }
            StatsTileData data = number(
                double(whileOff), QStringLiteral("%1 families ignored their switch").arg(tally.size()));
            data.status = whileOff > 0 ? StatsTileStatus::Warning : StatsTileStatus::Good;
            data.series = tallySeries(tally, 8);
            data.unit = QStringLiteral("calls");
            return data;
        }
        if (id == QLatin1String("tool-runtimes")) {
            QHash<QString, double> runtimes;
            for (const AgentToolFamilyStats &family : families)
                for (auto it = family.runtimeNames.cbegin(); it != family.runtimeNames.cend(); ++it)
                    runtimes[it.key()] += double(it.value());
            StatsTileData data = number(
                runtimes.size(), QStringLiteral("runtime tool names across %1 families")
                                     .arg(totals.familiesObserved));
            data.series = tallySeries(tallyOf(runtimes), 8);
            data.unit = QStringLiteral("calls");
            return data;
        }
        if (id == QLatin1String("tool-baseline")) {
            qint64 baseline = 0, actual = 0, measured = 0;
            for (const AgentToolFamilyStats &family : families) {
                baseline += family.baselineTokens;
                actual += family.consumedTokens();
                measured += family.measuredCalls;
            }
            StatsTileData data = number(
                double(baseline),
                QString::fromUtf8("vs %1 injected \xC2\xB7 %2 measured calls")
                    .arg(StatsDashboard::compactNumber(double(actual)),
                         StatsDashboard::compactNumber(double(measured))));
            data.series = orderedSeries({QStringLiteral("Baseline"), QStringLiteral("Injected")},
                                        {double(baseline), double(actual)});
            data.unit = QStringLiteral("tokens");
            return data;
        }
        // tool-coverage
        StatsTileData data;
        data.value = QStringLiteral("%1/%2").arg(totals.familiesObserved).arg(totals.familiesTotal);
        data.note = QStringLiteral("tool families observed");
        QList<QPair<QString, double>> tally;
        for (const AgentToolFamilyStats &family : families)
            tally.append({family.family, double(family.calls)});
        data.series = tallySeries(tally, 8);
        data.unit = QStringLiteral("calls");
        return data;
    }
    if (id.startsWith(QLatin1String("improvement"))) {
        if (!m_agentToolImprovements || m_agentToolImprovements->signalCount() == 0)
            return unavailable(QStringLiteral("No improvement signals logged yet"));
        const QVector<AgentToolImprovement> logged = m_agentToolImprovements->improvements();
        QHash<QString, double> kinds, families;
        double stake = 0;
        for (const AgentToolImprovement &signal : logged) {
            kinds[AgentToolImprovementLog::kindLabel(signal.kind)] += double(signal.occurrences);
            families[signal.family.isEmpty() ? QStringLiteral("other") : signal.family] +=
                double(signal.tokensAtStake);
            stake += double(signal.tokensAtStake);
        }
        if (id == QLatin1String("improvement-signals")) {
            StatsTileData data = number(
                m_agentToolImprovements->signalCount(),
                QString::fromUtf8("%1 occurrences \xC2\xB7 %2 tokens at stake")
                    .arg(StatsDashboard::compactNumber(double(m_agentToolImprovements->occurrenceCount())),
                         StatsDashboard::compactNumber(stake)));
            data.series = tallySeries(tallyOf(kinds), 8);
            data.unit = QStringLiteral("occurrences");
            return data;
        }
        StatsTileData data = number(
            stake, QStringLiteral("context tokens across %1")
                       .arg(pluralNote(logged.size(), "signal", "signals")));
        data.series = tallySeries(tallyOf(families), 8);
        data.unit = QStringLiteral("tokens");
        return data;
    }
    if (id.startsWith(QLatin1String("guardrail"))) {
        if (!m_agentGuardrailLedger || !m_agentGuardrailLedger->hasEvents())
            return unavailable(QStringLiteral("No guardrail events yet"));
        const AgentGuardrailTotals totals = m_agentGuardrailLedger->totals(now);
        if (id == QLatin1String("guardrail-blocked")) {
            StatsTileData data = number(
                double(totals.blocked),
                QString::fromUtf8("%1 outside allowed directories \xC2\xB7 %2 repos")
                    .arg(totals.blockedOutside).arg(totals.repositoriesWithEvents));
            data.series = bucketSeries(totals.recentBlocked, 60 * 60 * 1000);
            data.trend = StatsTrendPolarity::DownIsGood;
            data.unit = QStringLiteral("blocked");
            return data;
        }
        StatsTileData data = number(
            double(totals.approved + totals.denied),
            QString::fromUtf8("%1 approved \xC2\xB7 %2 denied \xC2\xB7 %3 stopped")
                .arg(totals.approved).arg(totals.denied).arg(totals.agentsStopped));
        data.series = orderedSeries(
            {QStringLiteral("Approved"), QStringLiteral("Denied"), QStringLiteral("Stopped")},
            {double(totals.approved), double(totals.denied), double(totals.agentsStopped)});
        data.unit = QStringLiteral("decisions");
        return data;
    }

    // ---- Actions -----------------------------------------------------------
    if (id == QLatin1String("workflows")) {
        if (!repo)
            return unavailable(noRepo);
        if (m_repoWorkflows.isEmpty())
            return unavailable(QStringLiteral("%1: no workflow files").arg(repoLabel));
        int valid = 0, steps = 0;
        QHash<QString, double> triggers;
        for (const ActionWorkflow &workflow : m_repoWorkflows) {
            if (workflow.valid)
                ++valid;
            steps += workflow.steps.size();
            for (const QString &trigger : workflow.on)
                triggers[trigger] += 1;
        }
        StatsTileData data = number(
            m_repoWorkflows.size(),
            QString::fromUtf8("%1 \xC2\xB7 %2 valid \xC2\xB7 %3")
                .arg(repoLabel).arg(valid).arg(pluralNote(steps, "step", "steps")));
        data.series = tallySeries(tallyOf(triggers), 6);
        data.unit = QStringLiteral("workflows");
        data.status = valid < m_repoWorkflows.size() ? StatsTileStatus::Warning
                                                     : StatsTileStatus::Good;
        return data;
    }
    if (id == QLatin1String("action-queue")) {
        int awaiting = 0, queued = 0, running = 0;
        for (const ActionRun &run : m_actionRuns) {
            if (run.status == ActionStatus::AwaitingApproval)
                ++awaiting;
            else if (run.status == ActionStatus::Queued)
                ++queued;
            else if (run.status == ActionStatus::Running)
                ++running;
        }
        StatsTileData data = number(
            awaiting + queued + running,
            QString::fromUtf8("%1 awaiting approval \xC2\xB7 %2 queued \xC2\xB7 %3 running \xC2\xB7 %4")
                .arg(awaiting).arg(queued).arg(running)
                .arg(pluralNote(m_actionRunners.size(), "runner", "runners")));
        data.series = sampledSeries(m_statsLiveSamples.value(id), m_statsSampleLabels);
        data.unit = QStringLiteral("pending");
        data.trend = StatsTrendPolarity::DownIsGood;
        if (awaiting > 0)
            data.status = StatsTileStatus::Warning;
        return data;
    }
    if (id.startsWith(QLatin1String("action"))) {
        if (m_actionRuns.isEmpty())
            return unavailable(QStringLiteral("No workflow runs yet"));
        if (id == QLatin1String("action-runs")) {
            QHash<QString, int> byStatus;
            int failed = 0;
            for (const ActionRun &run : m_actionRuns) {
                byStatus[run.status] += 1;
                if (run.status == ActionStatus::Failed)
                    ++failed;
            }
            QList<QPair<QString, double>> tally;
            for (auto it = byStatus.cbegin(); it != byStatus.cend(); ++it) {
                QString label = it.key();
                if (!label.isEmpty())
                    label[0] = label.at(0).toUpper();
                tally.append({label.replace(QLatin1Char('-'), QLatin1Char(' ')),
                              double(it.value())});
            }
            StatsTileData data = number(m_actionRuns.size(),
                                        QStringLiteral("%1 failed").arg(failed));
            data.series = tallySeries(tally, 8);
            data.unit = QStringLiteral("runs");
            return data;
        }
        if (id == QLatin1String("action-history") || id == QLatin1String("action-failed")) {
            const bool failedOnly = id == QLatin1String("action-failed");
            StatsTileData data;
            data.series = dailySeries(
                m_actionRuns, 14,
                [](const ActionRun &run) { return run.createdAtMs; },
                [failedOnly](const ActionRun &run) {
                    return failedOnly && run.status != ActionStatus::Failed ? 0.0 : 1.0;
                });
            double fortnight = 0;
            for (double count : data.series.values)
                fortnight += count;
            int failed = 0;
            for (const ActionRun &run : m_actionRuns)
                if (run.status == ActionStatus::Failed)
                    ++failed;
            data.value = StatsDashboard::compactNumber(fortnight);
            data.note = failedOnly
                            ? QString::fromUtf8("in 14 days \xC2\xB7 %1 failed of %2 on record")
                                  .arg(failed).arg(m_actionRuns.size())
                            : QString::fromUtf8("in 14 days \xC2\xB7 %1 on record")
                                  .arg(m_actionRuns.size());
            data.unit = QStringLiteral("runs");
            if (failedOnly) {
                data.trend = StatsTrendPolarity::DownIsGood;
                data.status = fortnight > 0 ? StatsTileStatus::Warning : StatsTileStatus::Good;
            }
            return data;
        }
        if (id == QLatin1String("action-pull-runs")) {
            int pullRuns = 0, held = 0;
            QHash<QString, double> authors;
            for (const ActionRun &run : m_actionRuns) {
                if (run.pullNumber <= 0)
                    continue;
                ++pullRuns;
                if (run.firstTimePullAuthor)
                    ++held;
                authors[run.pullAuthor.isEmpty() ? QStringLiteral("unknown") : run.pullAuthor] += 1;
            }
            StatsTileData data = number(
                pullRuns, QString::fromUtf8("%1 held for a first-time author \xC2\xB7 %2")
                              .arg(held).arg(pluralNote(authors.size(), "author", "authors")));
            data.series = tallySeries(tallyOf(authors), 8);
            if (held > 0)
                data.status = StatsTileStatus::Warning;
            data.unit = QStringLiteral("runs");
            return data;
        }
        if (id == QLatin1String("action-steps")) {
            QHash<QString, double> outcomes;
            int steps = 0;
            for (const ActionRun &run : m_actionRuns)
                for (const QString &status : run.stepStatuses) {
                    ++steps;
                    QString label = status.isEmpty() ? QStringLiteral("pending") : status;
                    label[0] = label.at(0).toUpper();
                    outcomes[label] += 1;
                }
            StatsTileData data = number(
                steps, QStringLiteral("%1 per run over %2")
                           .arg(steps / qMax(1, int(m_actionRuns.size())))
                           .arg(pluralNote(m_actionRuns.size(), "run", "runs")));
            data.series = tallySeries(tallyOf(outcomes), 7);
            data.unit = QStringLiteral("steps");
            return data;
        }
        // Timing: every finished run, newest first.
        struct TimedRun {
            int id = 0;
            QString workflow;
            QString status;
            qint64 durationMs = -1;
            qint64 waitMs = -1;
            qint64 finishedAtMs = 0;
        };
        QVector<TimedRun> finishedRuns;
        int succeeded = 0;
        for (const ActionRun &run : m_actionRuns) {
            if (run.status != ActionStatus::Success && run.status != ActionStatus::Failed)
                continue;
            if (run.status == ActionStatus::Success)
                ++succeeded;
            TimedRun timed;
            timed.id = run.id;
            timed.workflow = run.workflowName.isEmpty() ? run.workflowPath : run.workflowName;
            timed.status = run.status;
            timed.finishedAtMs = run.finishedAtMs;
            if (run.startedAtMs > 0 && run.finishedAtMs >= run.startedAtMs)
                timed.durationMs = run.finishedAtMs - run.startedAtMs;
            if (run.createdAtMs > 0 && run.startedAtMs >= run.createdAtMs)
                timed.waitMs = run.startedAtMs - run.createdAtMs;
            finishedRuns.append(timed);
        }
        const int finished = finishedRuns.size();
        if (finished == 0)
            return unavailable(QStringLiteral("No finished runs yet"));
        if (id == QLatin1String("action-success")) {
            StatsTileData data;
            data.value = QStringLiteral("%1%").arg(qRound(100.0 * succeeded / finished));
            data.status = floorStatus(100.0 * succeeded / finished, 90, 70);
            data.note = QStringLiteral("%1 of %2 finished runs").arg(succeeded).arg(finished);
            data.series = orderedSeries(
                {QStringLiteral("Succeeded"), QStringLiteral("Failed")},
                {double(succeeded), double(finished - succeeded)});
            data.unit = QStringLiteral("runs");
            return data;
        }
        if (id == QLatin1String("action-duration") || id == QLatin1String("action-wait")) {
            const bool wait = id == QLatin1String("action-wait");
            qint64 total = 0;
            int timed = 0;
            QVector<double> recent;
            QStringList recentLabels;
            for (const TimedRun &run : finishedRuns) {
                const qint64 ms = wait ? run.waitMs : run.durationMs;
                if (ms < 0)
                    continue;
                total += ms;
                ++timed;
                if (recent.size() < 20) {
                    recent.prepend(ms / 1000.0);
                    recentLabels.prepend(QStringLiteral("#%1").arg(run.id));
                }
            }
            if (timed == 0)
                return unavailable(QStringLiteral("No run carries %1 timestamps yet")
                                       .arg(wait ? QStringLiteral("queue") : QStringLiteral("start")));
            StatsTileData data;
            data.value = StatsDashboard::compactDuration(total / timed);
            data.note = QStringLiteral("%1 over %2")
                            .arg(wait ? QStringLiteral("average wait to start")
                                      : QStringLiteral("average run time"),
                                 pluralNote(timed, "finished run", "finished runs"));
            data.series.labels = recentLabels;
            data.series.values = recent;
            data.unit = QStringLiteral("s");
            data.trend = StatsTrendPolarity::DownIsGood;
            return data;
        }
        if (id == QLatin1String("action-workflow-times")) {
            QHash<QString, QPair<qint64, int>> perWorkflow; // total ms, runs
            for (const TimedRun &run : finishedRuns) {
                if (run.durationMs < 0)
                    continue;
                QPair<qint64, int> &entry = perWorkflow[run.workflow];
                entry.first += run.durationMs;
                entry.second += 1;
            }
            if (perWorkflow.isEmpty())
                return unavailable(QStringLiteral("No timed runs yet"));
            QList<QPair<QString, double>> tally;
            QString slowest;
            double slowestSeconds = -1;
            for (auto it = perWorkflow.cbegin(); it != perWorkflow.cend(); ++it) {
                const double seconds = double(it.value().first) / it.value().second / 1000.0;
                tally.append({it.key(), seconds});
                if (seconds > slowestSeconds) {
                    slowestSeconds = seconds;
                    slowest = it.key();
                }
            }
            StatsTileData data = number(
                perWorkflow.size(),
                QString::fromUtf8("workflows timed \xC2\xB7 slowest %1 at %2")
                    .arg(slowest, StatsDashboard::compactDuration(qint64(slowestSeconds * 1000))));
            data.series = tallySeries(tally, 8);
            data.unit = QStringLiteral("s avg");
            return data;
        }
        // action-slowest / action-last
        const TimedRun *pick = nullptr;
        for (const TimedRun &run : finishedRuns) {
            if (run.durationMs < 0)
                continue;
            if (id == QLatin1String("action-last")) {
                pick = &run; // newest first
                break;
            }
            if (!pick || run.durationMs > pick->durationMs)
                pick = &run;
        }
        if (!pick)
            return unavailable(QStringLiteral("No timed runs yet"));
        StatsTileData data;
        data.value = StatsDashboard::compactDuration(pick->durationMs);
        data.note = QString::fromUtf8("#%1 \xC2\xB7 %2 \xC2\xB7 %3 \xC2\xB7 %4")
                        .arg(pick->id)
                        .arg(pick->workflow, pick->status,
                             dayLabel(pick->finishedAtMs > 0 ? pick->finishedAtMs : now));
        QVector<double> recent;
        QStringList recentLabels;
        for (const TimedRun &run : finishedRuns) {
            if (run.durationMs < 0 || recent.size() >= 20)
                continue;
            recent.prepend(run.durationMs / 1000.0);
            recentLabels.prepend(QStringLiteral("#%1").arg(run.id));
        }
        data.series.labels = recentLabels;
        data.series.values = recent;
        data.unit = QStringLiteral("s");
        data.trend = StatsTrendPolarity::DownIsGood;
        if (id == QLatin1String("action-last"))
            data.status = pick->status == ActionStatus::Success ? StatsTileStatus::Good
                                                                : StatsTileStatus::Critical;
        return data;
    }

    // ---- Network & logs ----------------------------------------------------
    if (id == QLatin1String("nodes-online")) {
        int online = 0;
        QHash<QString, int> byPlatform;
        for (const MemberInfo &member : m_homeRoster) {
            if (!member.online)
                continue;
            ++online;
            byPlatform[member.platform.isEmpty() ? QStringLiteral("unknown")
                                                 : member.platform] += 1;
        }
        QList<QPair<QString, double>> tally;
        for (auto it = byPlatform.cbegin(); it != byPlatform.cend(); ++it)
            tally.append({it.key(), double(it.value())});
        StatsTileData data = number(
            online, pluralNote(m_homeRoster.size(), "member known", "members known"));
        data.series = tallySeries(tally, 6);
        data.unit = QStringLiteral("nodes");
        if (online > 0)
            data.status = StatsTileStatus::Good;
        else if (!m_homeRoster.isEmpty())
            data.status = StatsTileStatus::Warning;
        return data;
    }
    if (id == QLatin1String("linked-nodes"))
        return number(m_profileLinkedNodes.size(),
                      QStringLiteral("linked to your account"));
    if (id == QLatin1String("network-catalog")) {
        if (m_networkRelayCount + m_networkNodeCount + m_networkHostCount <= 0)
            return unavailable(QStringLiteral("Open Network to load the catalog"));
        StatsTileData data = number(
            m_networkNodeCount, QString::fromUtf8("nodes \xC2\xB7 %1 relays \xC2\xB7 %2 hosts")
                                    .arg(m_networkRelayCount).arg(m_networkHostCount));
        data.series = orderedSeries(
            {QStringLiteral("Relays"), QStringLiteral("Nodes"), QStringLiteral("Hosts")},
            {double(m_networkRelayCount), double(m_networkNodeCount),
             double(m_networkHostCount)});
        data.unit = QStringLiteral("entries");
        return data;
    }
    if (id == QLatin1String("servers")) {
        int measured = 0;
        QList<QPair<QString, double>> tally;
        for (const ServerConfig &server : m_servers) {
            const RelayLatencySample sample = m_relayHostLatency.value(hostOf(server.url));
            if (sample.ms > 0) {
                ++measured;
                tally.append({server.name.isEmpty() ? hostOf(server.url) : server.name,
                              double(sample.ms)});
            }
        }
        const QString active = m_activeServer >= 0 && m_activeServer < m_servers.size()
                                   ? m_servers.at(m_activeServer).name
                                   : QString();
        StatsTileData data = number(
            m_servers.size(),
            active.isEmpty() ? QStringLiteral("%1 measured").arg(measured)
                             : QString::fromUtf8("active %1 \xC2\xB7 %2 measured").arg(active).arg(measured));
        data.series = tallySeries(tally, 8);
        data.unit = QStringLiteral("ms");
        data.series.tones.clear();
        for (double ms : data.series.values)
            data.series.tones.append(levelStatus(ms, 150, 500));
        if (active.isEmpty())
            data.status = StatsTileStatus::Warning;
        return data;
    }
    if (id == QLatin1String("services")) {
        QHash<QString, double> byStatus;
        int operational = 0, degraded = 0, down = 0, total = 0;
        for (const QList<FooterStatusRow> *rows : {&m_footerRelayStatuses, &m_footerDesktopStatuses})
            for (const FooterStatusRow &row : *rows) {
                ++total;
                if (row.status == QLatin1String("operational"))
                    ++operational;
                else if (row.status == QLatin1String("degraded"))
                    ++degraded;
                else if (row.status == QLatin1String("down"))
                    ++down;
                QString label = row.status.isEmpty() ? QStringLiteral("unknown") : row.status;
                label[0] = label.at(0).toUpper();
                byStatus[label] += 1;
            }
        if (total == 0)
            return unavailable(QStringLiteral("No status sample yet"));
        StatsTileData data;
        data.value = QStringLiteral("%1/%2").arg(operational).arg(total);
        data.note = QString::fromUtf8("operational \xC2\xB7 %1 relay-reported \xC2\xB7 %2 checked here")
                        .arg(m_footerRelayStatuses.size()).arg(m_footerDesktopStatuses.size());
        data.series = tallySeries(tallyOf(byStatus), 4);
        data.unit = QStringLiteral("checks");
        // Checks with no verdict yet ("unknown") neither pass nor fail.
        if (down > 0)
            data.status = StatsTileStatus::Critical;
        else if (degraded > 0)
            data.status = StatsTileStatus::Warning;
        else if (operational == total)
            data.status = StatsTileStatus::Good;
        return data;
    }
    if (id == QLatin1String("web-alerts")) {
        if (m_webAlertsFetchedAtMs <= 0)
            return unavailable(QStringLiteral("Web alerts not fetched yet"));
        StatsTileData data =
            number(m_webAlertsTotal, QStringLiteral("%1 unread").arg(m_webAlertsUnread));
        data.status = m_webAlertsUnread > 0 ? StatsTileStatus::Warning : StatsTileStatus::Good;
        return data;
    }
    if (id == QLatin1String("firewall")) {
        if (m_requestFirewallHistory.isEmpty())
            return unavailable(QStringLiteral("No request has been checked yet"));
        int allowed = 0, blocked = 0;
        QHash<QString, double> rules;
        for (const FirewallHistoryEntry &entry : m_requestFirewallHistory) {
            if (entry.allowed)
                ++allowed;
            else
                ++blocked;
            if (!entry.rule.isEmpty())
                rules[entry.rule] += 1;
        }
        StatsTileData data = number(
            blocked, QString::fromUtf8("blocked \xC2\xB7 %1 allowed \xC2\xB7 %2")
                         .arg(allowed).arg(pluralNote(rules.size(), "rule matched", "rules matched")));
        data.series = orderedSeries({QStringLiteral("Allowed"), QStringLiteral("Blocked")},
                                    {double(allowed), double(blocked)});
        data.unit = QStringLiteral("requests");
        return data;
    }
    if (id == QLatin1String("uptime")) {
        if (m_connectedAtMs <= 0) {
            StatsTileData data = unavailable(QStringLiteral("Not connected"));
            data.status = StatsTileStatus::Critical;
            return data;
        }
        const qint64 session = now - m_connectedAtMs;
        StatsTileData data;
        data.status = StatsTileStatus::Good;
        data.value = StatsDashboard::compactDuration(session);
        data.note = QString::fromUtf8("this session \xC2\xB7 %1 lifetime")
                        .arg(StatsDashboard::compactDuration(m_totalConnectionMs + session));
        return data;
    }
    if (id == QLatin1String("log-lines")) {
        if (!m_logActivityLights)
            return unavailable(QStringLiteral("Log activity is not tracked yet"));
        double total = 0;
        QList<QPair<QString, double>> tally;
        for (const QString &badge : forkmesh::ui::LogActivityLights::badges()) {
            const double count = double(m_logActivityLights->countFor(badge));
            total += count;
            tally.append({badge, count});
        }
        StatsTileData data = number(total, QStringLiteral("this session"));
        data.series = tallySeries(tally, 8);
        data.unit = QStringLiteral("lines");
        return data;
    }
    if (id == QLatin1String("log-history")) {
        if (m_networkLogDigest.isEmpty())
            return unavailable(QStringLiteral("No archived log digest yet"));
        StatsTileData data;
        const qint64 start = now - 29 * kDayMs;
        data.series.values = QVector<double>(30, 0.0);
        for (int d = 0; d < 30; ++d)
            data.series.labels.append(dayLabel(start + d * kDayMs));
        double total = 0;
        double today = 0;
        for (auto it = m_networkLogDigest.cbegin(); it != m_networkLogDigest.cend(); ++it) {
            double lines = 0;
            for (int count : it.value())
                lines += count;
            total += lines;
            const int bucket = int((it.key() - start + kDayMs) / kDayMs) - 1;
            if (bucket >= 0 && bucket < 30)
                data.series.values[bucket] += lines;
            if (bucket == 29)
                today += lines;
        }
        data.value = StatsDashboard::compactNumber(total);
        data.note = QString::fromUtf8("%1 today \xC2\xB7 30-day archive")
                        .arg(StatsDashboard::compactNumber(today));
        data.unit = QStringLiteral("lines");
        return data;
    }
    if (id == QLatin1String("stalls")) {
        StatsTileData data = number(m_stallCount, QStringLiteral("GUI stalls this session"));
        data.status = levelStatus(m_stallCount, 1, 10);
        return data;
    }

    // ---- This machine ------------------------------------------------------
    if (id == QLatin1String("cpu")) {
        StatsTileData data;
        const double last = m_statsCpuSamples.isEmpty() ? 0.0 : m_statsCpuSamples.last();
        data.value = percentText(last);
        data.note = QString::fromUtf8("this app \xC2\xB7 host %1%")
                        .arg(qRound(qMax(0.0, SystemStats::hostCpuPercent())));
        data.series.labels = m_statsSampleLabels;
        data.series.values = m_statsCpuSamples;
        data.unit = QStringLiteral("%");
        data.trend = StatsTrendPolarity::DownIsGood;
        data.status = levelStatus(last, 50, 85);
        return data;
    }
    if (id == QLatin1String("memory")) {
        StatsTileData data;
        data.value = StatsDashboard::compactBytes(SystemStats::residentBytes());
        data.note = QString::fromUtf8("resident \xC2\xB7 %1 threads").arg(SystemStats::threadCount());
        data.series.labels = m_statsSampleLabels;
        data.series.values = m_statsRssSamples;
        data.unit = QStringLiteral("MB");
        data.trend = StatsTrendPolarity::DownIsGood;
        return data;
    }
    if (id == QLatin1String("host-memory")) {
        const qint64 total = SystemStats::totalMemoryBytes();
        const qint64 available = SystemStats::availableMemoryBytes();
        if (total <= 0)
            return unavailable(QStringLiteral("Host memory is not readable here"));
        StatsTileData data;
        data.value = QStringLiteral("%1%").arg(qRound(100.0 * double(total - available) / double(total)));
        data.note = QStringLiteral("%1 of %2 in use")
                        .arg(StatsDashboard::compactBytes(total - available),
                             StatsDashboard::compactBytes(total));
        data.series.labels = m_statsSampleLabels;
        data.series.values = m_statsHostMemorySamples;
        data.unit = QStringLiteral("%");
        data.trend = StatsTrendPolarity::DownIsGood;
        data.status = levelStatus(100.0 * double(total - available) / double(total), 75, 90);
        return data;
    }
    if (id == QLatin1String("swap")) {
        const qint64 total = SystemStats::totalSwapBytes();
        const qint64 free = SystemStats::freeSwapBytes();
        if (total <= 0)
            return unavailable(QStringLiteral("No swap configured"));
        StatsTileData data;
        data.value = StatsDashboard::compactBytes(total - free);
        data.note = QString::fromUtf8("of %1 \xC2\xB7 %2% used")
                        .arg(StatsDashboard::compactBytes(total))
                        .arg(qRound(100.0 * double(total - free) / double(total)));
        data.series = orderedSeries({QStringLiteral("Used"), QStringLiteral("Free")},
                                    {double(total - free) / (1024.0 * 1024 * 1024),
                                     double(free) / (1024.0 * 1024 * 1024)});
        data.unit = QStringLiteral("GB");
        data.status = levelStatus(100.0 * double(total - free) / double(total), 25, 60);
        data.series.tones = {data.status, StatsTileStatus::None};
        return data;
    }
    if (id == QLatin1String("disk")) {
        QString path = repoGitDir();
        if (path.isEmpty())
            path = QDir::homePath();
        const qint64 total = SystemStats::diskTotalBytes(path);
        const qint64 free = SystemStats::diskFreeBytes(path);
        if (total <= 0)
            return unavailable(QStringLiteral("Disk usage is not readable here"));
        StatsTileData data;
        data.value = StatsDashboard::compactBytes(free);
        data.note = QString::fromUtf8("free of %1 \xC2\xB7 %2% used")
                        .arg(StatsDashboard::compactBytes(total))
                        .arg(qRound(100.0 * double(total - free) / double(total)));
        data.series = orderedSeries({QStringLiteral("Used"), QStringLiteral("Free")},
                                    {double(total - free) / (1024.0 * 1024 * 1024),
                                     double(free) / (1024.0 * 1024 * 1024)});
        data.unit = QStringLiteral("GB");
        data.status = levelStatus(100.0 * double(total - free) / double(total), 80, 92);
        data.series.tones = {data.status, StatsTileStatus::None};
        return data;
    }
    if (id == QLatin1String("open-files")) {
        const int open = SystemStats::openFileCount();
        const int limit = SystemStats::openFileSoftLimit();
        if (open < 0)
            return unavailable(QStringLiteral("Open files are not readable here"));
        StatsTileData data = number(open, limit > 0
                                              ? QStringLiteral("of %1 allowed").arg(limit)
                                              : QStringLiteral("descriptors"));
        if (limit > 0)
            data.status = levelStatus(100.0 * open / limit, 50, 80);
        return data;
    }
    if (id == QLatin1String("threads"))
        return number(SystemStats::threadCount(), QStringLiteral("in this process"));
    if (id == QLatin1String("app-uptime")) {
        if (!m_debugSessionUptime.isValid())
            return unavailable(QStringLiteral("Launch time not recorded"));
        StatsTileData data;
        data.value = StatsDashboard::compactDuration(m_debugSessionUptime.elapsed());
        data.note = QStringLiteral("since launch");
        return data;
    }
    if (id == QLatin1String("restarts") || id == QLatin1String("restart-time")) {
        using forkmesh::restarttiming::RestartRun;
        const QList<RestartRun> rebuilds = restartRunHistory(RestartKind::Rebuild);
        const QList<RestartRun> updates = restartRunHistory(RestartKind::Update);
        if (id == QLatin1String("restarts"))
            return number(rebuilds.size() + updates.size(),
                          QString::fromUtf8("%1 rebuild \xC2\xB7 %2 update")
                              .arg(rebuilds.size())
                              .arg(updates.size()));
        struct TimedRestart {
            RestartRun run;
            bool update = false;
        };
        QVector<TimedRestart> timed;
        for (const RestartRun &run : rebuilds)
            timed.append({run, false});
        for (const RestartRun &run : updates)
            timed.append({run, true});
        if (timed.isEmpty())
            return unavailable(QStringLiteral("No restart timed yet"));
        // Totals from older builds carry no timestamp and stay first, in order.
        std::stable_sort(timed.begin(), timed.end(),
                         [](const TimedRestart &a, const TimedRestart &b) {
                             return a.run.finishedAtMs < b.run.finishedAtMs;
                         });
        const TimedRestart &last = timed.constLast();
        QList<qint64> usual;
        for (const RestartRun &run : last.update ? updates : rebuilds)
            usual.append(run.totalMs);
        StatsTileData data;
        data.value = StatsDashboard::compactDuration(last.run.totalMs);
        data.note = QString::fromUtf8("last %1 \xC2\xB7 usually %2")
                        .arg(last.update ? QStringLiteral("update")
                                         : QStringLiteral("rebuild"),
                             StatsDashboard::compactDuration(
                                 forkmesh::restarttiming::estimatedDurationMs(usual)));
        const int first = qMax(0, int(timed.size()) - 20);
        for (int index = first; index < timed.size(); ++index) {
            const RestartRun &run = timed.at(index).run;
            data.series.values.append(run.totalMs / 1000.0);
            data.series.labels.append(
                run.finishedAtMs > 0
                    ? QDateTime::fromMSecsSinceEpoch(run.finishedAtMs)
                          .toString(QStringLiteral("MMM d HH:mm"))
                    : QStringLiteral("#%1").arg(index + 1));
        }
        data.unit = QStringLiteral("s");
        data.trend = StatsTrendPolarity::DownIsGood;
        return data;
    }
    if (id == QLatin1String("diff-cache"))
        return number(m_branchDiffCache.size(),
                      QString::fromUtf8("cached branch diffs \xC2\xB7 %1")
                          .arg(StatsDashboard::compactBytes(m_branchDiffCacheBytes)));
    if (id == QLatin1String("loop-lag") || id == QLatin1String("events-per-second") ||
        id == QLatin1String("widgets") || id == QLatin1String("windows") ||
        id == QLatin1String("timers") || id == QLatin1String("thread-pool")) {
        if (!m_qtDiagnostics)
            return unavailable(QStringLiteral("Qt runtime diagnostics are off"));
        const QVector<forkmesh::ui::QtRuntimeDiagnostics::Snapshot> history =
            m_qtDiagnostics->history();
        const forkmesh::ui::QtRuntimeDiagnostics::Snapshot snapshot =
            m_qtDiagnostics->snapshot();
        StatsTileData data;
        for (const auto &sample : history) {
            data.series.labels.append(
                QDateTime::fromMSecsSinceEpoch(sample.sampledAtMs)
                    .toString(QStringLiteral("HH:mm:ss")));
            if (id == QLatin1String("loop-lag"))
                data.series.values.append(double(sample.p95LoopLagMs));
            else if (id == QLatin1String("events-per-second"))
                data.series.values.append(double(sample.eventsPerSecond));
            else if (id == QLatin1String("windows"))
                data.series.values.append(double(sample.windowCount));
            else if (id == QLatin1String("timers"))
                data.series.values.append(double(sample.activeTimerCount));
            else if (id == QLatin1String("thread-pool"))
                data.series.values.append(double(sample.threadPoolActive));
            else
                data.series.values.append(double(sample.widgetCount));
        }
        if (id == QLatin1String("loop-lag")) {
            data.value = QStringLiteral("%1 ms").arg(snapshot.p95LoopLagMs);
            data.note = QString::fromUtf8("p95 \xC2\xB7 peak %1 ms").arg(snapshot.peakLoopLagMs);
            data.unit = QStringLiteral("ms");
            data.trend = StatsTrendPolarity::DownIsGood;
            data.status = levelStatus(snapshot.p95LoopLagMs, 50, 200);
        } else if (id == QLatin1String("events-per-second")) {
            data.value = StatsDashboard::compactNumber(snapshot.eventsPerSecond);
            data.note = QString::fromUtf8("%1 paints \xC2\xB7 %2 layouts per second")
                            .arg(snapshot.paintsPerSecond).arg(snapshot.layoutsPerSecond);
            data.unit = QStringLiteral("events/s");
        } else if (id == QLatin1String("windows")) {
            data.value = StatsDashboard::compactNumber(snapshot.windowCount);
            data.note = QStringLiteral("%1 visible").arg(snapshot.visibleWindowCount);
            data.unit = QStringLiteral("windows");
        } else if (id == QLatin1String("timers")) {
            data.value = StatsDashboard::compactNumber(snapshot.activeTimerCount);
            data.note = QStringLiteral("active of %1 registered").arg(snapshot.timerCount);
            data.unit = QStringLiteral("timers");
        } else if (id == QLatin1String("thread-pool")) {
            data.value = QStringLiteral("%1/%2").arg(snapshot.threadPoolActive).arg(snapshot.threadPoolMax);
            data.note = QStringLiteral("global pool workers busy");
            data.unit = QStringLiteral("busy");
            if (snapshot.threadPoolMax > 0)
                data.status = levelStatus(100.0 * snapshot.threadPoolActive /
                                              snapshot.threadPoolMax,
                                          75, 100);
        } else {
            data.value = StatsDashboard::compactNumber(snapshot.widgetCount);
            data.note = QString::fromUtf8("%1 visible \xC2\xB7 %2 objects")
                            .arg(snapshot.visibleWidgetCount)
                            .arg(StatsDashboard::compactNumber(snapshot.objectCount));
            data.unit = QStringLiteral("widgets");
        }
        return data;
    }

    // ---- Market & rewards --------------------------------------------------
    if (id == QLatin1String("packages")) {
        if (m_packageInstallCounts.isEmpty())
            return unavailable(QStringLiteral("Install counts not fetched yet"));
        double installs = 0, downloads = 0;
        QList<QPair<QString, double>> tally;
        for (auto it = m_packageInstallCounts.cbegin(); it != m_packageInstallCounts.cend(); ++it) {
            downloads += double(it.value().first);
            installs += double(it.value().second);
            tally.append({it.key(), double(it.value().second)});
        }
        StatsTileData data = number(
            installs, QString::fromUtf8("%1 downloads \xC2\xB7 %2 packages")
                          .arg(StatsDashboard::compactNumber(downloads))
                          .arg(m_packageInstallCounts.size()));
        data.series = tallySeries(tally, 8);
        data.unit = QStringLiteral("installs");
        return data;
    }
    if (id == QLatin1String("release-downloads")) {
        if (m_releaseDownloadsCache.isEmpty())
            return unavailable(QStringLiteral("Open Releases to fetch download counts"));
        double total = 0;
        for (int count : m_releaseDownloadsCache)
            total += count;
        return number(total, QString::fromUtf8("%1 \xC2\xB7 %2")
                                 .arg(m_releaseDownloadsSource,
                                      pluralNote(m_releaseDownloadsCache.size(), "asset", "assets")));
    }
    if (id == QLatin1String("rewards")) {
        if (m_contributorRewards.isEmpty())
            return unavailable(QStringLiteral("Open the reward pool to fetch rewards"));
        QHash<QString, double> byStatus;
        double lamports = 0;
        for (const QJsonObject &reward : m_contributorRewards) {
            QString status = reward.value(QStringLiteral("status")).toString();
            if (status.isEmpty())
                status = QStringLiteral("pending");
            byStatus[status] += 1;
            lamports += reward.value(QStringLiteral("amountLamports")).toDouble();
        }
        StatsTileData data = number(
            m_contributorRewards.size(),
            QString::fromUtf8("rewards \xC2\xB7 %1 \xC2\xB7 %2 paid")
                .arg(solText(lamports))
                .arg(qRound(byStatus.value(QStringLiteral("paid"))))); 
        data.series = tallySeries(tallyOf(byStatus), 6);
        data.unit = QStringLiteral("rewards");
        return data;
    }
    if (id == QLatin1String("ledger")) {
        if (!m_ledgerPayableLoaded && !m_ledgerLocalRecordsLoaded)
            return unavailable(QStringLiteral("Open Ledger to load its records"));
        StatsTileData data = number(
            m_ledgerPayable.size() + m_ledgerLocalRecords.size(),
            QString::fromUtf8("%1 payable \xC2\xB7 %2 kept on this machine")
                .arg(m_ledgerPayable.size()).arg(m_ledgerLocalRecords.size()));
        data.series = orderedSeries({QStringLiteral("Payable"), QStringLiteral("Local")},
                                    {double(m_ledgerPayable.size()),
                                     double(m_ledgerLocalRecords.size())});
        data.unit = QStringLiteral("records");
        return data;
    }
    if (id == QLatin1String("wallets")) {
        double lamports = 0;
        int read = 0;
        QList<QPair<QString, double>> tally;
        for (auto it = m_ledgerWalletBalances.cbegin(); it != m_ledgerWalletBalances.cend(); ++it) {
            if (it.value().first < 0)
                continue;
            ++read;
            lamports += double(it.value().first);
            tally.append({it.key().left(6), double(it.value().first) / kLamportsPerSol});
        }
        if (read == 0)
            return unavailable(QStringLiteral("No wallet balance read yet"));
        StatsTileData data;
        data.value = solText(lamports);
        data.note = QStringLiteral("across %1").arg(pluralNote(read, "wallet", "wallets"));
        data.series = tallySeries(tally, 8);
        data.unit = QStringLiteral("SOL");
        return data;
    }
    if (id == QLatin1String("payouts"))
        return number(ContributorPayoutReceipt::Settled::rewardIdSet().size(),
                      QStringLiteral("paid from this machine"));

    // ---- Users (admin) -----------------------------------------------------
    if (id.startsWith(QLatin1String("users"))) {
        if (!m_isAdmin)
            return unavailable(QStringLiteral("Administrator only"));
        if (m_usersDirectoryPayload.isEmpty())
            return unavailable(QStringLiteral("Open Users to load the directory"));
        if (id == QLatin1String("users")) {
            QHash<QString, int> byBucket;
            for (const QJsonValue &value : m_usersDirectoryPayload) {
                QString bucket = value.toObject().value(QStringLiteral("activityBucket")).toString();
                if (bucket.isEmpty())
                    bucket = QStringLiteral("unknown");
                byBucket[bucket] += 1;
            }
            QList<QPair<QString, double>> tally;
            for (auto it = byBucket.cbegin(); it != byBucket.cend(); ++it)
                tally.append({it.key(), double(it.value())});
            StatsTileData data = number(m_usersDirectoryPayload.size(),
                                        QStringLiteral("accounts in the directory"));
            data.series = tallySeries(tally, 6);
            data.unit = QStringLiteral("accounts");
            return data;
        }
        double commits = 0, pulls = 0, issues = 0, discussions = 0, steps = 0, visits = 0;
        for (const QJsonValue &value : m_usersDirectoryPayload) {
            const QJsonObject user = value.toObject();
            commits += user.value(QStringLiteral("commits")).toDouble();
            pulls += user.value(QStringLiteral("pulls")).toDouble();
            issues += user.value(QStringLiteral("issues")).toDouble();
            discussions += user.value(QStringLiteral("discussions")).toDouble();
            steps += user.value(QStringLiteral("totalSteps")).toDouble();
            visits += user.value(QStringLiteral("officeVisits")).toDouble();
        }
        if (id == QLatin1String("users-activity")) {
            StatsTileData data = number(
                commits + pulls + issues + discussions,
                QString::fromUtf8("%1 commits \xC2\xB7 %2 pulls")
                    .arg(StatsDashboard::compactNumber(commits),
                         StatsDashboard::compactNumber(pulls)));
            data.series = orderedSeries(
                {QStringLiteral("Commits"), QStringLiteral("Pulls"),
                 QStringLiteral("Issues"), QStringLiteral("Discussions")},
                {commits, pulls, issues, discussions});
            data.unit = QStringLiteral("events");
            return data;
        }
        return number(steps, QStringLiteral("%1 office visits")
                                 .arg(StatsDashboard::compactNumber(visits)));
    }

    return unavailable(QStringLiteral("Unknown statistic"));
}
