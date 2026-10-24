#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
#include "OnboardingTour.h"
#include "StartupSplash.h"

using forkmesh::ui::OnboardingStep;
using forkmesh::ui::OnboardingTrack;
using forkmesh::ui::OnboardingTour;

namespace {

constexpr auto kFirstInstallPendingSetting =
    "onboarding/firstInstallPendingV1";
constexpr auto kWelcomeCompleteSetting = "onboarding/welcomeCompleteV1";
constexpr auto kTourCompleteSetting = "onboarding/tourCompleteV1";
constexpr auto kTourLastStepSetting = "onboarding/tourLastStep";

OnboardingStep tourStep(const char *id, const char *kicker,
                        const char *title, const char *body,
                        std::initializer_list<const char *> details,
                        QWidget *target, std::function<void()> reveal = {})
{
    OnboardingStep step;
    step.id = QString::fromLatin1(id);
    step.kicker = QString::fromUtf8(kicker);
    step.title = QString::fromUtf8(title);
    step.body = QString::fromUtf8(body);
    for (const char *detail : details)
        step.details.append(QString::fromUtf8(detail));
    step.target = target;
    step.reveal = std::move(reveal);
    return step;
}

} // namespace

void MainWindow::initializeFirstRunOnboardingState()
{
    QSettings settings;
    if (m_freshInstall &&
        !settings.value(QLatin1String(kWelcomeCompleteSetting), false).toBool()) {
        // The generated fun node name makes the next launch look established,
        // so remember that this install is still waiting for an explicit first-
        // run choice. Existing installations have neither this marker nor the
        // fresh-install signal and are deliberately grandfathered in.
        settings.setValue(QLatin1String(kFirstInstallPendingSetting), true);
    }
    m_firstRunWelcomePending =
        settings.value(QLatin1String(kFirstInstallPendingSetting), false)
            .toBool() &&
        !settings.value(QLatin1String(kWelcomeCompleteSetting), false).toBool();
}

void MainWindow::maybeShowFirstRunWelcome()
{
    m_firstRunWelcomeScheduled = false;
    if (!m_firstRunWelcomePending || !isVisible())
        return;
    if (!m_tourPackageInstalled && !Theme::isNext())
        return;
    if (m_onboardingTour && m_onboardingTour->isVisible())
        return;
    if (forkmesh::ui::activeStartupSplash()) {
        m_firstRunWelcomeScheduled = true;
        QTimer::singleShot(350, this,
                           &MainWindow::maybeShowFirstRunWelcome);
        return;
    }
    if (forkmesh::ui::NextAccess::maybeShowWelcome(*this))
        return;
    showFirstRunWelcome();
}

void MainWindow::showFirstRunWelcome()
{
    if (!centralWidget() || !m_tourPackageInstalled)
        return;
    if (!m_onboardingTour) {
        m_onboardingTour = new OnboardingTour(centralWidget());
        m_onboardingTour->startTourRequested = [this](OnboardingTrack track) {
            completeFirstRunWelcome();
            startFeatureTourTrack(track);
        };
        m_onboardingTour->welcomeDismissed = [this] {
            completeFirstRunWelcome();
            showSection(0);
        };
        m_onboardingTour->tourClosed = [this](bool completed) {
            QSettings settings;
            if (completed) {
                settings.setValue(QLatin1String(kTourCompleteSetting), true);
                settings.remove(QLatin1String(kTourLastStepSetting));
                showSection(0);
            } else if (m_onboardingTour) {
                settings.setValue(QLatin1String(kTourLastStepSetting),
                                  m_onboardingTour->currentStepId());
            }
        };
    }
    m_onboardingTour->setGeometry(centralWidget()->rect());
    m_onboardingTour->setTrackSizes(featureTourSteps());
    m_onboardingTour->showWelcome(machineNodeName());
}

void MainWindow::completeFirstRunWelcome()
{
    QSettings settings;
    settings.setValue(QLatin1String(kWelcomeCompleteSetting), true);
    settings.remove(QLatin1String(kFirstInstallPendingSetting));
    settings.sync();
    m_firstRunWelcomePending = false;
    m_firstRunWelcomeScheduled = false;
}

void MainWindow::syncTourPackage(bool installed)
{
    m_tourPackageInstalled = installed;
    if (m_tourNavButton) {
        m_tourNavButton->setEnabled(installed);
        if (!installed)
            m_tourNavButton->hide();
        else if (m_tourNavButton->parentWidget())
            m_tourNavButton->show();
    }
    if (installed || !m_onboardingTour)
        return;
    OnboardingTour *tour = m_onboardingTour;
    m_onboardingTour = nullptr;
    tour->hide();
    tour->deleteLater();
}

