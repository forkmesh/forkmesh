#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
#include "WorkspaceLayout.h"

#include <QActionGroup>

using namespace forkmesh::ui;

// The window's layout presets and the "Change view" button that switches
// between them, parked at the right edge of the chrome bar beside the
// minimize/maximize/close controls.
//
// Everything a mode changes is a property of widgets the rest of the app
// already owns - the navigation rail's visibility and the status strip's - so
// no page has to know a mode exists. The one surface a mode adds, Workspace's
// terminal pane, is built at the bottom of this file; Agents places it as its
// splitter's third pane and only applyAppViewMode() shows it.

namespace {

// Menu-item tint that reads on either theme, matching the other top-bar menus.
QColor viewMenuIconColor()
{
    return QColor(currentThemeIsDark() ? QStringLiteral("#c9d1d9")
                                       : QStringLiteral("#1f2328"));
}

} // namespace

QPushButton *MainWindow::buildViewModeButton()
{
    auto *button = new QPushButton;
    button->setObjectName(QStringLiteral("viewModeButton"));
    button->setCursor(Qt::PointingHandCursor);
    // The chrome bar is a drag handle for the frameless window; a focus ring
    // here would be the only focusable thing in that strip.
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(32, 30);
    connect(button, &QPushButton::clicked, this, &MainWindow::showViewModeMenu);
    m_viewModeButton = button;
    updateViewModeButton();
    return button;
}

void MainWindow::updateViewModeButton()
{
    if (!m_viewModeButton)
        return;
    setOcticon(m_viewModeButton, appViewModeIcon(m_viewMode), 14);
    m_viewModeButton->setToolTip(
        QStringLiteral("View: %1\n%2\n\nClick to pick a view; Ctrl+Shift+L "
                       "cycles through them.")
            .arg(appViewModeName(m_viewMode), appViewModeDescription(m_viewMode)));
    // Screen readers get the live mode, not a static label: the icon is the
    // only other thing that says which view is on.
    m_viewModeButton->setAccessibleName(
        QStringLiteral("Change view - currently %1")
            .arg(appViewModeName(m_viewMode)));
}

