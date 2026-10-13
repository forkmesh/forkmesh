#include "HomeBoard.h"
#include "HomeLayout.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "RailGroups.h"
#include "SystemStats.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeySequence>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

using namespace forkmesh::ui;

namespace {

constexpr int kHomeTickMs = 1000;
constexpr int kHomeSearchRevealMs = 160;
const QString kHomePagesSetting = QStringLiteral("ui/homePages");

QString plural(int count, const QString &noun)
{
    return QStringLiteral("%1 %2%3")
        .arg(count)
        .arg(noun, count == 1 ? QString() : QStringLiteral("s"));
}

QString tooltipSummary(const QString &tooltip)
{
    const QString line = tooltip.section(QLatin1Char('\n'), 0, 0);
    const QString dash = QString::fromUtf8(" \xE2\x80\x94 ");
    int at = line.indexOf(dash);
    int skip = dash.size();
    if (at < 0) {
        at = line.indexOf(QLatin1String(" - "));
        skip = 3;
    }
    if (at < 0)
        return QString();
    QString rest = line.mid(at + skip).trimmed();
    int cut = rest.size();
    for (const QChar stop : {QLatin1Char(','), QLatin1Char(';'),
                             QLatin1Char(':'), QLatin1Char('.')}) {
        const int found = rest.indexOf(stop);
        if (found > 0)
            cut = qMin(cut, found);
    }
    rest.truncate(cut);
    if (!rest.isEmpty())
        rest[0] = rest.at(0).toUpper();
    return rest;
}

}