void MainWindow::ensureTourTargetVisible(QWidget *target)
{
    if (!target)
        return;
    auto *rail = findChild<QScrollArea *>(QStringLiteral("appNavigationRail"));
    if (rail && (target == rail->widget() || rail->widget()->isAncestorOf(target)))
        rail->ensureWidgetVisible(target, 6, 12);
}

void MainWindow::startFeatureTour()
{
    if (!prepareFeatureTour())
        return;
    m_onboardingTour->setGeometry(centralWidget()->rect());
    m_onboardingTour->setTrackSizes(featureTourSteps());
    m_onboardingTour->showChooser();
}

void MainWindow::startFeatureTourTrack(OnboardingTrack track)
{
    if (!prepareFeatureTour())
        return;
    // The tour points at the rail and the status strip, which the reduced
    // views hide.
    if (forkmesh::ui::appViewModeHidesShellChrome(m_viewMode))
        setAppViewMode(forkmesh::ui::AppViewMode::Standard);
    m_onboardingTour->setGeometry(centralWidget()->rect());
    m_onboardingTour->startTour(
        OnboardingTour::stepsForTrack(featureTourSteps(), track));
}

bool MainWindow::prepareFeatureTour()
{
    if (!centralWidget() || !m_tourPackageInstalled)
        return false;
    // Clicking the permanent Tour action during the short first-frame delay is
    // itself an explicit first-run choice. Resolve the welcome before its timer
    // can replace the tour that the user just opened.
    if (m_firstRunWelcomePending)
        completeFirstRunWelcome();
    if (!m_onboardingTour) {
        // The replay button can be the first route here on an older install,
        // where the first-run welcome never needed to construct the overlay.
        showFirstRunWelcome();
        if (m_onboardingTour)
            m_onboardingTour->hide();
    }
    return m_onboardingTour != nullptr;
}