void MainWindow::showViewModeMenu()
{
    if (!m_viewModeButton)
        return;
    QMenu menu(this);
    menu.setToolTipsVisible(true); // each mode explains itself on hover

    QAction *header = menu.addAction(QStringLiteral("View"));
    header->setEnabled(false);
    menu.addSeparator();

    auto *group = new QActionGroup(&menu);
    group->setExclusive(true);
    const QColor iconColor = viewMenuIconColor();
    for (AppViewMode mode : kAppViewModes) {
        QAction *entry =
            menu.addAction(themedOcticon(appViewModeIcon(mode), iconColor, 14),
                           appViewModeName(mode));
        entry->setCheckable(true);
        entry->setChecked(mode == m_viewMode);
        entry->setActionGroup(group);
        entry->setToolTip(appViewModeDescription(mode));
        connect(entry, &QAction::triggered, this,
                [this, mode] { setAppViewMode(mode); });
    }

    menu.addSeparator();
    QAction *next = menu.addAction(QStringLiteral("Next view"));
    next->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L));
    connect(next, &QAction::triggered, this, &MainWindow::cycleAppViewMode);

    menu.addSeparator();
    QAction *designHeader = menu.addAction(QStringLiteral("Interface"));
    designHeader->setEnabled(false);
    auto *designGroup = new QActionGroup(&menu);
    designGroup->setExclusive(true);
    const bool nextDesign = Theme::isNext();
    for (const bool wantNext : {false, true}) {
        QAction *entry = menu.addAction(wantNext ? QStringLiteral("New (preview)")
                                                 : QStringLiteral("Classic"));
        entry->setObjectName(wantNext ? QStringLiteral("viewMenuDesignNext")
                                      : QStringLiteral("viewMenuDesignClassic"));
        entry->setCheckable(true);
        entry->setChecked(wantNext == nextDesign);
        entry->setActionGroup(designGroup);
        entry->setToolTip(wantNext
                              ? QStringLiteral("The new interface is a preview; "
                                               "switch back at any time.")
                              : QStringLiteral("The current ForkMesh interface."));
        connect(entry, &QAction::triggered, this, [this, wantNext] {
            if (Theme::isNext() == wantNext)
                return;
            const QString value = wantNext ? QStringLiteral("next")
                                           : QStringLiteral("classic");
            if (auto *combo = findChild<QComboBox *>(
                    QStringLiteral("appDesignCombo"))) {
                const int index = combo->findData(value);
                if (index >= 0 && index != combo->currentIndex()) {
                    combo->setCurrentIndex(index);
                    return;
                }
            }
            QSettings().setValue(QLatin1String(Theme::kDesignSetting), value);
            applyTheme(m_packageRegistry.get());
        });
    }

    if (Theme::isNext()) {
        menu.addSeparator();
        QAction *layoutHeader = menu.addAction(QStringLiteral("Agents layout"));
        layoutHeader->setEnabled(false);
        auto *layoutGroup = new QActionGroup(&menu);
        layoutGroup->setExclusive(true);
        const bool classicLayout = workspaceLayoutClassic();
        for (const bool classic : {false, true}) {
            QAction *entry = menu.addAction(classic ? QStringLiteral("Classic")
                                                    : QStringLiteral("New"));
            entry->setObjectName(classic
                                     ? QStringLiteral("workspaceLayoutClassic")
                                     : QStringLiteral("workspaceLayoutNew"));
            entry->setCheckable(true);
            entry->setChecked(classic == classicLayout);
            entry->setActionGroup(layoutGroup);
            entry->setToolTip(
                classic
                    ? QStringLiteral("The earlier Agents page: the action bar, "
                                     "the list views and the detail buttons.")
                    : QStringLiteral("Session cards, one conversation header "
                                     "and a side panel for the diff, the "
                                     "terminal and the browser."));
            connect(entry, &QAction::triggered, this, [this, classic] {
                if (workspaceLayoutClassic() == classic)
                    return;
                setWorkspaceLayoutClassic(classic);
                NextAccess::syncWorkspace(*this);
                logSystem(QStringLiteral("Agents layout switched to %1")
                              .arg(classic ? QStringLiteral("Classic")
                                           : QStringLiteral("New")));
            });
        }
    }

    // Anchored to the button's right edge because the button lives at the
    // window's right edge; Qt flips the popup back inside the screen.
    menu.exec(m_viewModeButton->mapToGlobal(
        QPoint(m_viewModeButton->width(), m_viewModeButton->height())));
}

void MainWindow::restoreAppViewMode()
{
    m_viewMode = appViewModeFromKey(
        QSettings().value(kAppViewModeSetting).toString(), AppViewMode::Standard);
    // Deliberately not openAgentsOverview(): startup already decides which
    // repository and page to reopen, and Agents is one of the deferred
    // surfaces. buildAgentsTab() re-applies the mode once it exists, which is
    // what actually shows the terminal pane for a restored Workspace.
    applyAppViewMode();
}

void MainWindow::setAppViewMode(AppViewMode mode)
{
    const bool changed = mode != m_viewMode;
    m_viewMode = mode;
    QSettings().setValue(kAppViewModeSetting, appViewModeKey(mode));
    // Open Agents before applying, so the terminal pane exists by the time it
    // has to be shown.
    if (appViewModeOpensAgents(mode))
        openAgentsOverview();
    applyAppViewMode();
    if (changed)
        logSystem(QStringLiteral("View switched to %1").arg(appViewModeName(mode)));
}

void MainWindow::cycleAppViewMode()
{
    setAppViewMode(nextAppViewMode(m_viewMode));
}