QWidget *MainWindow::buildHomeGridSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("homeSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 8);
    outer->setSpacing(6);

    auto *heading = new QHBoxLayout;
    m_homeGridGreeting = new QLabel(QStringLiteral("Home"));
    m_homeGridGreeting->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(m_homeGridGreeting);
    heading->addStretch();
    outer->addLayout(heading);

    m_homeGridHeadline = new QLabel;
    m_homeGridHeadline->setObjectName(QStringLiteral("mutedLabel"));
    m_homeGridHeadline->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(m_homeGridHeadline);

    m_homeGridSearch = new QWidget;
    m_homeGridSearch->setObjectName(QStringLiteral("homeSearchRow"));
    auto *searchRow = new QHBoxLayout(m_homeGridSearch);
    searchRow->setContentsMargins(0, 4, 0, 4);
    m_homeGridFilter = new QLineEdit;
    m_homeGridFilter->setObjectName(QStringLiteral("homeFilter"));
    m_homeGridFilter->setPlaceholderText(QStringLiteral("Search pages…"));
    m_homeGridFilter->setClearButtonEnabled(true);
    m_homeGridFilter->setMinimumWidth(260);
    m_homeGridFilter->setMaximumWidth(520);
    m_homeGridFilter->setToolTip(QStringLiteral(
        "Show only the pages whose name, group or status contains this text; "
        "Enter opens the first one, Esc closes the search"));
    searchRow->addStretch();
    searchRow->addWidget(m_homeGridFilter, 1);
    searchRow->addStretch();
    m_homeGridSearch->hide();
    connect(m_homeGridFilter, &QLineEdit::textChanged, this,
            [this](const QString &text) {
                if (m_homeGridBoard)
                    m_homeGridBoard->setFilter(text);
            });
    connect(m_homeGridFilter, &QLineEdit::returnPressed, this, [this] {
        if (m_homeGridBoard)
            activateHomeGridTile(m_homeGridBoard->firstVisibleKey());
    });
    connect(m_homeGridFilter, &QLineEdit::editingFinished, this, [this] {
        QTimer::singleShot(0, this, [this] {
            if (m_homeGridFilter && !m_homeGridFilter->hasFocus() &&
                m_homeGridFilter->text().trimmed().isEmpty())
                hideHomeGridSearch();
        });
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_homeGridFilter);
    escape->setContext(Qt::WidgetShortcut);
    connect(escape, &QShortcut::activated, this, [this] { hideHomeGridSearch(); });
    outer->addWidget(m_homeGridSearch);

    m_homeGridBoard = new HomeBoard;
    m_homeGridBoard->setIconProvider(
        [](const QString &name, const QColor &color, int size) {
            return tintedOcticonPixmap(name.isEmpty() ? QStringLiteral("link")
                                                      : name,
                                       color, size);
        });
    m_homeGridBoard->setOnActivate(
        [this](const QString &key) { activateHomeGridTile(key); });
    m_homeGridBoard->setSavedPages(HomeLayout::fromJson(
        QSettings().value(kHomePagesSetting).toString()));
    m_homeGridBoard->setOnLayoutChanged([](const HomeLayout::Pages &pages) {
        QSettings settings;
        if (pages.isEmpty())
            settings.remove(kHomePagesSetting);
        else
            settings.setValue(kHomePagesSetting, HomeLayout::toJson(pages));
    });

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("homeScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(m_homeGridBoard);
    scroll->viewport()->setAutoFillBackground(false);
    outer->addWidget(scroll, 1);

    m_homeGridBoard->setPullGate([this, scroll] {
        const QScrollBar *bar = scroll->verticalScrollBar();
        return m_homeGridSearch && m_homeGridSearch->isHidden() &&
               bar->value() == bar->minimum();
    });
    m_homeGridBoard->setOnPull([this] { showHomeGridSearch(); });
    m_homeGridBoard->setOnType(
        [this](const QString &text) { showHomeGridSearch(text); });

    m_homeGridDots = new HomePageDots;
    m_homeGridDots->setOnPick([this](int index) {
        if (m_homeGridBoard)
            m_homeGridBoard->setPage(index);
    });
    m_homeGridBoard->setOnPagesChanged([this, scroll](int count, int current) {
        if (m_homeGridDots)
            m_homeGridDots->setPages(count, current);
        scroll->verticalScrollBar()->setValue(0);
    });
    m_homeGridDots->setPages(m_homeGridBoard->pageCount(),
                             m_homeGridBoard->page());
    outer->addWidget(m_homeGridDots);

    m_homeGridTimer = new QTimer(page);
    m_homeGridTimer->setInterval(kHomeTickMs);
    connect(m_homeGridTimer, &QTimer::timeout, this, [this] {
        if (!homeGridOnScreen()) {
            m_homeGridTimer->stop();
            return;
        }
        refreshHomeGridPage();
    });
    return page;
}

void MainWindow::showHomeGridSearch(const QString &typed)
{
    if (!m_homeGridSearch || !m_homeGridFilter)
        return;
    if (m_homeGridSearch->isHidden()) {
        const int height = m_homeGridSearch->sizeHint().height();
        m_homeGridSearch->setMaximumHeight(0);
        m_homeGridSearch->show();
        auto *grow = new QPropertyAnimation(m_homeGridSearch, "maximumHeight",
                                            m_homeGridSearch);
        grow->setDuration(kHomeSearchRevealMs);
        grow->setStartValue(0);
        grow->setEndValue(height);
        grow->setEasingCurve(QEasingCurve::OutCubic);
        connect(grow, &QPropertyAnimation::finished, m_homeGridSearch,
                [search = m_homeGridSearch] {
                    search->setMaximumHeight(QWIDGETSIZE_MAX);
                });
        grow->start(QAbstractAnimation::DeleteWhenStopped);
    }
    m_homeGridFilter->setFocus(Qt::OtherFocusReason);
    if (!typed.isEmpty())
        m_homeGridFilter->insert(typed);
}

void MainWindow::hideHomeGridSearch()
{
    if (!m_homeGridSearch || !m_homeGridFilter)
        return;
    const bool hadFocus = m_homeGridFilter->hasFocus();
    m_homeGridFilter->clear();
    m_homeGridSearch->hide();
    if (hadFocus && m_homeGridBoard)
        m_homeGridBoard->setFocus(Qt::OtherFocusReason);
}

bool MainWindow::homeGridOnScreen() const
{
    return m_sectionStack && m_homeGridBoard &&
           m_sectionStack->currentIndex() == kHomeGridSectionIndex &&
           isVisible() && !isMinimized();
}

QVector<HomeGroup> MainWindow::homeGridGroups() const
{
    QList<QWidget *> railWidgets;
    if (m_appNavigationRailLayout) {
        for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
            QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
            if (!widget)
                continue;
            if (auto *folder = dynamic_cast<RailFolderWidget *>(widget))
                railWidgets += folder->members();
            else
                railWidgets.append(widget);
        }
    }

    QHash<QString, QVector<HomeTile>> byGroup;
    for (QWidget *widget : std::as_const(railWidgets)) {
        auto *button = qobject_cast<QAbstractButton *>(widget);
        const QString key = widget->property("forkmeshRailKey").toString();
        if (!button || key.isEmpty() || key == QLatin1String("home") ||
            widget->isHidden())
            continue;
        auto *rail = dynamic_cast<ActivityRailButton *>(widget);
        HomeTile tile;
        tile.key = key;
        tile.title = rail ? rail->label() : button->text();
        if (tile.title.isEmpty())
            tile.title = button->accessibleName();
        if (tile.title.isEmpty())
            tile.title = key;
        tile.icon = rail ? rail->iconName() : QString();
        tile.tooltip = button->toolTip();
        if (rail) {
            tile.badge = rail->badgeCount();
            tile.urgent = rail->badgeUrgent();
            tile.pending = rail->pendingSyncCount();
            tile.live = rail->syncing() || rail->activityBlink();
            tile.alert = rail->alertTint();
        }
        if (key == QLatin1String("agents")) {
            for (const AgentSession &session : m_agentSessions)
                if (!session.merged && session.status == AgentStatus::Running) {
                    tile.live = true;
                    break;
                }
        }
        tile.detail = homeGridTileDetail(key, widget);
        byGroup[RailGroups::defaultGroupOf(key)].append(tile);
    }

    QVector<HomeGroup> groups;
    for (const RailGroups::BuiltIn &spec : RailGroups::builtInGroups()) {
        const QString id = QString::fromLatin1(spec.id);
        const QVector<HomeTile> tiles = byGroup.value(id);
        if (tiles.isEmpty())
            continue;
        groups.append({id, QString::fromLatin1(spec.title), tiles});
    }
    return groups;
}

QString MainWindow::homeGridTileDetail(const QString &key, QWidget *widget) const
{
    auto *rail = dynamic_cast<ActivityRailButton *>(widget);
    const int badge = rail ? rail->badgeCount() : 0;
    if (key == QLatin1String("agents")) {
        int running = 0;
        int waiting = 0;
        int failed = 0;
        for (const AgentSession &session : m_agentSessions) {
            if (session.merged)
                continue;
            if (session.status == AgentStatus::Running ||
                session.status == AgentStatus::Queued)
                ++running;
            else if (session.status == AgentStatus::Waiting)
                ++waiting;
            else if (session.status == AgentStatus::Failed)
                ++failed;
        }
        if (failed > 0)
            return QStringLiteral("%1 failed").arg(failed);
        if (waiting > 0)
            return QStringLiteral("%1 waiting").arg(waiting);
        if (running > 0)
            return QStringLiteral("%1 running").arg(running);
        return m_agentSessions.isEmpty() ? QStringLiteral("Idle")
                                         : plural(m_agentSessions.size(),
                                                  QStringLiteral("session"));
    }
    if (key == QLatin1String("chat"))
        return badge > 0 ? QStringLiteral("%1 unread").arg(badge)
                         : QStringLiteral("Caught up");
    if (key == QLatin1String("notifications"))
        return badge > 0 ? QStringLiteral("%1 new").arg(badge)
                         : QStringLiteral("All read");
    if (key == QLatin1String("git") && rail) {
        if (rail->syncing())
            return QStringLiteral("Syncing");
        if (rail->pendingSyncCount() > 0)
            return QStringLiteral("%1 to push").arg(rail->pendingSyncCount());
        return badge > 0 ? QStringLiteral("%1 changed").arg(badge)
                         : QStringLiteral("Clean");
    }
    if (rail && rail->activityBlink())
        return QStringLiteral("Working");
    if (rail && rail->syncing())
        return QStringLiteral("Syncing");
    if (key == QLatin1String("tasks") && badge > 0)
        return QStringLiteral("%1 open").arg(badge);
    if (key == QLatin1String("network") && badge > 0)
        return QStringLiteral("%1 online").arg(badge);
    if (key == QLatin1String("memory")) {
        const qint64 rss = SystemStats::residentBytes();
        if (rss > 0)
            return QStringLiteral("%1 MB").arg(rss / (1024 * 1024));
    }
    if (auto *button = qobject_cast<QAbstractButton *>(widget))
        return tooltipSummary(button->toolTip());
    return QString();
}

QString MainWindow::homeGridHeadline() const
{
    int running = 0;
    int waiting = 0;
    int failed = 0;
    for (const AgentSession &session : m_agentSessions) {
        if (session.merged)
            continue;
        if (session.status == AgentStatus::Running ||
            session.status == AgentStatus::Queued)
            ++running;
        else if (session.status == AgentStatus::Waiting)
            ++waiting;
        else if (session.status == AgentStatus::Failed)
            ++failed;
    }
    const auto badgeOf = [](QWidget *widget) {
        auto *rail = dynamic_cast<ActivityRailButton *>(widget);
        return rail && !widget->isHidden() ? rail->badgeCount() : 0;
    };
    const int pings = badgeOf(m_notificationButton);
    const int unread = badgeOf(m_chatButton);
    QStringList parts;
    if (running > 0)
        parts << plural(running, QStringLiteral("agent")) +
                     QStringLiteral(" working");
    if (waiting > 0)
        parts << QStringLiteral("%1 waiting on you").arg(waiting);
    if (failed > 0)
        parts << QStringLiteral("%1 failed").arg(failed);
    if (pings > 0)
        parts << plural(pings, QStringLiteral("ping"));
    if (unread > 0)
        parts << QStringLiteral("%1 unread").arg(unread);
    if (parts.isEmpty())
        return QStringLiteral("All quiet");
    return parts.mid(0, 4).join(QString::fromUtf8(" \xC2\xB7 "));
}

void MainWindow::refreshHomeGridPage()
{
    if (!m_homeGridBoard)
        return;
    const int hour = QTime::currentTime().hour();
    const QString greeting = hour >= 5 && hour < 12   ? QStringLiteral("Good morning")
                             : hour >= 12 && hour < 17 ? QStringLiteral("Good afternoon")
                             : hour >= 17 && hour < 22 ? QStringLiteral("Good evening")
                                                       : QStringLiteral("Up late");
    if (m_homeGridGreeting && m_homeGridGreeting->text() != greeting)
        m_homeGridGreeting->setText(greeting);
    const QString headline = homeGridHeadline();
    if (m_homeGridHeadline && m_homeGridHeadline->text() != headline)
        m_homeGridHeadline->setText(headline);
    m_homeGridBoard->setGroups(homeGridGroups());
    if (m_homeGridTimer && !m_homeGridTimer->isActive() && homeGridOnScreen())
        m_homeGridTimer->start();
}

bool MainWindow::activateHomeGridTile(const QString &key)
{
    if (key.isEmpty() || !m_appNavigationRailLayout)
        return false;
    QList<QWidget *> railWidgets;
    for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
        QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
        if (auto *folder = dynamic_cast<RailFolderWidget *>(widget))
            railWidgets += folder->members();
        else if (widget)
            railWidgets.append(widget);
    }
    for (QWidget *widget : std::as_const(railWidgets)) {
        if (widget->property("forkmeshRailKey").toString() != key)
            continue;
        auto *button = qobject_cast<QAbstractButton *>(widget);
        if (!button || !button->isEnabled())
            return false;
        button->click();
        return true;
    }
    return false;
}

#ifdef FORKMESH_WINDOW_TESTS
QVector<HomeGroup> MainWindow::testHomeGridGroups() const
{
    return homeGridGroups();
}

QString MainWindow::testHomeGridHeadline() const
{
    return m_homeGridHeadline ? m_homeGridHeadline->text() : QString();
}

bool MainWindow::testHomeGridSearchShown() const
{
    return m_homeGridSearch && !m_homeGridSearch->isHidden();
}
#endif