QVector<OnboardingStep> MainWindow::featureTourSteps()
{
    auto *rail = findChild<QWidget *>(QStringLiteral("appNavigationRail"));
    auto *breadcrumb =
        findChild<QWidget *>(QStringLiteral("breadcrumbBar"));
    auto *statusBar = findChild<QWidget *>(QStringLiteral("appStatusArea"));

    const auto revealSection = [this](int index, QWidget *target) {
        return [this, index, target] {
            showSection(index);
            ensureTourTargetVisible(target);
        };
    };
    const auto revealHome = [this](QWidget *target) {
        return [this, target] {
            showSection(0);
            ensureTourTargetVisible(target);
        };
    };

    QVector<OnboardingStep> steps;
    steps.reserve(25);
    steps.append(tourStep(
        "navigation", "THE DESKTOP", "Everything starts on the rail",
        "Shared work lives at the top; your personal tools and preferences "
        "live below. We’ll visit the real surfaces without creating or changing data.",
        {"Checked buttons show where you are.",
         "Badges call out unread work and live activity.",
         "Scroll the rail on a smaller screen—nothing is hidden permanently."},
        rail, [this, rail] {
            showSection(0);
            ensureTourTargetVisible(rail);
        }));

    steps.append(tourStep(
        "agents", "AI WORKSPACE", "Delegate work to coding agents",
        "Start, resume, and supervise Claude Code or Codex sessions while each "
        "agent works safely in its own branch and worktree.",
        {"Choose provider, model, effort, permissions, and execution mode.",
         "Follow live transcripts, tool activity, images, cost, and usage limits.",
         "Review the agent diff, open its issue or pull request, merge it, or clean it up."},
        m_agentsNavButton, [this] {
            openAgentsOverview();
            ensureTourTargetVisible(m_agentsNavButton);
        }));

    steps.append(tourStep(
        "prompt", "AVAILABLE EVERYWHERE", "A prompt that follows your work",
        "The floating composer stays available on every page, so the repository, "
        "task, notification, or log you are looking at can become agent context.",
        {"Switch between Ask, Edit, Plan, and automatic routing.",
         "Pick an agent, model, effort, and speed before sending.",
         "Attach files, screenshots, pasted images, voice, or slash-command shortcuts."},
        m_promptOverlayHost, revealHome(m_promptOverlayHost)));

    steps.append(tourStep(
        "repositories", "THE MESH", "Browse repositories across every node",
        "Repos is the network-wide catalog: discover work hosted by your node, "
        "another mirror, or a relay, then preview it before bringing it local.",
        {"Search and inspect public repositories without cloning first.",
         "Clone, fork, or add an existing local Git checkout.",
         "Mirror public work broadly; private replicas stay encrypted and access-controlled."},
        m_reposNavButton,
        revealSection(kNetworkReposSectionIndex, m_reposNavButton)));

    steps.append(tourStep(
        "repo-switcher", "REPOSITORY HOME", "Open, import, and switch work here",
        "The project picker is the doorway into a local workspace. Under the "
        "current project it shows its instance, your account and the active repository.",
        {"Create a repository, import from GitHub or GitLab, or add a local folder.",
         "Switch branches and repositories without leaving the desktop.",
         "See mirror state, commit identity, and the checked-out revision at a glance."},
        m_projectMenuButton, revealHome(m_projectMenuButton)));

    steps.append(tourStep(
        "repo-plan", "INSIDE A REPOSITORY", "Plan and discuss the work",
        "Each repository becomes a complete collaboration space—not just a file browser.",
        {"Issues include search, boards, labels, milestones, sub-issues, votes, and bounties.",
         "Discussions keep longer-form decisions beside the code.",
         "Projects turn issues into roadmaps, milestones, and Gantt-style plans."},
        m_projectMenuButton, revealHome(m_projectMenuButton)));

    steps.append(tourStep(
        "repo-ship", "INSIDE A REPOSITORY", "Review, merge, and ship changes",
        "The code and source-control views carry a change from an edit through peer review.",
        {"Browse and edit files, inspect history and symbols, then stage, commit, pull, and push.",
         "Pull requests include conversation, commits, diffs, AI review, approvals, and merge queue.",
         "Secret scanning and quality/security views catch risky changes before they leave."},
        m_projectMenuButton, revealHome(m_projectMenuButton)));

    steps.append(tourStep(
        "repo-operate", "INSIDE A REPOSITORY", "Automate and operate every version",
        "Repository operations stay visible, reproducible, and tied to the exact branch that ran them.",
        {"Actions run workflows on labeled nodes with approval, logs, artifacts, and repair handoff.",
         "Branches and worktrees show ahead/behind state, conflicts, agent ownership, and cleanup.",
         "Releases, artifacts, mirrors, insights, size maps, shortcuts, and repo settings round out the workspace."},
        m_projectMenuButton, revealHome(m_projectMenuButton)));

    steps.append(tourStep(
        "market", "EXTEND FORKMESH", "Install features from the Market",
        "The Market adds creator packages without bloating the core desktop. "
        "Your Library shows exactly what is installed.",
        {"Add apps, integrations, themes, agents, and desktop tools.",
         "Inspect package details and permissions before installing.",
         "Publish your own package to a marketplace when it is ready."},
        m_marketNavButton,
        revealSection(kMarketSectionIndex, m_marketNavButton)));

    QWidget *ledgerTarget =
        m_ledgerNavButton && m_ledgerNavButton->isVisible()
            ? static_cast<QWidget *>(m_ledgerNavButton)
            : static_cast<QWidget *>(m_marketNavButton);
    steps.append(tourStep(
        "ledger", "OPTIONAL PACKAGE", "Keep a transparent project ledger",
        "Ledger combines relay-owned reward and payment records with private "
        "income or expense entries stored on this machine.",
        {"Track exact SOL or USD records, counterparties, categories, and outstanding balances.",
         "Watch named wallet balances and live spot estimates.",
         "Review, send, verify, settle, or undo payment records with explicit signing."},
        ledgerTarget, [this, ledgerTarget] {
            if (m_ledgerFeature)
                openLedger();
            else
                showSection(kMarketSectionIndex);
            ensureTourTargetVisible(ledgerTarget);
        }));

    steps.append(tourStep(
        "chat", "COLLABORATION", "Talk where the work lives",
        "Chat connects mesh channels, direct messages, repository rooms, office "
        "rooms, and optional Discord bridging in one conversation surface.",
        {"Send formatted text, links, reactions, files, images, video, and voice.",
         "Private rooms use encrypted payloads and managed invitations.",
         "Presence, typing, unread state, members, and channel controls stay in view."},
        m_chatButton, [this] {
            showChatView();
            ensureTourTargetVisible(m_chatButton);
        }));

    steps.append(tourStep(
        "pings", "YOUR INBOX", "Pings collect what needs attention",
        "Agent results, reviews, issues, chat, mirrors, actions, nodes, payments, "
        "and system health all arrive in one durable inbox.",
        {"Open a ping straight into the repository, conversation, agent, or action it names.",
         "Mark items read, filter them, or pause interruptions while keeping the history.",
         "Choose in-app, native desktop, and email delivery in Settings."},
        m_notificationButton, [this] {
            showNotifications();
            ensureTourTargetVisible(m_notificationButton);
        }));

    steps.append(tourStep(
        "tasks", "ORGANIZATIONS", "Turn shared work into an accountable task",
        "Organization tasks connect people, repositories, agents, and QA without "
        "forcing early ideas into the issue tracker.",
        {"Search, assign, start, complete, reopen, and create follow-ups.",
         "Send work to an agent with the composer’s current model and mode.",
         "Request QA, attach a test plan, or promote a mature task into a repository issue."},
        m_tasksNavButton,
        revealSection(kOrganizationTasksSectionIndex, m_tasksNavButton)));

    steps.append(tourStep(
        "calendar", "TIME & ACTIVITY", "See plans and repository activity together",
        "Calendar combines private and organization events with repository commits, "
        "releases, issues, and pull activity on the same timeline.",
        {"Switch month, week, day, and agenda views with search and time-zone support.",
         "Invite attendees, RSVP, recur events, and scope calendars to an organization.",
         "Ask an agent to suggest calendar entries from natural language."},
        m_calendarNavButton,
        revealSection(kCalendarSectionIndex, m_calendarNavButton)));

    steps.append(tourStep(
        "notes", "PERSONAL KNOWLEDGE", "Capture Markdown notes before they disappear",
        "Notes can stay local, sync to your account, or be shared publicly when "
        "you deliberately choose to publish them.",
        {"Edit Markdown with attachments and version history.",
         "Switch storage and sharing per note instead of globally.",
         "Turn a note into a task or repository issue, or add it directly to an agent prompt."},
        m_notesNavButton,
        revealSection(kNotesSectionIndex, m_notesNavButton)));

    steps.append(tourStep(
        "files", "LOCAL WORKSPACE", "Browse and edit files without losing context",
        "Files moves between this machine, the open repository, and encrypted "
        "Coves while keeping an editor and symbol outline alongside the tree.",
        {"Reveal hidden files, search and replace, preview Markdown, and inspect history.",
         "Save, commit, pull, and push repository files from the editor.",
         "Coves provide encrypted shared files with explicit membership and versioning."},
        m_filesNavButton, [this] {
            showSection(kFilesSectionIndex);
            ensureTourTargetVisible(m_filesNavButton);
        }));

    steps.append(tourStep(
        "team", "AI SPECIAL AGENTS", "Give recurring agents a role and a memory",
        "Special agents turn one-off sessions into named AI agents with a defined role, "
        "repository, model, schedule, and operating instructions.",
        {"Create and edit special agent profiles, schedules, skills, and log monitors.",
         "See their latest shipment, active session, work history, cost, and performance.",
         "Start a special agent manually or let scheduled and monitored work find it."},
        m_teamNavButton,
        revealSection(kTeamSectionIndex, m_teamNavButton)));

    QWidget *usersTarget =
        m_usersNavButton && m_usersNavButton->isVisible()
            ? static_cast<QWidget *>(m_usersNavButton)
            : static_cast<QWidget *>(m_teamNavButton);
    steps.append(tourStep(
        "users", "PEOPLE & WORLD", "Understand who is on the mesh",
        "User profiles connect identity, contributions, public World presence, "
        "activity, repositories, and payout information.",
        {"Open member profiles from chat, issues, reviews, and repository activity.",
         "Administrators also get the Users activity and World statistics surface.",
         "Organization membership, teams, roles, and repository permissions live in Settings."},
        usersTarget, [this, usersTarget] {
            if (m_isAdmin)
                showSection(kUsersSectionIndex);
            else
                showSection(kTeamSectionIndex);
            ensureTourTargetVisible(usersTarget);
        }));

    steps.append(tourStep(
        "network", "MESH HEALTH", "See every relay, node, and host",
        "Network explains how this desktop reaches the mesh and whether each "
        "piece is healthy before a failure reaches your work.",
        {"Inspect relays, desktop nodes, provisioned hosts, latency, and online state.",
         "Trace websocket and Durable Object health plus request/backoff history.",
         "Review the outbound firewall and allow-list every external destination explicitly."},
        m_networkNavButton,
        revealSection(kNetworkDiagnosticsSectionIndex, m_networkNavButton)));

    steps.append(tourStep(
        "control", "THIS NODE", "Operate the infrastructure you control",
        "Control is the local operations console for this desktop and its mirror fleet.",
        {"Manage mirror serving, synchronization, integrity, keys, and connected hosts.",
         "Bootstrap Cloudflare tunnels, domains, deployments, and managed mirror nodes.",
         "Review diagnostics, reward-pool signing, capacity, security, and recovery state."},
        m_controlNodeNavButton,
        revealSection(kControlNodeSectionIndex, m_controlNodeNavButton)));

    steps.append(tourStep(
        "settings", "MAKE IT YOURS", "Settings covers the whole operating environment",
        "Configure ForkMesh once, then keep deeper controls close when your workflow changes.",
        {"Profile, node identity, appearance, startup, updates, VM isolation, and repository storage.",
         "Agents, accounts, models, voice, apps, notifications, organizations, secrets, and MCP.",
         "Security reporting, encrypted identity backup, data export/import, local backups, and uninstall."},
        m_settingsNavButton,
        revealSection(1, m_settingsNavButton)));

    steps.append(tourStep(
        "search", "CTRL+K", "Search across the desktop from one box",
        "Global search understands repositories, commits, issues, pulls, discussions, "
        "files, users, agents, actions, notifications, and navigation destinations.",
        {"Press Ctrl+K from anywhere to focus it.",
         "Use keyboard results to jump directly to the matching object.",
         "Use Ctrl+F when you only want to find visible text on the current page."},
        m_globalSearch, revealHome(m_globalSearch)));

    steps.append(tourStep(
        "capture", "VISUAL CONTEXT", "Capture, mark up, and send what you see",
        "Capture grabs a screen region, opens a lightweight annotation surface, "
        "and attaches the result to the prompt—without leaving a mystery file behind.",
        {"Select any region across your desktop.",
         "Add arrows, boxes, freehand marks, blur, text, and crops before sharing.",
         "Installed capture support also powers visual context for agent work."},
        m_navScreenshotButton, [this] {
            showSection(0);
            ensureTourTargetVisible(m_navScreenshotButton);
        }));

    steps.append(tourStep(
        "activity", "ALWAYS VISIBLE", "Live activity and health stay at the edges",
        "The top chrome shows relay and repository context plus live agents, nodes, "
        "and actions. The footer shows branch, worktree, commit, background work, "
        "offline mode, and the exact app build.",
        {"Click live dots and action cells to open the work behind them.",
         "Click the version to reveal CPU, memory, swap, disk, logs, and website health.",
         "The full Log page adds filters, timelines, frequencies, source links, and cloud tailing."},
        statusBar ? statusBar : breadcrumb, [this, statusBar] {
            showSection(4);
            ensureTourTargetVisible(statusBar);
        }));

    steps.append(tourStep(
        "ready", "YOU’RE READY", "The mesh is yours",
        "Start with a repository, a question for an agent, or a conversation. "
        "ForkMesh will keep the rest of the workspace close as the work grows.",
        {"Open or add a repository from the top-left switcher.",
         "Write a goal in the floating prompt and choose an agent.",
         "Come back to Tour in the rail for a deeper track any time."},
        m_tourNavButton, revealHome(m_tourNavButton)));

    const QHash<QString, OnboardingTrack> tracks{
        {QStringLiteral("navigation"), OnboardingTrack::Essentials},
        {QStringLiteral("agents"), OnboardingTrack::Essentials},
        {QStringLiteral("prompt"), OnboardingTrack::Essentials},
        {QStringLiteral("repositories"), OnboardingTrack::Essentials},
        {QStringLiteral("repo-switcher"), OnboardingTrack::Essentials},
        {QStringLiteral("pings"), OnboardingTrack::Essentials},
        {QStringLiteral("search"), OnboardingTrack::Essentials},
        {QStringLiteral("ready"), OnboardingTrack::Essentials},
        {QStringLiteral("repo-plan"), OnboardingTrack::Advanced},
        {QStringLiteral("repo-ship"), OnboardingTrack::Advanced},
        {QStringLiteral("repo-operate"), OnboardingTrack::Advanced},
        {QStringLiteral("market"), OnboardingTrack::Advanced},
        {QStringLiteral("chat"), OnboardingTrack::Advanced},
        {QStringLiteral("files"), OnboardingTrack::Advanced},
        {QStringLiteral("settings"), OnboardingTrack::Advanced},
        {QStringLiteral("capture"), OnboardingTrack::Advanced},
    };
    for (OnboardingStep &step : steps)
        step.track = tracks.value(step.id, OnboardingTrack::Everything);
    return steps;
}