void MainWindow::applyAppViewMode()
{
    const bool reduced = appViewModeHidesShellChrome(m_viewMode);
    if (m_appNavigationRail)
        m_appNavigationRail->setVisible(!reduced);
    if (m_appStatusArea)
        m_appStatusArea->setVisible(!reduced);
    applyTeammateRailVisibility();
    applyPackageRailVisibility();

    const bool terminal = appViewModeShowsWorkspaceTerminal(m_viewMode);
    if (m_workspaceTerminalDock &&
        terminal == m_workspaceTerminalDock->isHidden()) {
        m_workspaceTerminalDock->setVisible(terminal);
        if (terminal && m_agentWorkspaceSplit) {
            // A hidden splitter pane comes back at zero width. Give it a
            // terminal wide enough for an 80-column line and leave the
            // session list and transcript their share of the rest.
            QList<int> sizes = m_agentWorkspaceSplit->sizes();
            if (sizes.size() == 3 && sizes.at(2) < 240) {
                const int total = qMax(m_agentWorkspaceSplit->width(), 960);
                const int pane = qBound(320, total / 3, 640);
                const int rest = qMax(0, sizes.at(0) + sizes.at(1) - pane);
                const int listShare = sizes.at(0) + sizes.at(1) > 0
                                          ? sizes.at(0) * rest /
                                                (sizes.at(0) + sizes.at(1))
                                          : rest / 2;
                m_agentWorkspaceSplit->setSizes(
                    {listShare, rest - listShare, pane});
            }
        }
        if (terminal && m_workspaceTerminal && !m_workspaceTerminal->isRunning())
            m_workspaceTerminal->resumeOr([this] { startWorkspaceTerminal(); });
    }

    NextAccess::syncWorkspace(*this);
    updateViewModeButton();
    // Both hidden surfaces change where the bottom of the content area is, and
    // the composer and log overlays are positioned against it.
    positionGlobalFooterOverlays();
}

QWidget *MainWindow::buildWorkspaceTerminalDock()
{
    auto *dock = new QWidget;
    dock->setObjectName(QStringLiteral("workspaceTerminalDock"));
    dock->setMinimumWidth(240);
    auto *layout = new QVBoxLayout(dock);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(4, 0, 0, 0);
    header->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("Terminal"));
    title->setObjectName(QStringLiteral("agentFilesHeading"));
    header->addWidget(title);
    auto *restart = new QPushButton;
    restart->setObjectName(QStringLiteral("windowChromeButton"));
    restart->setCursor(Qt::PointingHandCursor);
    restart->setFocusPolicy(Qt::NoFocus);
    restart->setFixedSize(26, 24);
    restart->setToolTip(QStringLiteral(
        "New shell in the selected session's worktree (ends the current one)"));
    setOcticon(restart, QStringLiteral("plus"), 12);
    connect(restart, &QPushButton::clicked, this,
            &MainWindow::startWorkspaceTerminal);
    header->addWidget(restart);
    header->addStretch();
    auto *leave = new QPushButton;
    leave->setObjectName(QStringLiteral("windowChromeButton"));
    leave->setCursor(Qt::PointingHandCursor);
    leave->setFocusPolicy(Qt::NoFocus);
    leave->setFixedSize(26, 24);
    leave->setToolTip(QStringLiteral("Close the terminal pane (Standard view)"));
    setOcticon(leave, QStringLiteral("x"), 12);
    connect(leave, &QPushButton::clicked, this,
            [this] { setAppViewMode(AppViewMode::Standard); });
    header->addWidget(leave);
    layout->addLayout(header);

    m_workspaceTerminal = new TerminalWidget;
    m_workspaceTerminal->setObjectName(QStringLiteral("workspaceTerminal"));
    m_workspaceTerminal->setPersistKey(QStringLiteral("workspace"));
    layout->addWidget(m_workspaceTerminal, 1);

    m_workspaceTerminalDock = dock;
    dock->hide();
    return dock;
}

void MainWindow::startWorkspaceTerminal()
{
    if (!m_workspaceTerminal)
        return;
#ifdef FORKMESH_WINDOW_TESTS
    // Window tests check the layout, not a live shell: forking one per test
    // would only leave child processes behind the fixture.
    return;
#else
    QString cwd;
    if (m_selectedAgentSessionId > 0)
        cwd = sessionWorkdir(m_selectedAgentSessionId);
    if (cwd.isEmpty() || !QDir(cwd).exists())
        cwd = QDir::homePath();
#ifdef Q_OS_WIN
    // cmd /k with a no-op keeps the interactive host shell open.
    m_workspaceTerminal->runCommand(QStringLiteral("rem"), cwd, {}, true);
#else
    // exec replaces the wrapper, so the pane is the user's own shell rather
    // than bash nested under the wrapper's exit banner.
    m_workspaceTerminal->runCommand(
        QStringLiteral("exec \"${SHELL:-bash}\" -i"), cwd, {}, false);
#endif
    m_workspaceTerminal->setFocus(Qt::OtherFocusReason);
#endif
}

#ifdef FORKMESH_WINDOW_TESTS
QWidget *MainWindow::testWorkspaceTerminal() const
{
    return m_workspaceTerminal;
}
#endif
