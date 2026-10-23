
#include "Tooltips.h"
#include "GitRefusal.h"
#include "BackgroundRunnerBadge.h"
#include "SettingsSectionsView.h"
#include "HomeBoard.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
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
#include "D1QuerySource.h"
#include "DatabaseManagerWidget.h"
#include "DiscordClient.h"
#include "KebabHeaderView.h"
#include "LogTimelineChart.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "PromptDraftStore.h"
#include "PromptSpellCheckHighlighter.h"
#include "PublicMirrorRuntime.h"
#include "QtRuntimeDiagnostics.h"
#include "RepoSecurity.h"
#include "RewardPoolSigner.h"
#include "TerminalWidget.h"
#include "WindowBackdrop.h"
#include "WindowFade.h"
#include "WorldSpeechBridge.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDrag>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTreeWidgetItemIterator>
#include <QActionGroup>
#include <QInputDialog>
#include <QMimeData>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkInformation>
#include <QPointer>
#include <QProcess>

#include <algorithm>
#include <cmath>
#include <functional>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <signal.h>
#endif

using namespace forkmesh::ui;

QWidget *MainWindow::buildBreadcrumb()
{
    auto *bar = new QWidget;
    bar->setObjectName("breadcrumbBar");

    m_relayMenuButton = new QPushButton;
    m_relayMenuButton->setObjectName("relayMenuButton");
    m_relayMenuButton->setCursor(Qt::PointingHandCursor);
    m_relayMenuButton->setIconSize(QSize(25, 25));
    m_relayMenuButton->setFixedWidth(railItemWidth());
    m_relayMenuButton->setToolTip("Switch, search, or add relays");
    connect(m_relayMenuButton, &QPushButton::clicked, this,
            &MainWindow::showRelayMenu);

    // Red dot pinned over the favicon while a freshly launched instance waits
    // to be linked, with the Approve button that opens the join
    // dialog right beside it. Both stay hidden until the signed heartbeat
    // reply reports a pending join request for this admin.
    m_relayJoinDot = new QLabel(m_relayMenuButton);
    m_relayJoinDot->setObjectName(QStringLiteral("relayJoinDot"));
    m_relayJoinDot->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_relayJoinDot->setFixedSize(10, 10);
    m_relayJoinDot->hide();
    m_relayJoinApproveButton = new QPushButton(QStringLiteral("Approve"));
    m_relayJoinApproveButton->setObjectName(
        QStringLiteral("relayJoinApproveButton"));
    m_relayJoinApproveButton->setCursor(Qt::PointingHandCursor);
    m_relayJoinApproveButton->hide();
    connect(m_relayJoinApproveButton, &QPushButton::clicked, this,
            &MainWindow::showRelayJoinApprovalDialog);

    m_runnerAttentionButton = new QPushButton;
    m_runnerAttentionButton->setObjectName(
        QStringLiteral("runnerSignInBadge"));
    m_runnerAttentionButton->setCursor(Qt::PointingHandCursor);
    m_runnerAttentionButton->setProperty("severity", QStringLiteral("expired"));
    m_runnerAttentionButton->hide();
    connect(m_runnerAttentionButton, &QPushButton::clicked, this,
            &MainWindow::openRunnerAttention);

    m_relaySpeedDot = new RelaySpeedDot(m_relayMenuButton);

    m_nodeMenuButton = new QPushButton;
    m_nodeMenuButton->setObjectName("nodeMenuButton");
    m_nodeMenuButton->setCursor(Qt::PointingHandCursor);
    m_nodeMenuButton->setToolTip("Pick a node to view its repositories");
    connect(m_nodeMenuButton, &QPushButton::clicked, this, &MainWindow::showNodeMenu);

    m_navSolanaBalance = new QLabel(QString::fromUtf8("\xE2\x80\x94 SOL"));
    m_navSolanaBalance->setObjectName("navSolanaBalance");
    m_navSolanaBalance->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    m_navSolanaBalance->setTextFormat(Qt::RichText);
    m_navSolanaBalance->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    m_navSolanaBalance->setOpenExternalLinks(false);
    m_navSolanaBalance->setCursor(Qt::PointingHandCursor);
    m_navSolanaBalance->setFixedWidth(railItemWidth());
    m_navSolanaBalance->setFont(navBalanceFont());
    m_navSolanaBalance->setStyleSheet(Theme::remapSheet(
        QStringLiteral("font-size:%1px;").arg(navBalanceFont().pixelSize())));
    m_navSolanaBalance->setToolTip(
        "Your Solana wallet balance. Click to check it again.\n"
        "Non-custodial payout address: this client only shares its public "
        "address; its private key stays in the wallet you control.");
    m_navSolanaBalance->installEventFilter(this);
    connect(m_navSolanaBalance, &QLabel::linkActivated, this,
            [this](const QString &) {
                QUrl url = catalogApiUrl();
                url.setPath(QStringLiteral("/dashboard/settings"));
                url.setQuery(QString());
                url.setFragment(QString());
                QDesktopServices::openUrl(url);
            });


    auto *tokenUsage = new TokenUsageMiniChart(
        QStringLiteral("Claude Code usage"), /*remainingMode=*/false,
        /*windows=*/3);
    tokenUsage->setObjectName(QStringLiteral("claudeAccountUsageButton"));
    tokenUsage->setAccessibleName(QStringLiteral("Claude Code accounts and usage"));
    tokenUsage->setToolTip(
        QStringLiteral("Claude Code accounts and usage\nClick to open the Usage "
                       "page. Right-click to manage accounts."));
    m_navTokenUsage = tokenUsage;
    tokenUsage->onHover = [this] {
        refreshClaudeCodeUsage(/*fromHover=*/true);
        refreshClaudeModelCombo();
    };
    tokenUsage->onClick = [this](const QPoint &position) {
        if (!openAgentUsagePage())
            showAgentAccountMenu(QStringLiteral("claude-code"), position);
    };
    tokenUsage->onContextMenu = [this](const QPoint &position) {
        showAgentAccountMenu(QStringLiteral("claude-code"), position);
    };
    tokenUsage->onChanged = [this] { refreshAgentToolbarAccountButtons(); };
    attachAgentUsageHoverCard(tokenUsage, [] { return kClaudeCodeProvider; });
    {
        QSettings settings;
        auto restore = [&](bool weekly, const QString &key) {
            if (settings.contains(key))
                tokenUsage->setUsage(weekly, settings.value(key).toInt());
        };
        restore(false, kClaudeUsage5hPctSetting);
        restore(true, kClaudeUsageWeekPctSetting);
        if (settings.contains(kClaudeUsageFablePctSetting))
            tokenUsage->setUsage(TokenUsageMiniChart::Fable,
                                 settings.value(kClaudeUsageFablePctSetting).toInt());
        auto restoreReset = [&](TokenUsageMiniChart::Window window,
                                const QString &key) {
            if (!settings.contains(key))
                return;
            const qint64 remaining = settings.value(key).toLongLong() -
                                     QDateTime::currentMSecsSinceEpoch();
            if (remaining > 0)
                tokenUsage->setReset(window, humanizeRemaining(remaining));
        };
        restoreReset(TokenUsageMiniChart::FiveHour, kClaudeUsage5hResetSetting);
        restoreReset(TokenUsageMiniChart::Weekly, kClaudeUsageWeekResetSetting);
        restoreReset(TokenUsageMiniChart::Fable, kClaudeUsageFableResetSetting);
    }
    auto *codexUsage = new TokenUsageMiniChart(
        QStringLiteral("Codex usage remaining"), /*remainingMode=*/true,
        /*windows=*/2);
    codexUsage->setObjectName(QStringLiteral("codexAccountUsageButton"));
    codexUsage->setAccessibleName(QStringLiteral("Codex accounts and usage"));
    m_navCodexUsage = codexUsage;
    codexUsage->onHover = [this] {
        refreshCodexUsageRemaining();
        flashUsageChart(m_navCodexUsage, true);
    };
    codexUsage->onClick = [this](const QPoint &position) {
        if (!openAgentUsagePage())
            showAgentAccountMenu(QStringLiteral("codex"), position);
    };
    codexUsage->onContextMenu = [this](const QPoint &position) {
        showAgentAccountMenu(QStringLiteral("codex"), position);
    };
    attachAgentUsageHoverCard(codexUsage, [] { return kCodexProvider; });
    codexUsage->onChanged = [this] { refreshAgentToolbarAccountButtons(); };
    refreshCodexUsageRemaining();
    QTimer::singleShot(0, this, &MainWindow::restoreUsageLimitReminders);

    m_repoMenuButton = new QPushButton;
    m_repoMenuButton->setObjectName("repoMenuButton");
    m_repoMenuButton->setCursor(Qt::PointingHandCursor);
    m_repoMenuButton->setToolTip("Open a repository, or add a local repo to mirror");
    connect(m_repoMenuButton, &QPushButton::clicked, this, &MainWindow::showRepoMenu);

    m_navGroup = new QButtonGroup(this);
    m_navGroup->setExclusive(true);

    m_repoViewButton = new ActivityRailButton(QStringLiteral("code"),
                                              QStringLiteral("Repo"));
    m_repoViewButton->setObjectName("topNavButton");
    m_repoViewButton->setCheckable(true);
    m_repoViewButton->setCursor(Qt::PointingHandCursor);
    m_repoViewButton->setToolTip(QStringLiteral("View the current repository's code"));
    setOcticon(m_repoViewButton, "code", 16);
    m_navGroup->addButton(m_repoViewButton, 0); // section 0: Home / Code
    connect(m_repoViewButton, &QPushButton::clicked, this, [this] {
        showSection(0);
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            if (m_repoCodeTab)
                m_repoCodeTab->setChecked(true);
            if (m_repoDetailStack)
                m_repoDetailStack->setCurrentIndex(0); // Code
            showRepoOverview();
        }
    });

    m_reposNavButton = new ActivityRailButton(QStringLiteral("repo"),
                                              QStringLiteral("Repos"));
    m_reposNavButton->setObjectName("topNavButton");
    m_reposNavButton->setCheckable(true);
    m_reposNavButton->setCursor(Qt::PointingHandCursor);
    m_reposNavButton->setToolTip(QStringLiteral("Browse all repositories on the network"));
    setOcticon(m_reposNavButton, "repo", 16);
    m_navGroup->addButton(m_reposNavButton, kNetworkReposSectionIndex);
    connect(m_reposNavButton, &QPushButton::clicked, this, [this] {
        showSection(kNetworkReposSectionIndex);
    });

    m_marketNavButton = new ActivityRailButton(QStringLiteral("package"),
                                               QStringLiteral("Market"));
    m_marketNavButton->setObjectName(QStringLiteral("marketNavButton"));
    m_marketNavButton->setCheckable(true);
    m_marketNavButton->setCursor(Qt::PointingHandCursor);
    m_marketNavButton->setToolTip(
        QStringLiteral("Market - browse and publish creator packages"));
    setOcticon(m_marketNavButton, "package", 16);
    m_navGroup->addButton(m_marketNavButton, kMarketSectionIndex);
    connect(m_marketNavButton, &QPushButton::clicked, this,
            [this] { showSection(kMarketSectionIndex); });

    m_ledgerNavButton = new ActivityRailButton(QStringLiteral("credit-card"),
                                               QStringLiteral("Ledger"));
    m_ledgerNavButton->setObjectName(QStringLiteral("ledgerNavButton"));
    m_ledgerNavButton->setAccessibleName(QStringLiteral("Open Ledger"));
    m_ledgerNavButton->setCheckable(true);
    m_ledgerNavButton->setCursor(Qt::PointingHandCursor);
    m_ledgerNavButton->setToolTip(
        QStringLiteral("Ledger - public financial records and pending payments"));
    setOcticon(m_ledgerNavButton, "credit-card", 16);
    m_navGroup->addButton(m_ledgerNavButton, kLedgerSectionIndex);
    attachPackageRailMenu(m_ledgerNavButton, QStringLiteral("builtin:ledger"));
    connect(m_ledgerNavButton, &QPushButton::clicked, this,
            &MainWindow::openLedger);

    m_tasksNavButton = new ActivityRailButton(QStringLiteral("list-unordered"),
                                              QStringLiteral("Tasks"));
    m_tasksNavButton->setObjectName("organizationTasksNavButton");
    m_tasksNavButton->setCheckable(true);
    m_tasksNavButton->setCursor(Qt::PointingHandCursor);
    m_tasksNavButton->setToolTip(QStringLiteral("Organization tasks"));
    setOcticon(m_tasksNavButton, "list-unordered", 16);
    m_navGroup->addButton(
        m_tasksNavButton, kOrganizationTasksSectionIndex);
    connect(m_tasksNavButton, &QPushButton::clicked, this, [this] {
        showSection(kOrganizationTasksSectionIndex);
    });

    m_calendarNavButton = new ActivityRailButton(QStringLiteral("calendar"),
                                                 QStringLiteral("Calendar"));
    m_calendarNavButton->setObjectName("calendarNavButton");
    m_calendarNavButton->setCheckable(true);
    m_calendarNavButton->setCursor(Qt::PointingHandCursor);
    m_calendarNavButton->setToolTip(QStringLiteral(
        "Private personal and organization calendars"));
    setOcticon(m_calendarNavButton, "calendar", 16);
    m_navGroup->addButton(m_calendarNavButton, kCalendarSectionIndex);
    connect(m_calendarNavButton, &QPushButton::clicked, this, [this] {
        showSection(kCalendarSectionIndex);
    });

    m_notesNavButton = new ActivityRailButton(QStringLiteral("note"),
                                              QStringLiteral("Notes"));
    m_notesNavButton->setObjectName("notesNavButton");
    m_notesNavButton->setCheckable(true);
    m_notesNavButton->setCursor(Qt::PointingHandCursor);
    m_notesNavButton->setToolTip(QStringLiteral("Local and cloud Markdown notes"));
    setOcticon(m_notesNavButton, "note", 16);
    m_navGroup->addButton(m_notesNavButton, kNotesSectionIndex);
    connect(m_notesNavButton, &QPushButton::clicked, this, [this] {
        showSection(kNotesSectionIndex);
    });

    m_breadcrumb = new QLabel;
    m_breadcrumb->setObjectName("breadcrumb");
    m_breadcrumb->setTextFormat(Qt::RichText);
    m_breadcrumb->setTextInteractionFlags(Qt::TextBrowserInteraction);
    connect(m_breadcrumb, &QLabel::linkActivated, this, [this](const QString &href) {
        if (href == "repos") {
            showSection(0);
        } else if (href == "server") {
            openServerWebsite(m_activeServer);
        }
    });

    auto *notificationRailButton =
        new ActivityRailButton(QStringLiteral("bell"), QStringLiteral("Pings"));
    m_notificationButton = notificationRailButton;
    m_notificationButton->setObjectName("topNavButton");
    m_notificationButton->setToolTip("Pings");
    m_navGroup->addButton(m_notificationButton, 3); // section 3: Notifications
    connect(m_notificationButton, &QPushButton::clicked, this,
            &MainWindow::showNotifications);

    m_userAvatarNavButton = new QPushButton;
    m_userAvatarNavButton->setObjectName("serverFooterButton");
    m_userAvatarNavButton->setCursor(Qt::PointingHandCursor);
    m_userAvatarNavButton->setFixedSize(26, 26);
    m_userAvatarNavButton->setIconSize(QSize(24, 24));
    m_userAvatarNavButton->setToolTip("Collapse the prompt overlay");
    m_userAvatarNavButton->installEventFilter(this);
    connect(m_userAvatarNavButton, &QPushButton::clicked, this, [this] {
        setPromptOverlayCollapsed(!m_promptOverlayCollapsed);
    });
    updateUserSwitcher();
    updateAvatarButton();
    updateAdminCrownBadge();

    m_connectionDot = new QLabel(m_userAvatarNavButton);
    m_connectionDot->setObjectName("connectionDot");
    m_connectionDot->setFixedSize(10, 10);
    m_connectionDot->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_connectionDot->move(26 - 10, 26 - 10);
    m_connectionDot->raise();

    m_adminCrownBadge = new QLabel(m_userAvatarNavButton);
    m_adminCrownBadge->setObjectName("adminCrownBadge");
    m_adminCrownBadge->setFixedSize(12, 12);
    m_adminCrownBadge->setAlignment(Qt::AlignCenter);
    m_adminCrownBadge->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_adminCrownBadge->setPixmap(tintedOcticonPixmap(
        QStringLiteral("crown"), QColor(QStringLiteral("#e3b341")), 12));
    m_adminCrownBadge->move(-2, -2);
    m_adminCrownBadge->raise();
    m_adminCrownBadge->hide();

    auto makeCaption = [](const QString &t) {
        auto *l = new QLabel(t);
        l->setObjectName("navCaption");
        return l;
    };
    m_nodeLabel = makeCaption(QStringLiteral("Node"));
    m_repoLabel = makeCaption(QStringLiteral("Repo"));

    m_agentsNavButton = new ActivityRailButton(QStringLiteral("smart-toy"),
                                               QStringLiteral("Agents"));
    m_agentsNavButton->setObjectName("topNavButton");
    m_agentsNavButton->setCheckable(true);
    m_agentsNavButton->setCursor(Qt::PointingHandCursor);
    m_agentsNavButton->setToolTip(QStringLiteral("Agents"));
    setOcticon(m_agentsNavButton, "smart-toy", 16);
    connect(m_agentsNavButton, &QPushButton::clicked, this,
            &MainWindow::openAgentsOverview);

    {
        const QStringList hidden =
            QSettings().value(kHiddenTopBarIndicatorsSetting).toStringList();
        m_hiddenTopBarIndicators = QSet<QString>(hidden.cbegin(), hidden.cend());
    }
    m_agentDotMatrix = new AgentDotMatrix;
    m_agentDotMatrix->onDotClicked = [this](int sessionId) {
        if (sessionId > 0)
            switchToAgentsTab(sessionId);
        else
            openAgentsOverview();
    };

    auto makeChromeDotDivider = [] {
        auto *divider = new QWidget;
        divider->setObjectName(QStringLiteral("chromeDotDivider"));
        divider->setAttribute(Qt::WA_StyledBackground, true);
        divider->setFixedWidth(1);
        divider->setFixedHeight(ChromeDotGrid::totalHeight());
        divider->hide();
        return divider;
    };
    m_chromeDotDivider = makeChromeDotDivider();
    m_nodeDotMatrix = new NodeDotMatrix;
    m_nodeDotMatrix->onDotClicked = [this](const QString &) {
        showNetworkTab(kNetworkNodesTab);
    };

    m_chromeActionDivider = makeChromeDotDivider();
    m_actionRunStrip = new ActionRunStrip;
    m_actionRunStrip->onCellClicked = [this](int runId) {
        if (runId <= 0 && !m_actionRuns.isEmpty())
            runId = m_actionRuns.first().id;
        if (runId > 0)
            openActionRunFromNotification(runId);
    };
    refreshActionRunStrip();

    auto *chatRailButton = new ActivityRailButton(QStringLiteral("comment"),
                                                  QStringLiteral("Chat"));
    m_chatButton = chatRailButton;
    m_chatButton->setObjectName("topNavButton");
    m_chatButton->setToolTip(QStringLiteral("Chat"));
    m_navGroup->addButton(m_chatButton, 2); // section 2: Chat
    connect(m_chatButton, &QPushButton::clicked, this, &MainWindow::showChatView);

    m_settingsNavButton = new ActivityRailButton(QStringLiteral("gear"),
                                                 QStringLiteral("Settings"));
    m_settingsNavButton->setObjectName("topNavButton");
    m_settingsNavButton->setToolTip(QStringLiteral("Settings"));
    m_navGroup->addButton(m_settingsNavButton, 1); // section 1: Settings
    connect(m_settingsNavButton, &QPushButton::clicked, this,
            [this] { showSection(1); });

    m_tourNavButton = new ActivityRailButton(QStringLiteral("rocket"),
                                              QStringLiteral("Tour"));
    m_tourNavButton->setObjectName(QStringLiteral("tourNavButton"));
    m_tourNavButton->setCheckable(false);
    m_tourNavButton->setCursor(Qt::PointingHandCursor);
    m_tourNavButton->setAccessibleName(
        QStringLiteral("Take the ForkMesh feature tour"));
    m_tourNavButton->setToolTip(
        QStringLiteral("Take the guided tour of every ForkMesh workspace"));
    setOcticon(m_tourNavButton, QStringLiteral("rocket"), 16);
    connect(m_tourNavButton, &QPushButton::clicked, this,
            &MainWindow::startFeatureTour);
    attachPackageRailMenu(m_tourNavButton, QStringLiteral("builtin:tour"));

    m_logNavButton = new ActivityRailButton(QStringLiteral("list-unordered"),
                                            QStringLiteral("Log"));
    m_logNavButton->setObjectName("topNavButton");
    m_logNavButton->setCheckable(true);
    m_logNavButton->setCursor(Qt::PointingHandCursor);
    m_logNavButton->setToolTip(QStringLiteral("Network and application log"));
    setOcticon(m_logNavButton, "list-unordered", 16);
    m_navGroup->addButton(m_logNavButton, 4);
    connect(m_logNavButton, &QPushButton::clicked, this,
            [this] { showSection(4); });

    // Control node: this desktop's operational surface for local mirrors,
    // permissions, keys, wallet public address, Cloudflare, and connected hosts.
    m_controlNodeNavButton = new ActivityRailButton(QStringLiteral("server"),
                                                    QStringLiteral("Control"));
    m_controlNodeNavButton->setObjectName("topNavButton");
    m_controlNodeNavButton->setCheckable(true);
    m_controlNodeNavButton->setCursor(Qt::PointingHandCursor);
    m_controlNodeNavButton->setToolTip(
        QStringLiteral("Control node - mirrors, health, keys, Cloudflare and hosts"));
    setOcticon(m_controlNodeNavButton, "server", 16);
    m_navGroup->addButton(m_controlNodeNavButton, kControlNodeSectionIndex);
    connect(m_controlNodeNavButton, &QPushButton::clicked, this,
            [this] { showSection(kControlNodeSectionIndex); });

    // Network: Relays, Nodes and Hosts as tabs alongside the
    // websocket / Durable Object diagnostics and the outbound firewall. The
    // three used to be rail buttons of their own (sections 7, 8 and 13); those
    // section indexes still resolve, they just land on the matching tab.
    m_networkNavButton = new ActivityRailButton(QStringLiteral("workflow"),
                                                QStringLiteral("Network"));
    m_networkNavButton->setObjectName("topNavButton");
    m_networkNavButton->setCheckable(true);
    m_networkNavButton->setCursor(Qt::PointingHandCursor);
    m_networkNavButton->setToolTip(
        QString::fromUtf8("Network \xE2\x80\x94 relays, nodes, hosts, websocket "
                          "and Durable Object diagnostics"));
    setOcticon(m_networkNavButton, "workflow", 16);
    m_navGroup->addButton(m_networkNavButton, kNetworkDiagnosticsSectionIndex);
    connect(m_networkNavButton, &QPushButton::clicked, this,
            [this] { showSection(kNetworkDiagnosticsSectionIndex); });

    m_usersNavButton = new ActivityRailButton(QStringLiteral("people"),
                                              QStringLiteral("Users"));
    m_usersNavButton->setObjectName(QStringLiteral("usersNavButton"));
    m_usersNavButton->setCheckable(true);
    m_usersNavButton->setCursor(Qt::PointingHandCursor);
    m_usersNavButton->setToolTip(
        QStringLiteral("Users - World profile and activity statistics"));
    setOcticon(m_usersNavButton, "people", 16);
    m_navGroup->addButton(m_usersNavButton, kUsersSectionIndex);
    connect(m_usersNavButton, &QPushButton::clicked, this,
            [this] { showSection(kUsersSectionIndex); });
    m_usersNavButton->setVisible(m_isAdmin);

    // The team lives directly under Users: both answer "who works here", one
    // for the people on the mesh and one for the AI teammates you run.
    m_teamNavButton = new ActivityRailButton(QStringLiteral("organization"),
                                             QStringLiteral("Specials"));
    m_teamNavButton->setObjectName(QStringLiteral("teamNavButton"));
    m_teamNavButton->setCheckable(true);
    m_teamNavButton->setCursor(Qt::PointingHandCursor);
    m_teamNavButton->setToolTip(
        QString::fromUtf8("Special Agents \xE2\x80\x94 your AI agents, the repository "
                          "each one works on, and what they shipped last"));
    setOcticon(m_teamNavButton, "organization", 16);
    m_navGroup->addButton(m_teamNavButton, kTeamSectionIndex);
    connect(m_teamNavButton, &QPushButton::clicked, this,
            [this] {
                const bool alreadyOnTeam =
                    m_sectionStack &&
                    m_sectionStack->currentIndex() == kTeamSectionIndex;
                showTeammateDashboard();
                if (!alreadyOnTeam)
                    showSection(kTeamSectionIndex);
            });

    m_filesNavButton = new ActivityRailButton(QStringLiteral("file-directory"),
                                              QStringLiteral("Files"));
    m_filesNavButton->setObjectName(QStringLiteral("topNavButton"));
    m_filesNavButton->setCheckable(true);
    m_filesNavButton->setCursor(Qt::PointingHandCursor);
    m_filesNavButton->setToolTip(
        QStringLiteral("Files - browse this machine's filesystem, hidden files "
                       "included; always opens at the repository root"));
    setOcticon(m_filesNavButton, "file-directory", 16);
    m_navGroup->addButton(m_filesNavButton, kFilesSectionIndex);
    connect(m_filesNavButton, &QPushButton::clicked, this,
            [this] { openFilesSectionAtRepoRoot(); });

    // Stats sits under Files: every number the app already keeps (repos,
    // issues, agents, tool efficiency, host load, logs) as one BI-style board.
    m_statsNavButton = new ActivityRailButton(QStringLiteral("graph"),
                                              QStringLiteral("Stats"));
    m_statsNavButton->setObjectName(QStringLiteral("topNavButton"));
    m_statsNavButton->setCheckable(true);
    m_statsNavButton->setCursor(Qt::PointingHandCursor);
    m_statsNavButton->setToolTip(QString::fromUtf8(
        "Stats \xE2\x80\x94 big numbers and charts for repositories, issues, "
        "agents, actions, the network and this machine; add, remove, drag "
        "and resize the tiles"));
    setOcticon(m_statsNavButton, "graph", 16);
    m_navGroup->addButton(m_statsNavButton, kStatsSectionIndex);
    connect(m_statsNavButton, &QPushButton::clicked, this,
            [this] { showSection(kStatsSectionIndex); });

    m_componentHealthNavButton =
        new ActivityRailButton(QStringLiteral("stopwatch"),
                               QStringLiteral("Health"));
    m_componentHealthNavButton->setObjectName("topNavButton");
    m_componentHealthNavButton->setCheckable(true);
    m_componentHealthNavButton->setCursor(Qt::PointingHandCursor);
    m_componentHealthNavButton->setToolTip(QString::fromUtf8(
        "Component health \xE2\x80\x94 one tiny card per source file: its size, "
        "the UI stall time attributed to it, and the patterns in it that have "
        "frozen this app before; click a card for that file's functions"));
    setOcticon(m_componentHealthNavButton, "stopwatch", 16);
    m_navGroup->addButton(m_componentHealthNavButton,
                          kComponentHealthSectionIndex);
    connect(m_componentHealthNavButton, &QPushButton::clicked, this,
            [this] { showSection(kComponentHealthSectionIndex); });

    m_memoryNavButton = new ActivityRailButton(QStringLiteral("database"),
                                               QStringLiteral("Memory"));
    m_memoryNavButton->setObjectName("topNavButton");
    m_memoryNavButton->setCheckable(true);
    m_memoryNavButton->setCursor(Qt::PointingHandCursor);
    m_memoryNavButton->setToolTip(QString::fromUtf8(
        "Memory \xE2\x80\x94 everything this app holds in RAM, live: every "
        "mapping grouped by what it is, the allocator's in-use and freed-but-"
        "held bytes, and the app's own caches, with a chart of the last few "
        "minutes"));
    setOcticon(m_memoryNavButton, "database", 16);
    m_navGroup->addButton(m_memoryNavButton, kMemorySectionIndex);
    connect(m_memoryNavButton, &QPushButton::clicked, this,
            [this] { showSection(kMemorySectionIndex); });

    m_homeGridNavButton = new ActivityRailButton(QStringLiteral("home"),
                                                 QStringLiteral("Home"));
    m_homeGridNavButton->setObjectName("topNavButton");
    m_homeGridNavButton->setCheckable(true);
    m_homeGridNavButton->setCursor(Qt::PointingHandCursor);
    m_homeGridNavButton->setToolTip(QString::fromUtf8(
        "Home \xE2\x80\x94 every page in the rail as one grid of icons, "
        "grouped, each with its live count and what it is doing right now"));
    setOcticon(m_homeGridNavButton, "home", 16);
    m_navGroup->addButton(m_homeGridNavButton, kHomeGridSectionIndex);
    connect(m_homeGridNavButton, &QPushButton::clicked, this,
            [this] { showSection(kHomeGridSectionIndex); });

    auto *rebuildButton = new ActivityRailButton(QStringLiteral("sync"),
                                                 QStringLiteral("Restart"));
    m_navRebuildButton = rebuildButton;
    rebuildButton->setCheckable(false);
    rebuildButton->setObjectName("topNavButton");
    rebuildButton->setCursor(Qt::PointingHandCursor);
    rebuildButton->setIconSize(QSize(kRailIconPx, kRailIconPx));
    rebuildButton->setToolTip(
        QString::fromUtf8("Rebuild & restart \xE2\x80\x94 fast local rebuild, "
                          "then relaunch; click again to cancel"));
    connect(rebuildButton, &QPushButton::clicked, this, [this] {
        if (cancelRestartIfRunning())
            return;
        startRestartSpin(m_navRebuildButton);
        quickRebuildRestart();
    });

    m_navSignInButton = new QPushButton(QStringLiteral("Log in / Sign up"));
    m_navSignInButton->setObjectName("primaryButton");
    m_navSignInButton->setCursor(Qt::PointingHandCursor);
    m_navSignInButton->setToolTip(
        QStringLiteral("Attach this machine to your ForkMesh account, or create "
                       "one on forkmesh.com"));
    setOcticon(m_navSignInButton, "sign-in", 14);
    m_navSignInButton->hide();
    connect(m_navSignInButton, &QPushButton::clicked, this,
            &MainWindow::showSignInMenu);

    m_accountSwitcherButton = new QPushButton;
    m_accountSwitcherButton->setObjectName(
        QStringLiteral("accountSwitcherButton"));
    m_accountSwitcherButton->setCursor(Qt::PointingHandCursor);
    m_accountSwitcherButton->setFixedSize(32, 32);
    m_accountSwitcherButton->setIconSize(QSize(28, 28));
    m_accountSwitcherButton->setToolTip(
        QStringLiteral("Account menu and user switcher"));
    m_accountSwitcherButton->setAccessibleName(
        QStringLiteral("Account menu and user switcher"));
    m_accountSwitcherButton->hide();
    connect(m_accountSwitcherButton, &QPushButton::clicked, this,
            &MainWindow::showUserAccountMenu);

    m_navScreenshotButton = new ActivityRailButton(QStringLiteral("screen-full"),
                                                   QStringLiteral("Capture"));
    m_navScreenshotButton->setCheckable(false);
    m_navScreenshotButton->setObjectName("topNavButton");
    m_navScreenshotButton->setToolTip(
        QString::fromUtf8("Screenshot a region \xE2\x80\x94 drag a square anywhere on "
                          "screen and it's attached to your prompt"));
    connect(m_navScreenshotButton, &QPushButton::clicked, this,
            &MainWindow::captureScreenRegion);
    attachPackageRailMenu(m_navScreenshotButton,
                          QStringLiteral("builtin:capture"));
    // Built before syncPackageRuntime() so an installed Timers package can
    // attach its strip on the first pass rather than after the first tick.
    createTimersNavStrip();
    syncPackageRuntime();

    m_navResizeButton = new ActivityRailButton(QStringLiteral("device-desktop"),
                                               QStringLiteral("Resize"));
    m_navResizeButton->setCheckable(false);
    m_navResizeButton->setObjectName("topNavButton");
    m_navResizeButton->setToolTip(
        QString::fromUtf8("Resize to 1280\xC3\x97" "720 \xE2\x80\x94 a common "
                          "minimal screen size, handy for previewing smaller "
                          "displays"));
    connect(m_navResizeButton, &QPushButton::clicked, this, [this] {
        if (isMaximized())
            showNormal();
        resize(1280, 720);
    });

    auto *resourceChart = new ResourceQuadrantSparkline;
    resourceChart->setClickHandler(ResourceQuadrantSparkline::Cpu,
                                   [this] { showDesktopResourceDiagnostics(); });
    resourceChart->setClickHandler(ResourceQuadrantSparkline::Swap,
                                   [this] { showDiagnosticsDialog(); });
    resourceChart->setClickHandler(ResourceQuadrantSparkline::Memory,
                                   [this] { showHighMemoryProcessPanel(); });
    resourceChart->setClickHandler(ResourceQuadrantSparkline::Disk, [this] {
        if (!packageSurfaceShown(QStringLiteral("size-map"))) {
            showDesktopResourceDiagnostics();
            return;
        }
        showSection(0);
        if (m_repoDetailTabs && m_sizeMapTabIndex >= 0) {
            if (QAbstractButton *sizeMapTab =
                    m_repoDetailTabs->button(m_sizeMapTabIndex))
                sizeMapTab->click();
        }
    });
    m_resourceChart = resourceChart;


    auto *layout = new QVBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *chrome = new WindowChromeBar;
    chrome->onContextMenu = [this](const QPoint &globalPos) {
        showTopBarIndicatorMenu(globalPos);
    };
    auto *chromeRow = new QHBoxLayout(chrome);
    chromeRow->setContentsMargins(0, 0, 8, 0);
    chromeRow->setSpacing(8);
    chromeRow->addWidget(buildProjectMenuButton());
    chromeRow->addWidget(forkmesh::ui::NextAccess::railToggleButton(*this));
    chromeRow->addWidget(m_relayJoinApproveButton);
    chromeRow->addWidget(m_runnerAttentionButton);
    auto *projectContextStash = new QWidget(bar);
    projectContextStash->hide();
    m_relayMenuButton->setParent(projectContextStash);
    m_accountSwitcherButton->setParent(projectContextStash);
    m_navSignInButton->setParent(projectContextStash);
    m_repoMenuButton->setParent(projectContextStash);
    auto *identityBalanceRow = new QHBoxLayout;
    identityBalanceRow->setContentsMargins(0, 0, 0, 0);
    identityBalanceRow->setSpacing(8);
    identityBalanceRow->addWidget(m_agentDotMatrix);
    identityBalanceRow->addWidget(m_chromeDotDivider, 0, Qt::AlignVCenter);
    identityBalanceRow->addWidget(m_nodeDotMatrix);
    identityBalanceRow->addWidget(m_chromeActionDivider, 0, Qt::AlignVCenter);
    identityBalanceRow->addWidget(m_actionRunStrip);
    // The Timers package's clock and its chips, beside the agent/node/action
    // indicators the top bar already carries. Hidden unless it is installed.
    if (m_timersNavStrip)
        identityBalanceRow->addWidget(m_timersNavStrip, 0, Qt::AlignVCenter);
    chromeRow->addLayout(identityBalanceRow);
    chromeRow->addStretch();

    auto *searchCluster = new QWidget;
    auto *searchClusterRow = new QHBoxLayout(searchCluster);
    searchClusterRow->setContentsMargins(0, 0, 0, 0);
    searchClusterRow->setSpacing(8);
    searchClusterRow->addWidget(createNavHistoryButtons());
    searchClusterRow->addWidget(createGlobalSearchBox());
    chromeRow->addWidget(searchCluster, 0, Qt::AlignCenter);
    chromeRow->addStretch();

    auto makeWindowButton = [this](QStyle::StandardPixmap icon, const QString &tip) {
        auto *button = new QPushButton;
        button->setObjectName(QStringLiteral("windowChromeButton"));
        button->setCursor(Qt::PointingHandCursor);
        button->setFixedSize(32, 30);
        button->setToolTip(tip);
        button->setIcon(style()->standardIcon(icon));
        button->setIconSize(QSize(14, 14));
        return button;
    };
    auto *minimizeButton = makeWindowButton(QStyle::SP_TitleBarMinButton,
                                            QStringLiteral("Minimize"));
    connect(minimizeButton, &QPushButton::clicked, this, &MainWindow::showMinimized);
    auto *maximizeButton = makeWindowButton(QStyle::SP_TitleBarMaxButton,
                                            QStringLiteral("Maximize / restore"));
    connect(maximizeButton, &QPushButton::clicked, this, [this] {
        if (isMaximized())
            showNormal();
        else
            showMaximized();
    });
    auto *closeButton = makeWindowButton(QStyle::SP_TitleBarCloseButton,
                                         QStringLiteral("Close"));
    closeButton->setObjectName(QStringLiteral("windowChromeCloseButton"));
    connect(closeButton, &QPushButton::clicked, this, &MainWindow::close);
    m_packageActionsButton = createPackageActionsButton(
        [this] { return currentNativePackageId(); }, chrome);
    m_packageActionsButton->hide();
    chromeRow->addWidget(m_packageActionsButton);
    chromeRow->addWidget(new BackgroundRunnerBadge([this] {
        showSection(1);
        if (!m_settingsSections)
            return;
        if (auto *box = m_settingsSections->findChild<QWidget *>(
                QStringLiteral("backgroundSettings")))
            m_settingsSections->revealWidget(box);
    }), 0, Qt::AlignVCenter);
    // The view switcher sits just before the window controls, so it stays in
    // the top-right corner whichever view hides the rail or the status strip.
    chromeRow->addWidget(buildViewModeButton());
    chromeRow->addWidget(minimizeButton);
    chromeRow->addWidget(maximizeButton);
    chromeRow->addWidget(closeButton);
    chrome->setMinimumWidth(0);
    chrome->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *chromeScroll = new QScrollArea;
    chromeScroll->setObjectName(QStringLiteral("topChromeScroll"));
    chromeScroll->setWidget(chrome);
    chromeScroll->setWidgetResizable(true);
    chromeScroll->setFrameShape(QFrame::NoFrame);
    chromeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    chromeScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chromeScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    chromeScroll->setMinimumWidth(0);
    chromeScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const int chromeHeight = chrome->height();
    QScrollBar *chromeHBar = chromeScroll->horizontalScrollBar();
    auto syncChromeScrollHeight = [chromeScroll, chromeHBar, chromeHeight] {
        const bool scrollable = chromeHBar->maximum() > chromeHBar->minimum();
        chromeScroll->setFixedHeight(
            chromeHeight + (scrollable ? chromeHBar->sizeHint().height() : 0));
    };
    connect(chromeHBar, &QScrollBar::rangeChanged, chromeScroll,
            [syncChromeScrollHeight](int, int) { syncChromeScrollHeight(); });
    syncChromeScrollHeight();
    layout->addWidget(chromeScroll);

    m_nodeLabel->setParent(bar);
    m_nodeLabel->hide();
    m_nodeMenuButton->setParent(bar);
    m_nodeMenuButton->hide();
    m_repoLabel->setParent(bar);
    m_repoLabel->hide();
    m_breadcrumb->setParent(bar);
    m_breadcrumb->hide();

    m_repoViewButton->setChecked(true);

    updateBreadcrumb();
    updateConnectionStatus();
    updateNotificationButton();
    updateChatButton();
    updateNavSolanaBalance();
    refreshRepoSyncIndicators();
    updateNavRebuildButton();
    updateNodeOnlineControls();
    updateSignInButton();
    return bar;
}

void MainWindow::updateNavRebuildButton()
{
    const bool visible =
        QSettings().value(kShowRebuildButtonSetting, false).toBool();
    if (m_navRebuildButton)
        m_navRebuildButton->setVisible(visible);
}

// Settings -> Appearance -> Background: a colour or picture veiled over the
// whole window at the chosen strength. Runs at startup and on every change of
// the mode, colour, image or strength, so the slider previews in real time.
void MainWindow::applyWindowBackdrop()
{
    using forkmesh::ui::WindowBackdrop;
    if (!m_windowBackdrop)
        m_windowBackdrop = new WindowBackdrop(this);
    QSettings settings;
    const QString mode =
        settings.value(kWindowBackdropModeSetting, QStringLiteral("none"))
            .toString();
    const QColor color(settings
                           .value(kWindowBackdropColorSetting,
                                  kDefaultWindowBackdropColor)
                           .toString());
    const QString imagePath =
        settings.value(kWindowBackdropImageSetting).toString();
    WindowBackdrop::Mode backdropMode = WindowBackdrop::Mode::None;
    if (mode == QLatin1String("color"))
        backdropMode = WindowBackdrop::Mode::Color;
    else if (mode == QLatin1String("image"))
        backdropMode = WindowBackdrop::Mode::Image;
    // The picture is only decoded while the image mode is chosen.
    m_windowBackdrop->setBackdrop(
        backdropMode, color,
        backdropMode == WindowBackdrop::Mode::Image ? imagePath : QString(),
        windowBackdropStrengthPercent());
    if (m_windowFade && m_windowFade->isVisible())
        m_windowFade->raise();
}

void MainWindow::applyWindowOpacity(int percent)
{
    if (m_windowFade)
        m_windowFade->setOpacityPercent(percent);
    else
        setWindowOpacity(static_cast<qreal>(percent) / 100.0);
}

// Settings -> Appearance can drop the caption under each left-rail icon, which
// also narrows the rail and shortens every item in it. Only buttons that live
// in the rail follow the preference, so this runs wherever one joins the rail
// as well as when the preference itself changes; it is idempotent and cheap
// enough to call on every rail rebuild.
void MainWindow::applyRailCaptionPreference()
{
    const bool hidden =
        QSettings().value(kHideRailCaptionsSetting, false).toBool();
    const bool changed = hidden != railCaptionsHidden();
    setRailCaptionsHidden(hidden);

    if (m_appNavigationRailLayout) {
        for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
            QLayoutItem *item = m_appNavigationRailLayout->itemAt(i);
            // ActivityRailButton has no Q_OBJECT of its own, so qobject_cast
            // would match every QPushButton in the rail.
            if (auto *button = dynamic_cast<ActivityRailButton *>(
                    item ? item->widget() : nullptr))
                button->followRailCaptionPreference();
            else if (auto *tile = dynamic_cast<VerticalIconButton *>(
                         item ? item->widget() : nullptr))
                tile->setFixedWidth(railColumnWidth());
        }
        for (const QPointer<RailFolderWidget> &folder :
             std::as_const(m_railFolderWidgets))
            if (folder)
                folder->followRailCaptionPreference();
        for (const QPointer<RailGroupSeparator> &separator :
             std::as_const(m_railGroupSeparators))
            if (separator)
                separator->followRailCaptionPreference();
        if (m_railSortButton) {
            m_railSortButton->setFixedWidth(railColumnWidth());
            refreshRailSortButton();
        }
        if (QWidget *content = m_appNavigationRailLayout->parentWidget())
            content->setMinimumWidth(railColumnWidth());
    }
    if (m_appNavigationRail)
        m_appNavigationRail->setFixedWidth(railWidth());
    if (m_navSolanaBalance)
        m_navSolanaBalance->setFixedWidth(railItemWidth());
    if (changed && m_relayMenuButton)
        updateRelaySwitcher(); // re-floors the instance button at the new width
}

QList<QPair<QString, QWidget *>> MainWindow::railHomeButtons() const
{
    QList<QPair<QString, QWidget *>> canonical = {
        {QStringLiteral("home"), m_homeGridNavButton},
        {QStringLiteral("code"), m_railCodeButton},
        {QStringLiteral("agents"), m_agentsNavButton},
        {QStringLiteral("git"), m_railGitButton},
        {QStringLiteral("repos"), m_reposNavButton},
        {QStringLiteral("market"), m_marketNavButton},
        {QStringLiteral("ledger"), m_ledgerNavButton},
    };
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        canonical.append({QStringLiteral("package:%1").arg(page.id),
                          page.navigation.data()});
    canonical += {
        {QStringLiteral("chat"), m_chatButton},
        {QStringLiteral("controlNode"), m_controlNodeNavButton},
        {QStringLiteral("network"), m_networkNavButton},
        {QStringLiteral("users"), m_usersNavButton},
        {QStringLiteral("team"), m_teamNavButton},
        {QStringLiteral("files"), m_filesNavButton},
        {QStringLiteral("stats"), m_statsNavButton},
        {QStringLiteral("componentHealth"), m_componentHealthNavButton},
        {QStringLiteral("memory"), m_memoryNavButton},
        {QStringLiteral("tour"), m_tourNavButton},
        {QStringLiteral("settings"), m_settingsNavButton},
        {QStringLiteral("screenshot"), m_navScreenshotButton},
        {QStringLiteral("notes"), m_notesNavButton},
        {QStringLiteral("calendar"), m_calendarNavButton},
        {QStringLiteral("tasks"), m_tasksNavButton},
        {QStringLiteral("notifications"), m_notificationButton},
    };
    return canonical;
}

namespace {

constexpr int kRailPopOutSpinLeadMs = 60;
constexpr int kRailPopOutSpinLimitMs = 8000;
constexpr int kRailDropCatcherReturnMargin = 8;

bool isWaylandPlatform()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"));
}

bool catchesRailDropsDuringDrag()
{
    return isWaylandPlatform() ||
           QGuiApplication::platformName() == QLatin1String("cocoa");
}

Qt::WindowFlags railDropCatcherFlags()
{
    Qt::WindowFlags flags = Qt::Tool | Qt::FramelessWindowHint |
                            Qt::WindowStaysOnTopHint |
                            Qt::NoDropShadowWindowHint;
    if (QGuiApplication::platformName() == QLatin1String("xcb"))
        flags |= Qt::X11BypassWindowManagerHint;
    return flags;
}

class RailDropCatcher : public QWidget
{
public:
    RailDropCatcher(QWidget *owner, QScreen *screen, const QPixmap &pixmap,
                    const QPoint &hotSpot)
        : QWidget(owner, railDropCatcherFlags()), m_screen(screen),
          m_pixmap(pixmap), m_hotSpot(hotSpot)
    {
        setObjectName(QStringLiteral("railDropCatcher"));
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAcceptDrops(true);
        m_spin.setInterval(16);
        connect(&m_spin, &QTimer::timeout, this, [this] {
            m_angle = std::fmod(m_angle + 12.0, 360.0);
            update();
        });
    }

    void place()
    {
        if (m_screen)
            setGeometry(m_screen->geometry());
    }

    void cover()
    {
        m_calibrated = !isWaylandPlatform();
        if (isVisible() || !m_screen)
            return;
        place();
        if (isWaylandPlatform())
            showMaximized();
        else
            show();
    }

    bool calibrated() const { return m_calibrated; }

    void calibrate(const QPoint &offset)
    {
        m_offset = offset;
        m_calibrated = true;
    }

    QPoint toGlobal(const QPoint &at) const
    {
        return isWaylandPlatform() ? at + m_offset : mapToGlobal(at);
    }

    void spinAt(const QPoint &at)
    {
        const QSizeF size = m_pixmap.deviceIndependentSize();
        m_center = QPointF(at - m_hotSpot) +
                   QPointF(size.width(), size.height()) / 2.0;
        const int reach =
            qCeil(std::hypot(size.width(), size.height()) / 2.0) + 1;
        setMask(QRegion(QRect(m_center.toPoint() - QPoint(reach, reach),
                              QSize(2 * reach, 2 * reach))));
        m_spinning = true;
        m_spin.start();
        QTimer::singleShot(kRailPopOutSpinLimitMs, this, &QObject::deleteLater);
        if (!isVisible())
            show();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (!m_spinning) {
            painter.fillRect(rect(), QColor(0, 0, 0, 1));
            return;
        }
        painter.setRenderHints(QPainter::Antialiasing |
                               QPainter::SmoothPixmapTransform);
        painter.translate(m_center);
        painter.rotate(m_angle);
        const QSizeF size = m_pixmap.deviceIndependentSize();
        painter.drawPixmap(QPointF(-size.width() / 2.0, -size.height() / 2.0),
                           m_pixmap);
    }

private:
    QPointer<QScreen> m_screen;
    QPixmap m_pixmap;
    QPoint m_hotSpot;
    QPoint m_offset;
    bool m_calibrated = false;
    bool m_spinning = false;
    QPointF m_center;
    qreal m_angle = 0;
    QTimer m_spin;
};

void stopRailPopOutSpinnerWhenShown(QWidget *spinner, QWidget *window)
{
    QWindow *handle =
        window && window->isVisible() ? window->windowHandle() : nullptr;
    if (!handle || handle->isExposed()) {
        spinner->deleteLater();
        return;
    }
    QPointer<QWidget> guarded(spinner);
    handle->installEventFilter(new RailEventHook(
        [guarded, handle](QObject *, QEvent *event) {
            if (event->type() == QEvent::Expose && handle->isExposed() &&
                guarded)
                guarded->deleteLater();
            return false;
        },
        spinner));
}

class RailDragOutWatch : public QObject
{
public:
    explicit RailDragOutWatch(const QList<QWidget *> &windows,
                              const QString &mimeType = kRailItemMimeType,
                              const QDrag *spinDrag = nullptr)
        : m_mimeType(mimeType)
    {
        for (QWidget *window : windows) {
            if (!window || m_windows.contains(window))
                continue;
            m_windows.append(window);
            m_acceptedDrops.append(window->acceptDrops());
            window->setAcceptDrops(true);
        }
        if (spinDrag && !m_windows.isEmpty()) {
            m_pixmap = spinDrag->pixmap();
            m_hotSpot = spinDrag->hotSpot();
        }
        qApp->installEventFilter(this);
    }

    ~RailDragOutWatch() override
    {
        qApp->removeEventFilter(this);
        for (const QPointer<RailDropCatcher> &catcher : std::as_const(m_catchers))
            delete catcher.data();
        for (int i = 0; i < m_windows.size(); ++i) {
            if (QWidget *window = m_windows.at(i))
                window->setAcceptDrops(m_acceptedDrops.at(i));
        }
    }

    bool droppedOutside() const { return m_outside && !m_dropped; }

    QWidget *takeSpinner()
    {
        QWidget *owner = m_windows.isEmpty() ? nullptr : m_windows.first().data();
        if (!droppedOutside() || m_pixmap.isNull() || !owner)
            return nullptr;
        RailDropCatcher *spinner = m_caught;
        QPoint at = m_caughtAt;
        if (spinner) {
            m_catchers.removeAll(spinner);
        } else {
            if (isWaylandPlatform())
                return nullptr;
            QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
            if (!screen)
                return nullptr;
            spinner = new RailDropCatcher(owner, screen, m_pixmap, m_hotSpot);
            spinner->place();
            at = spinner->mapFromGlobal(QCursor::pos());
        }
        spinner->spinAt(at);
        return spinner;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (type != QEvent::DragEnter && type != QEvent::DragMove &&
            type != QEvent::DragLeave && type != QEvent::Drop)
            return false;
        for (const QPointer<RailDropCatcher> &catcher : std::as_const(m_catchers)) {
            if (catcher && watched == catcher->windowHandle())
                return false;
            if (catcher && watched == catcher.data())
                return catcherEvent(catcher, event);
        }
        if (watched->isWindowType()) {
            if (type == QEvent::DragLeave) {
                m_outside = true;
                if (!m_pixmap.isNull() && catchesRailDropsDuringDrag())
                    QTimer::singleShot(0, this, [this] {
                        if (m_outside)
                            coverOutside();
                    });
            } else if (type == QEvent::Drop) {
                m_dropped = true;
            } else {
                m_outside = false;
                m_lastWindow = static_cast<QWindow *>(watched);
                m_lastPos = static_cast<QDropEvent *>(event)->position().toPoint();
            }
            return false;
        }
        auto *widget = qobject_cast<QWidget *>(watched);
        if (type == QEvent::DragLeave || !widget || !m_windows.contains(widget))
            return false;
        auto *drop = static_cast<QDropEvent *>(event);
        if (!drop->mimeData() || !drop->mimeData()->hasFormat(m_mimeType))
            return false;
        drop->setDropAction(Qt::MoveAction);
        drop->accept();
        return true;
    }

private:
    void coverOutside()
    {
        if (m_catchers.isEmpty()) {
            QList<QScreen *> screens = QGuiApplication::screens();
            if (isWaylandPlatform())
                screens = {m_windows.first() ? m_windows.first()->screen()
                                             : QGuiApplication::primaryScreen()};
            for (QScreen *screen : std::as_const(screens)) {
                if (screen && m_windows.first())
                    m_catchers.append(new RailDropCatcher(
                        m_windows.first(), screen, m_pixmap, m_hotSpot));
            }
        }
        for (const QPointer<RailDropCatcher> &catcher : std::as_const(m_catchers)) {
            if (catcher)
                catcher->cover();
        }
    }

    QPoint exitPoint() const
    {
        if (!m_lastWindow)
            return m_lastPos;
        const QSize size = m_lastWindow->size();
        const int left = m_lastPos.x();
        const int right = size.width() - 1 - m_lastPos.x();
        const int top = m_lastPos.y();
        const int bottom = size.height() - 1 - m_lastPos.y();
        const int nearest = std::min({left, right, top, bottom});
        QPoint out = m_lastPos;
        if (nearest == left)
            out.setX(-1);
        else if (nearest == right)
            out.setX(size.width());
        else if (nearest == top)
            out.setY(-1);
        else
            out.setY(size.height());
        return m_lastWindow->mapToGlobal(out);
    }

    bool overTrackedWindow(const QPoint &global) const
    {
        for (const QPointer<QWidget> &window : m_windows) {
            QWindow *handle = window ? window->windowHandle() : nullptr;
            if (handle && handle->isVisible() &&
                QRect(QPoint(), handle->size())
                    .adjusted(kRailDropCatcherReturnMargin,
                              kRailDropCatcherReturnMargin,
                              -kRailDropCatcherReturnMargin,
                              -kRailDropCatcherReturnMargin)
                    .contains(handle->mapFromGlobal(global)))
                return true;
        }
        return false;
    }

    bool catcherEvent(RailDropCatcher *catcher, QEvent *event)
    {
        if (event->type() == QEvent::DragLeave)
            return false;
        auto *drop = static_cast<QDropEvent *>(event);
        if (!drop->mimeData() || !drop->mimeData()->hasFormat(m_mimeType))
            return false;
        const QPoint at = drop->position().toPoint();
        if (!catcher->calibrated())
            catcher->calibrate(exitPoint() - at);
        if (event->type() == QEvent::Drop) {
            m_caught = catcher;
            m_caughtAt = at;
        } else if (overTrackedWindow(catcher->toGlobal(at))) {
            for (const QPointer<RailDropCatcher> &each : std::as_const(m_catchers)) {
                if (each)
                    each->hide();
            }
            return false;
        }
        drop->setDropAction(Qt::MoveAction);
        drop->accept();
        return true;
    }

    QString m_mimeType;
    QList<QPointer<QWidget>> m_windows;
    QList<bool> m_acceptedDrops;
    bool m_outside = false;
    bool m_dropped = false;
    QPixmap m_pixmap;
    QPoint m_hotSpot;
    QPointer<QWindow> m_lastWindow;
    QPoint m_lastPos;
    QList<QPointer<RailDropCatcher>> m_catchers;
    QPointer<RailDropCatcher> m_caught;
    QPoint m_caughtAt;
};

class ButtonDragOutPopOut : public QObject
{
public:
    ButtonDragOutPopOut(QAbstractButton *button, QWidget *mainWindow,
                        std::function<void()> popOut)
        : QObject(button), m_mainWindow(mainWindow), m_popOut(std::move(popOut))
    {
        button->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *button = qobject_cast<QAbstractButton *>(watched);
        if (!button)
            return false;
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            m_pressed = mouse->button() == Qt::LeftButton;
            m_pressAt = mouse->position().toPoint();
            return false;
        }
        case QEvent::MouseButtonRelease:
            m_pressed = false;
            return false;
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (!m_pressed || !(mouse->buttons() & Qt::LeftButton) ||
                (mouse->position().toPoint() - m_pressAt).manhattanLength() <
                    QApplication::startDragDistance())
                return false;
            m_pressed = false;
            startDrag(button);
            return true;
        }
        default:
            return false;
        }
    }

private:
    void startDrag(QAbstractButton *button)
    {
        auto *mime = new QMimeData;
        mime->setData(kPoppedOutPageMimeType, QByteArray());
        auto *drag = new QDrag(button);
        drag->setMimeData(mime);
        drag->setPixmap(button->grab());
        drag->setHotSpot(m_pressAt);
        QPointer<QAbstractButton> source(button);
        bool outside = false;
        {
            RailDragOutWatch watch({m_mainWindow.data(), button->window()},
                                   kPoppedOutPageMimeType);
            drag->exec(Qt::MoveAction);
            outside = watch.droppedOutside();
        }
        if (source)
            source->setDown(false);
        if (outside && m_popOut)
            QTimer::singleShot(0, this, m_popOut);
    }

    QPointer<QWidget> m_mainWindow;
    std::function<void()> m_popOut;
    QPoint m_pressAt;
    bool m_pressed = false;
};

class PoppedOutPageWindow : public QWidget
{
public:
    PoppedOutPageWindow(QWidget *parent, std::function<void()> onClose)
        : QWidget(parent, Qt::Window), m_onClose(std::move(onClose))
    {
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        event->ignore();
        if (m_onClose)
            QTimer::singleShot(0, this, m_onClose);
    }

private:
    std::function<void()> m_onClose;
};

void bringPoppedOutWindowForward(QWidget *window)
{
    window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    window->raise();
    window->activateWindow();
}

void placePoppedOutWindowAtCursor(QWidget *window, QWidget *mainWindow)
{
    const QPoint cursor = QCursor::pos();
    if (mainWindow && !mainWindow->frameGeometry().contains(cursor))
        window->move(cursor - QPoint(40, 16));
}

QString poppedOutPageGeometryKey(const QString &railKey)
{
    return QStringLiteral("ui/poppedOutPages/%1/geometry")
        .arg(QString(railKey).replace(QLatin1Char('/'), QLatin1Char('_')));
}

class RailDragReorder : public QObject
{
public:
    RailDragReorder(QVBoxLayout *layout,
                    std::function<void(const QStringList &)> save,
                    std::function<void()> revert,
                    std::function<bool(QWidget *)> canPopOut,
                    std::function<void(QWidget *, QWidget *)> popOut)
        : QObject(layout->parentWidget()), m_layout(layout),
          m_save(std::move(save)), m_revert(std::move(revert)),
          m_canPopOut(std::move(canPopOut)), m_popOut(std::move(popOut))
    {
        QWidget *content = layout->parentWidget();
        content->setAcceptDrops(true);
        content->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_layout)
            return false;
        if (watched == m_layout->parentWidget())
            return dragEvent(event);
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || m_layout->indexOf(widget) < 0)
            return false;
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                m_pressed = widget;
                m_pressAt = mouse->position().toPoint();
            }
            return false;
        }
        case QEvent::MouseButtonRelease:
            m_pressed = nullptr;
            return false;
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (m_pressed != widget || !(mouse->buttons() & Qt::LeftButton) ||
                (mouse->position().toPoint() - m_pressAt).manhattanLength() <
                    QApplication::startDragDistance())
                return false;
            startDrag(widget);
            return true;
        }
        default:
            return false;
        }
    }

private:
    void startDrag(QWidget *widget)
    {
        m_pressed = nullptr;
        if (auto *button = qobject_cast<QAbstractButton *>(widget))
            button->setDown(false);
        auto *mime = new QMimeData;
        mime->setData(kRailItemMimeType,
                      widget->property("forkmeshRailKey").toString().toUtf8());
        auto *drag = new QDrag(widget);
        drag->setMimeData(mime);
        drag->setPixmap(widget->grab());
        drag->setHotSpot(m_pressAt);
        m_dropped = false;
        QPointer<QWidget> source(widget);
        bool outside = false;
        QWidget *spinner = nullptr;
        {
            RailDragOutWatch watch({widget->window()},
                                   kRailItemMimeType,
                                   m_canPopOut(widget) ? drag : nullptr);
            drag->exec(Qt::MoveAction);
            outside = watch.droppedOutside();
            spinner = watch.takeSpinner();
        }
        if (!m_dropped)
            m_revert();
        if (outside && source)
            m_popOut(source, spinner);
        else
            delete spinner;
    }

    bool dragEvent(QEvent *event)
    {
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            QWidget *item = itemFor(drag->mimeData());
            if (!item)
                return false;
            const QPoint at = drag->position().toPoint();
            moveItem(item, at.y());
            if (QWidget *viewport = m_layout->parentWidget()->parentWidget()) {
                if (auto *scroll =
                        qobject_cast<QScrollArea *>(viewport->parentWidget()))
                    scroll->ensureVisible(at.x(), at.y(), 0, 24);
            }
            drag->setDropAction(Qt::MoveAction);
            drag->accept();
            return true;
        }
        case QEvent::Drop: {
            auto *drop = static_cast<QDropEvent *>(event);
            QWidget *item = itemFor(drop->mimeData());
            if (!item)
                return false;
            moveItem(item, drop->position().toPoint().y());
            drop->setDropAction(Qt::MoveAction);
            drop->accept();
            m_dropped = true;
            m_save(railKeys());
            return true;
        }
        default:
            return false;
        }
    }

    QWidget *itemFor(const QMimeData *mime) const
    {
        if (!mime || !mime->hasFormat(kRailItemMimeType))
            return nullptr;
        const QString key = QString::fromUtf8(mime->data(kRailItemMimeType));
        for (int i = 0; i < m_layout->count(); ++i) {
            QWidget *widget = m_layout->itemAt(i)->widget();
            if (widget && !key.isEmpty() &&
                widget->property("forkmeshRailKey").toString() == key)
                return widget;
        }
        return nullptr;
    }

    void moveItem(QWidget *item, int y)
    {
        QList<QWidget *> others;
        for (int i = 0; i < m_layout->count(); ++i) {
            QWidget *widget = m_layout->itemAt(i)->widget();
            if (widget && widget != item)
                others.append(widget);
        }
        int target = 0;
        for (int i = 0; i < others.size(); ++i) {
            if (!others.at(i)->isHidden() &&
                others.at(i)->geometry().center().y() < y)
                target = i + 1;
        }
        if (m_layout->indexOf(item) == target)
            return;
        m_layout->removeWidget(item);
        m_layout->insertWidget(target, item, 0, Qt::AlignLeft);
        m_layout->activate();
    }

    QStringList railKeys() const
    {
        QStringList keys;
        for (int i = 0; i < m_layout->count(); ++i) {
            QWidget *widget = m_layout->itemAt(i)->widget();
            const QString key =
                widget ? widget->property("forkmeshRailKey").toString()
                       : QString();
            if (!key.isEmpty())
                keys.append(key);
        }
        return keys;
    }

    QPointer<QVBoxLayout> m_layout;
    std::function<void(const QStringList &)> m_save;
    std::function<void()> m_revert;
    std::function<bool(QWidget *)> m_canPopOut;
    std::function<void(QWidget *, QWidget *)> m_popOut;
    QPointer<QWidget> m_pressed;
    QPoint m_pressAt;
    bool m_dropped = false;
};

} // namespace

namespace {

constexpr int kRailClickCountsFlushMs = 30 * 1000;
QHash<QString, qint64> g_pendingRailClicks;

QVariantMap railClickCounts()
{
    QVariantMap counts = QSettings().value(kRailClickCountsSetting).toMap();
    for (auto it = g_pendingRailClicks.cbegin(); it != g_pendingRailClicks.cend();
         ++it)
        counts.insert(it.key(), counts.value(it.key()).toLongLong() + it.value());
    return counts;
}

void flushRailClickCounts()
{
    if (g_pendingRailClicks.isEmpty())
        return;
    const QVariantMap counts = railClickCounts();
    g_pendingRailClicks.clear();
    QSettings().setValue(kRailClickCountsSetting, counts);
}

void recordPendingRailClick(const QString &key)
{
    const bool scheduled = !g_pendingRailClicks.isEmpty();
    g_pendingRailClicks[key] += 1;
    if (scheduled)
        return;
    static bool flushOnQuit = false;
    if (!flushOnQuit && QCoreApplication::instance()) {
        flushOnQuit = true;
        QObject::connect(QCoreApplication::instance(),
                         &QCoreApplication::aboutToQuit,
                         QCoreApplication::instance(), &flushRailClickCounts);
    }
    QTimer::singleShot(kRailClickCountsFlushMs, QCoreApplication::instance(),
                       &flushRailClickCounts);
}

constexpr int kRailCountSortDelayMs = 400;
constexpr int kRailSortButtonHeight = 16;

struct RailSortRank {
    QString name;
    qint64 clicks = 0;
    int badge = 0;
};

QString railItemName(QWidget *widget)
{
    if (auto *folder = dynamic_cast<RailFolderWidget *>(widget))
        return folder->name();
    if (auto *rail = dynamic_cast<ActivityRailButton *>(widget))
        if (!rail->label().isEmpty())
            return rail->label();
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) {
        const QString text = button->text().remove(QLatin1Char('&')).simplified();
        if (!text.isEmpty())
            return text;
    }
    if (!widget->accessibleName().isEmpty())
        return widget->accessibleName();
    return widget->property("forkmeshRailKey").toString();
}

int railItemBadge(QWidget *widget)
{
    auto *rail = dynamic_cast<ActivityRailButton *>(widget);
    return rail ? rail->badgeCount() : 0;
}

bool railRankLess(RailGroups::SortMode mode, const RailSortRank &a,
                  const RailSortRank &b)
{
    switch (mode) {
    case RailGroups::SortMode::Name:
        return QString::localeAwareCompare(a.name.toCaseFolded(),
                                           b.name.toCaseFolded()) < 0;
    case RailGroups::SortMode::Used:
        return a.clicks > b.clicks;
    case RailGroups::SortMode::Count:
        return a.badge > b.badge;
    case RailGroups::SortMode::Custom:
        break;
    }
    return false;
}

QString railEntryGroup(const RailGroups::State &groups,
                       const RailFolders::State &folders, const QString &key)
{
    if (!RailFolders::isFolderKey(key))
        return RailGroups::groupOf(groups, key);
    const QString assigned = RailGroups::assignedGroupOf(groups, key);
    if (!assigned.isEmpty())
        return assigned;
    const int index =
        RailFolders::indexOf(folders, key.mid(RailFolders::kKeyPrefix.size()));
    const QString first =
        index >= 0 ? folders.at(index).members.value(0) : QString();
    return first.isEmpty() ? QStringLiteral("more")
                           : RailGroups::groupOf(groups, first);
}

}

void MainWindow::applyRailOrder()
{
    if (!m_appNavigationRailLayout)
        return;
    if (!m_railDragReorder)
        m_railDragReorder = new RailDragReorder(
            m_appNavigationRailLayout,
            [this](const QStringList &keys) {
                adoptRailGroupsFromLayout();
                saveRailOrder(keys);
            },
            [this] { applyRailOrder(); },
            [this](QWidget *item) { return canPopOutRailItem(item); },
            [this](QWidget *item, QWidget *spinner) {
                popOutRailItemLater(item, spinner);
            });
    QWidget *railContent = m_appNavigationRailLayout->parentWidget();
    if (!m_railEventHook) {
        m_railEventHook = new RailEventHook(
            [this](QObject *watched, QEvent *event) {
                return handleRailEvent(watched, event);
            },
            this);
        if (railContent) {
            railContent->setAcceptDrops(true);
            railContent->installEventFilter(m_railEventHook);
        }
        if (m_navGroup)
            connect(m_navGroup, &QButtonGroup::buttonToggled, this,
                    [this] { refreshRailFolders(); });
    }

    placeMovableButtons();
    applyRepoTabRowOrder();
    syncRailShortcutTiles();

    QList<QPair<QString, QWidget *>> canonical = railHomeButtons();
    int movedInAt = 0;
    for (int i = 0; i < canonical.size(); ++i) {
        if (canonical.at(i).first == QLatin1String("git"))
            movedInAt = i + 1;
    }
    for (const auto &[key, widget] :
         m_repoTabRowTabs + m_repoTabRowActions + m_repoCodeBarButtons) {
        if (widget)
            canonical.insert(movedInAt++, {key, widget.data()});
    }
    for (const RailShortcuts::Shortcut &shortcut : railShortcutState()) {
        if (ActivityRailButton *tile = m_railShortcutButtons.value(shortcut.id))
            canonical.append({RailShortcuts::railKey(shortcut.id), tile});
    }

    QList<QWidget *> current;
    for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
        QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
        if (!widget || dynamic_cast<RailGroupSeparator *>(widget))
            continue;
        if (auto *folder = dynamic_cast<RailFolderWidget *>(widget))
            current += folder->members();
        else
            current.append(widget);
    }

    QList<QPair<QString, QWidget *>> order;
    for (const auto &[key, widget] : std::as_const(canonical)) {
        if (!widget || !current.contains(widget))
            continue;
        order.append({key, widget});
        if (!widget->property("forkmeshRailKey").isValid()) {
            widget->setProperty("forkmeshRailKey", key);
            widget->installEventFilter(m_railDragReorder);
            if (auto *button = qobject_cast<QAbstractButton *>(widget))
                connect(button, &QAbstractButton::clicked, this,
                        [this, key] {
                            recordRailClick(key);
                            QTimer::singleShot(0, this,
                                               [this] { raisePoppedOutPages(); });
                        });
            if (auto *rail = dynamic_cast<ActivityRailButton *>(widget)) {
                QPointer<MainWindow> self(this);
                rail->setBadgeOrderObserver([self] {
                    if (self)
                        self->scheduleRailCountSort();
                });
            }
            hookRailWidget(widget);
        }
    }
    for (QWidget *widget : std::as_const(current)) {
        const bool known = std::any_of(
            order.cbegin(), order.cend(),
            [widget](const auto &entry) { return entry.second == widget; });
        if (!known)
            order.append({QString(), widget});
    }

    const QStringList saved = QSettings().value(kRailOrderSetting).toStringList();
    if (!saved.isEmpty()) {
        QList<std::pair<double, QPair<QString, QWidget *>>> ranked;
        double previous = -1;
        for (const auto &entry : std::as_const(order)) {
            const int at = entry.first.isEmpty() ? -1 : saved.indexOf(entry.first);
            if (at >= 0)
                previous = at;
            ranked.append({at >= 0 ? at : previous + 0.5, entry});
        }
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto &a, const auto &b) {
                             return a.first < b.first;
                         });
        order.clear();
        for (const auto &entry : std::as_const(ranked))
            order.append(entry.second);
    }

    const RailGroups::SortMode sortMode = railSortMode();
    const QVariantMap counts = sortMode == RailGroups::SortMode::Used
                                   ? railClickCounts()
                                   : QVariantMap();
    const auto rankOf = [&](const QString &key, QWidget *widget) {
        RailSortRank rank;
        if (sortMode == RailGroups::SortMode::Name)
            rank.name = railItemName(widget);
        rank.clicks = counts.value(key).toLongLong();
        rank.badge = railItemBadge(widget);
        return rank;
    };
    if (sortMode != RailGroups::SortMode::Custom) {
        QHash<QWidget *, RailSortRank> ranks;
        for (const auto &[key, widget] : std::as_const(order))
            ranks.insert(widget, rankOf(key, widget));
        std::stable_sort(order.begin(), order.end(),
                         [&](const auto &a, const auto &b) {
                             return railRankLess(sortMode, ranks.value(a.second),
                                                 ranks.value(b.second));
                         });
    }

    const RailFolders::State &folders = railFolderState();
    QList<QPointer<RailFolderWidget>> stale;
    for (auto it = m_railFolderWidgets.begin(); it != m_railFolderWidgets.end();) {
        if (!it.value() || RailFolders::indexOf(folders, it.key()) < 0) {
            stale.append(it.value());
            it = m_railFolderWidgets.erase(it);
        } else {
            ++it;
        }
    }
    for (const RailFolders::Folder &folder : folders) {
        RailFolderWidget *widget = m_railFolderWidgets.value(folder.id);
        if (!widget) {
            widget = new RailFolderWidget(folder.id, folder.name, railContent);
            const QString id = folder.id;
            widget->onDropKey = [this, id](const QString &key) {
                moveRailItemToFolder(key, id);
            };
            widget->header()->installEventFilter(m_railEventHook);
            widget->followRailCaptionPreference();
            m_railFolderWidgets.insert(folder.id, widget);
        } else if (widget->name() != folder.name) {
            widget->setName(folder.name);
        }
    }

    QList<QWidget *> top;
    QHash<QString, QList<QWidget *>> members;
    for (const auto &[key, widget] : std::as_const(order)) {
        RailFolderWidget *folder =
            m_railFolderWidgets.value(RailFolders::folderOf(folders, key));
        if (!folder) {
            top.append(widget);
            continue;
        }
        if (!top.contains(folder))
            top.append(folder);
        members[folder->id()].append(widget);
    }
    for (const RailFolders::Folder &folder : folders) {
        RailFolderWidget *widget = m_railFolderWidgets.value(folder.id);
        if (widget && !top.contains(widget))
            top.append(widget);
    }
    if (sortMode != RailGroups::SortMode::Custom) {
        QHash<QWidget *, RailSortRank> ranks;
        for (QWidget *widget : std::as_const(top)) {
            auto *folder = dynamic_cast<RailFolderWidget *>(widget);
            if (!folder) {
                ranks.insert(widget,
                             rankOf(widget->property("forkmeshRailKey").toString(),
                                    widget));
                continue;
            }
            RailSortRank rank;
            rank.name = folder->name();
            for (QWidget *member : members.value(folder->id())) {
                const RailSortRank inner = rankOf(
                    member->property("forkmeshRailKey").toString(), member);
                rank.clicks += inner.clicks;
                rank.badge += inner.badge;
            }
            ranks.insert(widget, rank);
        }
        std::stable_sort(top.begin(), top.end(), [&](QWidget *a, QWidget *b) {
            return railRankLess(sortMode, ranks.value(a), ranks.value(b));
        });
    }

    QList<RailGroupSeparator *> unusedSeparators;
    for (auto it = m_railGroupSeparators.begin();
         it != m_railGroupSeparators.end();) {
        if (!it.value()) {
            it = m_railGroupSeparators.erase(it);
            continue;
        }
        unusedSeparators.append(it.value());
        ++it;
    }
    if (railGrouped()) {
        const RailGroups::State &groups = railGroupState();
        QHash<QString, QList<QWidget *>> byGroup;
        for (QWidget *widget : std::as_const(top))
            byGroup[railEntryGroup(groups, folders,
                                   widget->property("forkmeshRailKey").toString())]
                .append(widget);
        QList<QWidget *> rows;
        for (const RailGroups::Group &group : groups.groups) {
            const QList<QWidget *> items = byGroup.value(group.id);
            if (items.isEmpty() && group.builtIn)
                continue;
            RailGroupSeparator *separator = m_railGroupSeparators.value(group.id);
            if (!separator) {
                separator = new RailGroupSeparator(group.id, group.name,
                                                   !group.builtIn, railContent);
                separator->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(separator, &QWidget::customContextMenuRequested, this,
                        [this, separator](const QPoint &at) {
                            showRailGroupMenu(separator, separator->mapToGlobal(at));
                        });
                const QString id = group.id;
                separator->onDropKey = [this, id](const QString &key) {
                    assignRailItemToGroup(key, id);
                };
                m_railGroupSeparators.insert(group.id, separator);
            } else if (separator->name() != group.name) {
                separator->setName(group.name);
            }
            unusedSeparators.removeAll(separator);
            rows.append(separator);
            rows += items;
        }
        top = rows;
    }
    for (RailGroupSeparator *separator : std::as_const(unusedSeparators)) {
        separator->hide();
        if (RailGroups::indexOf(railGroupState(), separator->id()) < 0) {
            m_railGroupSeparators.remove(separator->id());
            separator->deleteLater();
        }
    }

    const int count = m_appNavigationRailLayout->count();
    bool unchanged = stale.isEmpty() && count == top.size() + 1 &&
                     m_appNavigationRailLayout->itemAt(count - 1)->spacerItem();
    for (int i = 0; unchanged && i < top.size(); ++i)
        unchanged = m_appNavigationRailLayout->itemAt(i)->widget() == top.at(i);
    for (auto it = m_railFolderWidgets.cbegin();
         unchanged && it != m_railFolderWidgets.cend(); ++it)
        unchanged = it.value()->members() == members.value(it.key());
    if (unchanged) {
        refreshRailGroupSeparators();
        refreshRailFolders();
        return;
    }

    while (QLayoutItem *item = m_appNavigationRailLayout->takeAt(0))
        delete item;
    for (QWidget *widget : std::as_const(top))
        m_appNavigationRailLayout->addWidget(widget, 0, Qt::AlignLeft);
    refreshRailGroupSeparators();
    for (auto it = m_railFolderWidgets.cbegin(); it != m_railFolderWidgets.cend();
         ++it)
        it.value()->setMembers(members.value(it.key()));
    m_appNavigationRailLayout->addStretch();
    for (const QPointer<RailFolderWidget> &folder : std::as_const(stale)) {
        if (!folder)
            continue;
        folder->hide();
        folder->deleteLater();
    }
    refreshRailFolders();
}

bool MainWindow::isInRail(QWidget *widget) const
{
    if (!widget || !m_appNavigationRailLayout)
        return false;
    if (m_appNavigationRailLayout->indexOf(widget) >= 0)
        return true;
    for (const QPointer<RailFolderWidget> &folder :
         std::as_const(m_railFolderWidgets))
        if (folder && folder->members().contains(widget))
            return true;
    return false;
}

RailFolders::State &MainWindow::railFolderState()
{
    if (!m_railFolderStateLoaded) {
        m_railFolderStateLoaded = true;
        m_railFolderState =
            RailFolders::parse(QSettings().value(kRailFoldersSetting).toString());
    }
    return m_railFolderState;
}

void MainWindow::saveRailFolderState()
{
    QSettings settings;
    if (railFolderState().isEmpty())
        settings.remove(kRailFoldersSetting);
    else
        settings.setValue(kRailFoldersSetting,
                          RailFolders::serialize(railFolderState()));
}

void MainWindow::refreshRailFolders()
{
    for (const QPointer<RailFolderWidget> &folder :
         std::as_const(m_railFolderWidgets))
        if (folder)
            folder->refresh();
}

void MainWindow::hookRailWidget(QWidget *widget)
{
    if (!widget || !m_railEventHook ||
        widget->property("forkmeshRailHooked").toBool())
        return;
    widget->setProperty("forkmeshRailHooked", true);
    widget->installEventFilter(m_railEventHook);
}

bool MainWindow::handleRailEvent(QObject *watched, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget || !m_appNavigationRailLayout)
        return false;
    const auto draggedKey = [](const QMimeData *mime) {
        return mime && mime->hasFormat(kRailItemMimeType)
                   ? QString::fromUtf8(mime->data(kRailItemMimeType))
                   : QString();
    };

    if (widget == m_appNavigationRailLayout->parentWidget()) {
        if (handleRailPageButtonDrag(event))
            return true;
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove: {
            auto *drag = static_cast<QDragMoveEvent *>(event);
            const QString key = draggedKey(drag->mimeData());
            if (key.isEmpty() ||
                RailFolders::folderOf(railFolderState(), key).isEmpty())
                return false;
            drag->acceptProposedAction();
            return true;
        }
        case QEvent::Drop: {
            auto *drop = static_cast<QDropEvent *>(event);
            const QString key = draggedKey(drop->mimeData());
            if (key.isEmpty() ||
                RailFolders::folderOf(railFolderState(), key).isEmpty())
                return false;
            drop->acceptProposedAction();
            moveRailItemToFolder(key, QString());
            return true;
        }
        case QEvent::ContextMenu: {
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->addAction(QStringLiteral("New folder…"), this,
                            [this] { promptNewRailFolder(QString()); });
            menu->addAction(QStringLiteral("New group…"), this,
                            [this] { promptNewRailGroup(QString()); });
            QMenu *sort = buildRailSortMenu(menu);
            sort->setTitle(QStringLiteral("Sort the left rail"));
            menu->addMenu(sort);
            menu->popup(static_cast<QContextMenuEvent *>(event)->globalPos());
            return true;
        }
        default:
            return false;
        }
    }

    const QString folderId = widget->property("forkmeshRailFolderId").toString();
    const QString key = widget->property("forkmeshRailKey").toString();
    if (folderId.isEmpty() && !isInRail(widget)) {
        if (m_railDragSource == widget)
            m_railDragSource.clear();
        return false;
    }
    switch (event->type()) {
    case QEvent::ContextMenu: {
        const QPoint globalPos = static_cast<QContextMenuEvent *>(event)->globalPos();
        if (!folderId.isEmpty())
            showRailFolderMenu(m_railFolderWidgets.value(folderId), globalPos);
        else
            showRailItemMenu(widget, globalPos);
        return true;
    }
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (folderId.isEmpty() && !key.isEmpty() &&
            mouse->button() == Qt::LeftButton &&
            m_appNavigationRailLayout->indexOf(widget) < 0) {
            m_railDragSource = widget;
            m_railDragStart = mouse->position().toPoint();
        }
        return false;
    }
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (m_railDragSource != widget || !(mouse->buttons() & Qt::LeftButton) ||
            (mouse->position().toPoint() - m_railDragStart).manhattanLength() <
                QApplication::startDragDistance())
            return false;
        m_railDragSource.clear();
        auto *mime = new QMimeData;
        mime->setData(kRailItemMimeType, key.toUtf8());
        auto *drag = new QDrag(widget);
        drag->setMimeData(mime);
        drag->setPixmap(widget->grab());
        drag->setHotSpot(m_railDragStart);
        QPointer<QWidget> source(widget);
        bool outside = false;
        QWidget *spinner = nullptr;
        {
            RailDragOutWatch watch({this, widget->window()},
                                   kRailItemMimeType,
                                   canPopOutRailItem(widget) ? drag : nullptr);
            drag->exec(Qt::MoveAction);
            outside = watch.droppedOutside();
            spinner = watch.takeSpinner();
        }
        if (auto *button = qobject_cast<QAbstractButton *>(source.data()))
            button->setDown(false);
        if (outside && source)
            popOutRailItemLater(source, spinner);
        else
            delete spinner;
        return true;
    }
    case QEvent::MouseButtonRelease:
        m_railDragSource.clear();
        return false;
    case QEvent::ShowToParent:
    case QEvent::HideToParent:
        if (widget->parentWidget() &&
            widget->parentWidget()->objectName() ==
                QLatin1String("railFolderBody"))
            refreshRailFolders();
        else if (!m_railGroupSeparators.isEmpty())
            QMetaObject::invokeMethod(
                this, [this] { refreshRailGroupSeparators(); },
                Qt::QueuedConnection);
        return false;
    default:
        return false;
    }
}

void MainWindow::showRailItemMenu(QWidget *widget, const QPoint &globalPos)
{
    if (!widget)
        return;
    const QString key = widget->property("forkmeshRailKey").toString();
    const QString entrypoint =
        widget->property("forkmeshPackageEntrypoint").toString();
    QMenu *menu = entrypoint.isEmpty() ? nullptr
                                       : buildPackageRailMenu(entrypoint, this);
    if (!menu)
        menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (!key.startsWith(QLatin1String("package:")) && canPopOutRailItem(widget)) {
        QPointer<QWidget> item(widget);
        menu->addAction(QStringLiteral("Open in new window"), this,
                        [this, item] { popOutRailItemLater(item); });
        menu->addSeparator();
    }
    if (RailShortcuts::isShortcutKey(key)) {
        const QString id = RailShortcuts::idOf(key);
        menu->addAction(QStringLiteral("Remove from left rail"), this, [this, id] {
            QTimer::singleShot(0, this, [this, id] { removeRailShortcut(id); });
        });
    }
    addButtonPlacementActions(menu, widget);
    if (!key.isEmpty()) {
        if (!menu->isEmpty())
            menu->addSeparator();
        const RailFolders::State &folders = railFolderState();
        const QString currentFolder = RailFolders::folderOf(folders, key);
        QMenu *move = menu->addMenu(QStringLiteral("Move to folder"));
        for (const RailFolders::Folder &folder : folders) {
            QAction *action = move->addAction(
                folder.name, this,
                [this, key, id = folder.id] { moveRailItemToFolder(key, id); });
            action->setCheckable(true);
            action->setChecked(folder.id == currentFolder);
            action->setEnabled(folder.id != currentFolder);
        }
        if (!folders.isEmpty())
            move->addSeparator();
        move->addAction(QStringLiteral("New folder…"), this,
                        [this, key] { promptNewRailFolder(key); });
        if (!currentFolder.isEmpty())
            menu->addAction(QStringLiteral("Take out of folder"), this, [this, key] {
                moveRailItemToFolder(key, QString());
            });
        addRailGroupActions(menu, key);
    }
    if (menu->isEmpty()) {
        menu->deleteLater();
        return;
    }
    menu->popup(globalPos);
}

void MainWindow::showRailFolderMenu(RailFolderWidget *folder,
                                    const QPoint &globalPos)
{
    if (!folder)
        return;
    const QString id = folder->id();
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addAction(folder->isPinned() ? QStringLiteral("Close folder")
                                       : QStringLiteral("Keep folder open"),
                    this, [this, id] {
                        if (RailFolderWidget *widget = m_railFolderWidgets.value(id))
                            widget->header()->click();
                    });
    menu->addAction(QStringLiteral("Rename folder…"), this,
                    [this, id] { renameRailFolder(id); });
    menu->addAction(QStringLiteral("New folder…"), this,
                    [this] { promptNewRailFolder(QString()); });
    addRailGroupActions(menu, RailFolders::railKey(id));
    menu->addSeparator();
    menu->addAction(QStringLiteral("Remove folder (keeps its items)"), this,
                    [this, id] { deleteRailFolder(id); });
    menu->popup(globalPos);
}

void MainWindow::moveRailItemToFolder(const QString &key, const QString &folderId)
{
    if (!RailFolders::move(railFolderState(), key, folderId))
        return;
    saveRailFolderState();
    applyRailOrder();
}

void MainWindow::promptNewRailFolder(const QString &key)
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New rail folder"), QStringLiteral("Folder name:"),
        QLineEdit::Normal, QStringLiteral("Folder"), &ok);
    if (!ok)
        return;
    RailFolders::State &state = railFolderState();
    const QString id = RailFolders::create(state, name);
    if (id.isEmpty())
        return;
    RailFolders::move(state, key, id);
    saveRailFolderState();
    applyRailOrder();
}

void MainWindow::renameRailFolder(const QString &folderId)
{
    RailFolders::State &state = railFolderState();
    const int index = RailFolders::indexOf(state, folderId);
    if (index < 0)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename rail folder"), QStringLiteral("Folder name:"),
        QLineEdit::Normal, state.at(index).name, &ok);
    if (!ok || !RailFolders::rename(state, folderId, name))
        return;
    saveRailFolderState();
    applyRailOrder();
}

void MainWindow::deleteRailFolder(const QString &folderId)
{
    if (!RailFolders::remove(railFolderState(), folderId))
        return;
    saveRailFolderState();
    applyRailOrder();
}

void MainWindow::recordRailClick(const QString &key)
{
    recordPendingRailClick(key);
    if (railSortMode() == RailGroups::SortMode::Used)
        applyRailOrder();
}

void MainWindow::saveRailOrder(const QStringList &keys)
{
    QSettings settings;
    QStringList merged = keys;
    int insertAt = 0;
    for (const QString &key : settings.value(kRailOrderSetting).toStringList()) {
        const int at = merged.indexOf(key);
        if (at >= 0)
            insertAt = at + 1;
        else
            merged.insert(insertAt++, key);
    }
    settings.setValue(kRailOrderSetting, merged);
    if (railSortMode() != RailGroups::SortMode::Custom)
        setRailSortMode(RailGroups::SortMode::Custom);
    else
        applyRailOrder();
}

RailGroups::SortMode MainWindow::railSortMode() const
{
    QSettings settings;
    const RailGroups::SortMode mode =
        RailGroups::parseSortMode(settings.value(kRailSortModeSetting).toString());
    if (mode == RailGroups::SortMode::Custom &&
        settings.value(kSortRailByClicksSetting, false).toBool())
        return RailGroups::SortMode::Used;
    return mode;
}

void MainWindow::setRailSortMode(RailGroups::SortMode mode)
{
    QSettings settings;
    settings.setValue(kRailSortModeSetting, RailGroups::sortModeKey(mode));
    settings.setValue(kSortRailByClicksSetting,
                      mode == RailGroups::SortMode::Used);
    if (auto *check =
            findChild<QCheckBox *>(QStringLiteral("railSortByClicksCheck"))) {
        const QSignalBlocker blocker(check);
        check->setChecked(mode == RailGroups::SortMode::Used);
    }
    refreshRailSortButton();
    applyRailOrder();
}

bool MainWindow::railGrouped() const
{
    return QSettings().value(kRailGroupedSetting, false).toBool();
}

void MainWindow::setRailGrouped(bool grouped)
{
    if (railGrouped() == grouped)
        return;
    QSettings().setValue(kRailGroupedSetting, grouped);
    refreshRailSortButton();
    applyRailOrder();
}

RailGroups::State &MainWindow::railGroupState()
{
    if (!m_railGroupStateLoaded) {
        m_railGroupStateLoaded = true;
        m_railGroupState =
            RailGroups::parse(QSettings().value(kRailGroupsSetting).toString());
    }
    return m_railGroupState;
}

void MainWindow::saveRailGroupState()
{
    QSettings settings;
    if (RailGroups::isDefault(railGroupState()))
        settings.remove(kRailGroupsSetting);
    else
        settings.setValue(kRailGroupsSetting,
                          RailGroups::serialize(railGroupState()));
}

QPushButton *MainWindow::buildRailSortButton()
{
    auto *button = new QPushButton;
    button->setObjectName(QStringLiteral("railSortButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFlat(true);
    button->setFixedSize(railColumnWidth(), kRailSortButtonHeight);
    setOcticon(button, QStringLiteral("sort-desc"), 12);
    m_railSortButton = button;
    refreshRailSortButton();
    connect(button, &QPushButton::clicked, this, [this] { showRailSortMenu(); });
    return button;
}

void MainWindow::refreshRailSortButton()
{
    if (!m_railSortButton)
        return;
    const RailGroups::SortMode mode = railSortMode();
    const QString title = RailGroups::sortModeTitle(mode);
    const QString summary = railGrouped()
                                ? QStringLiteral("%1, in groups").arg(title)
                                : title;
    m_railSortButton->setAccessibleName(
        QStringLiteral("Sort the left rail (%1)").arg(summary));
    m_railSortButton->setToolTip(
        QStringLiteral("Sort the left rail: %1\nClick to sort alphabetically, by "
                       "most used or by count, or to group the icons.")
            .arg(summary));
    m_railSortButton->setProperty("railSortMode", RailGroups::sortModeKey(mode));
    m_railSortButton->setProperty("railGrouped", railGrouped());
    const bool expanded = forkmesh::ui::railExpanded();
    const QString label = expanded ? summary : QString();
    if (m_railSortButton->text() != label)
        m_railSortButton->setText(label);
    m_railSortButton->setFixedHeight(expanded ? 22 : kRailSortButtonHeight);
    const QString sheet =
        QStringLiteral(
            "QPushButton#railSortButton { border: none; padding: 0 0 0 %1px; margin: 0;"
            " background: transparent; min-height: 0; text-align: %2; color: %3;"
            " font-size: 11px; }"
            "QPushButton#railSortButton:hover { background: rgba(127, 127, 127, 40);"
            " border-radius: 3px; color: %4; }")
            .arg(expanded ? 12 : 0)
            .arg(expanded ? QStringLiteral("left") : QStringLiteral("center"))
            .arg(Theme::hex(Theme::Role::FgMuted), Theme::hex(Theme::Role::Fg));
    if (m_railSortButton->styleSheet() != sheet)
        m_railSortButton->setStyleSheet(sheet);
}

QMenu *MainWindow::buildRailSortMenu(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("railSortMenu"));
    menu->addSection(QStringLiteral("Sort"));
    auto *modes = new QActionGroup(menu);
    modes->setExclusive(true);
    const RailGroups::SortMode current = railSortMode();
    struct Choice {
        RailGroups::SortMode mode;
        const char *label;
    };
    static const Choice choices[] = {
        {RailGroups::SortMode::Custom, "Custom order (drag to arrange)"},
        {RailGroups::SortMode::Name, "Alphabetically"},
        {RailGroups::SortMode::Used, "Most frequently used"},
        {RailGroups::SortMode::Count, "By count"},
    };
    for (const Choice &choice : choices) {
        const RailGroups::SortMode mode = choice.mode;
        QAction *action = menu->addAction(QString::fromUtf8(choice.label), this,
                                          [this, mode] { setRailSortMode(mode); });
        action->setObjectName(QStringLiteral("railSort_%1")
                                  .arg(RailGroups::sortModeKey(mode)));
        action->setCheckable(true);
        action->setChecked(mode == current);
        modes->addAction(action);
    }
    menu->addSection(QStringLiteral("Groups"));
    QAction *group = menu->addAction(QStringLiteral("Group the icons"), this,
                                     [this] { setRailGrouped(!railGrouped()); });
    group->setObjectName(QStringLiteral("railSortGroup"));
    group->setCheckable(true);
    group->setChecked(railGrouped());
    group->setToolTip(QStringLiteral(
        "Split the rail into labelled groups. The sort applies inside each group."));
    menu->addAction(QStringLiteral("New group…"), this,
                    [this] { promptNewRailGroup(QString()); })
        ->setObjectName(QStringLiteral("railSortNewGroup"));
    if (!RailGroups::isDefault(railGroupState()))
        menu->addAction(QStringLiteral("Reset groups"), this, [this] {
                railGroupState() = RailGroups::parse(QString());
                saveRailGroupState();
                applyRailOrder();
            })->setObjectName(QStringLiteral("railSortResetGroups"));
    return menu;
}

void MainWindow::showRailSortMenu()
{
    if (!m_railSortButton)
        return;
    QMenu *menu = buildRailSortMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_railSortButton->mapToGlobal(
        QPoint(0, m_railSortButton->height())));
}

void MainWindow::addRailGroupActions(QMenu *menu, const QString &key)
{
    if (!menu || key.isEmpty() || !railGrouped())
        return;
    const RailGroups::State &groups = railGroupState();
    const QString current =
        railEntryGroup(groups, railFolderState(), key);
    QMenu *move = menu->addMenu(QStringLiteral("Move to group"));
    for (const RailGroups::Group &group : groups.groups) {
        QAction *action = move->addAction(
            group.name, this,
            [this, key, id = group.id] { assignRailItemToGroup(key, id); });
        action->setCheckable(true);
        action->setChecked(group.id == current);
        action->setEnabled(group.id != current);
    }
    move->addSeparator();
    move->addAction(QStringLiteral("New group…"), this,
                    [this, key] { promptNewRailGroup(key); });
}

void MainWindow::showRailGroupMenu(RailGroupSeparator *separator,
                                   const QPoint &globalPos)
{
    if (!separator)
        return;
    const QString id = separator->id();
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    if (separator->isCustom())
        menu->addAction(QStringLiteral("Rename group…"), this,
                        [this, id] { renameRailGroup(id); });
    menu->addAction(QStringLiteral("Move group up"), this,
                    [this, id] { moveRailGroup(id, -1); });
    menu->addAction(QStringLiteral("Move group down"), this,
                    [this, id] { moveRailGroup(id, 1); });
    menu->addAction(QStringLiteral("New group…"), this,
                    [this] { promptNewRailGroup(QString()); });
    QMenu *sort = buildRailSortMenu(menu);
    sort->setTitle(QStringLiteral("Sort the left rail"));
    menu->addMenu(sort);
    menu->addSeparator();
    if (separator->isCustom())
        menu->addAction(QStringLiteral("Remove group (keeps its items)"), this,
                        [this, id] { deleteRailGroup(id); });
    menu->addAction(QStringLiteral("Stop grouping"), this,
                    [this] { setRailGrouped(false); });
    menu->popup(globalPos);
}

void MainWindow::promptNewRailGroup(const QString &key)
{
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New rail group"), QStringLiteral("Group name:"),
        QLineEdit::Normal, QStringLiteral("Group"), &ok);
    if (!ok)
        return;
    RailGroups::State &state = railGroupState();
    const QString id = RailGroups::create(state, name);
    if (id.isEmpty())
        return;
    if (!key.isEmpty() &&
        !RailFolders::folderOf(railFolderState(), key).isEmpty() &&
        RailFolders::move(railFolderState(), key, QString()))
        saveRailFolderState();
    RailGroups::assign(state, key, id);
    saveRailGroupState();
    if (!railGrouped())
        setRailGrouped(true);
    else
        applyRailOrder();
}

void MainWindow::renameRailGroup(const QString &groupId)
{
    RailGroups::State &state = railGroupState();
    const int index = RailGroups::indexOf(state, groupId);
    if (index < 0 || state.groups.at(index).builtIn)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("Rename rail group"), QStringLiteral("Group name:"),
        QLineEdit::Normal, state.groups.at(index).name, &ok);
    if (!ok || !RailGroups::rename(state, groupId, name))
        return;
    saveRailGroupState();
    applyRailOrder();
}

void MainWindow::deleteRailGroup(const QString &groupId)
{
    if (!RailGroups::remove(railGroupState(), groupId))
        return;
    saveRailGroupState();
    applyRailOrder();
}

void MainWindow::moveRailGroup(const QString &groupId, int step)
{
    QSet<QString> shown;
    if (QWidget *content = m_appNavigationRailLayout
                               ? m_appNavigationRailLayout->parentWidget()
                               : nullptr) {
        for (const QPointer<RailGroupSeparator> &separator :
             std::as_const(m_railGroupSeparators))
            if (separator && separator->isVisibleTo(content))
                shown.insert(separator->id());
    }
    shown.insert(groupId);
    if (!RailGroups::moveGroup(railGroupState(), groupId, step, shown))
        return;
    saveRailGroupState();
    applyRailOrder();
}

void MainWindow::assignRailItemToGroup(const QString &key, const QString &groupId)
{
    if (key.isEmpty())
        return;
    bool changed = false;
    if (!RailFolders::isFolderKey(key) &&
        !RailFolders::folderOf(railFolderState(), key).isEmpty() &&
        RailFolders::move(railFolderState(), key, QString())) {
        saveRailFolderState();
        changed = true;
    }
    if (RailGroups::assign(railGroupState(), key, groupId)) {
        saveRailGroupState();
        changed = true;
    }
    if (changed)
        applyRailOrder();
}

void MainWindow::adoptRailGroupsFromLayout()
{
    if (!m_appNavigationRailLayout || !railGrouped())
        return;
    RailGroups::State &groups = railGroupState();
    const RailFolders::State &folders = railFolderState();
    QString current;
    bool changed = false;
    for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
        QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
        if (!widget)
            continue;
        if (auto *separator = dynamic_cast<RailGroupSeparator *>(widget)) {
            current = separator->id();
            continue;
        }
        const QString key = widget->property("forkmeshRailKey").toString();
        if (key.isEmpty() || current.isEmpty() ||
            railEntryGroup(groups, folders, key) == current)
            continue;
        changed = RailGroups::assign(groups, key, current) || changed;
    }
    if (changed)
        saveRailGroupState();
}

void MainWindow::refreshRailGroupSeparators()
{
    if (!m_appNavigationRailLayout)
        return;
    QWidget *content = m_appNavigationRailLayout->parentWidget();
    RailGroupSeparator *separator = nullptr;
    bool any = false;
    const auto settle = [&] {
        if (separator)
            separator->setVisible(separator->isCustom() || any);
    };
    for (int i = 0; i < m_appNavigationRailLayout->count(); ++i) {
        QWidget *widget = m_appNavigationRailLayout->itemAt(i)->widget();
        if (!widget)
            continue;
        if (auto *next = dynamic_cast<RailGroupSeparator *>(widget)) {
            settle();
            separator = next;
            any = false;
            continue;
        }
        any = any || !content || widget->isVisibleTo(content);
    }
    settle();
}

void MainWindow::scheduleRailCountSort()
{
    if (railSortMode() != RailGroups::SortMode::Count)
        return;
    if (!m_railCountSortTimer) {
        m_railCountSortTimer = new QTimer(this);
        m_railCountSortTimer->setSingleShot(true);
        m_railCountSortTimer->setInterval(kRailCountSortDelayMs);
        connect(m_railCountSortTimer, &QTimer::timeout, this,
                [this] { applyRailOrder(); });
    }
    if (!m_railCountSortTimer->isActive())
        m_railCountSortTimer->start();
}

bool MainWindow::canPopOutRailItem(QWidget *item) const
{
    auto *button = qobject_cast<QAbstractButton *>(item);
    if (!button)
        return false;
    const QString key = button->property("forkmeshRailKey").toString();
    if (key.startsWith(QLatin1String("package:"))) {
        const QString id = key.mid(8);
        return std::any_of(m_nativePackagePages.cbegin(),
                           m_nativePackagePages.cend(),
                           [&id](const NativePackagePage &page) {
                               return page.id == id && page.widget;
                           });
    }
    if (m_navGroup && m_navGroup->id(button) > 0)
        return true;
    if (m_repoDetailTabs && m_repoDetailTabs->id(button) >= 0)
        return true;
    return button == m_railCodeButton || button == m_railGitButton ||
           button == m_agentsNavButton ||
           key.startsWith(QLatin1String("repoCode:"));
}

void MainWindow::popOutRailItemLater(QWidget *item, QWidget *spinner)
{
    QPointer<QWidget> guarded(item);
    QPointer<QWidget> guardedSpinner(spinner);
    QTimer::singleShot(spinner ? kRailPopOutSpinLeadMs : 0, this,
                       [this, guarded, guardedSpinner] {
                           QWidget *window =
                               guarded ? popOutRailItem(guarded) : nullptr;
                           if (guardedSpinner)
                               stopRailPopOutSpinnerWhenShown(guardedSpinner,
                                                              window);
                       });
}

QWidget *MainWindow::popOutRailItem(QWidget *item)
{
    if (!canPopOutRailItem(item) || !m_sectionStack)
        return nullptr;
    auto *button = qobject_cast<QAbstractButton *>(item);
    const QString key = button->property("forkmeshRailKey").toString();
    if (key.startsWith(QLatin1String("package:"))) {
        const QString id = key.mid(8);
        openNativePackageWindow(id);
        for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
            if (page.id == id && page.window) {
                placePoppedOutWindowAtCursor(page.window, this);
                return page.window;
            }
        }
        return nullptr;
    }
    QString title = button->accessibleName();
    if (title.isEmpty())
        title = button->text();
    title = title.section(QStringLiteral(" ("), 0, 0).remove(QLatin1Char('&')).trimmed();
    if (title.isEmpty())
        title = key;

    button->click();
    const int section = m_sectionStack->currentIndex();
    const int navId = m_navGroup ? m_navGroup->id(button) : -1;
    if (navId > 0) {
        if (section != navId)
            return nullptr;
        popOutStackPage(m_sectionStack, section, key, title);
    } else {
        if (section != 0 || !m_repoDetailStack)
            return nullptr;
        const int tab = m_repoDetailStack->currentIndex();
        const int tabId = m_repoDetailTabs ? m_repoDetailTabs->id(button) : -1;
        if (tabId >= 0 && tabId != tab)
            return nullptr;
        popOutStackPage(m_repoDetailStack, tab, key, title);
    }
    for (const PoppedOutPage &out : std::as_const(m_poppedOutPages)) {
        if (out.key == key)
            return out.window;
    }
    return nullptr;
}

void MainWindow::attachPageDragOut(QAbstractButton *button,
                                   std::function<void()> popOut)
{
    if (button)
        new ButtonDragOutPopOut(button, this, std::move(popOut));
}

void MainWindow::popOutLogPage()
{
    if (!m_sectionStack)
        return;
    ensureSectionBuilt(4);
    popOutStackPage(m_sectionStack, 4, QStringLiteral("log"),
                    QStringLiteral("Log"));
    catchUpLogPage();
}

void MainWindow::popOutStackPage(QStackedWidget *stack, int index,
                                 const QString &key, const QString &title)
{
    QWidget *page = stack ? stack->widget(index) : nullptr;
    if (!page)
        return;
    for (const PoppedOutPage &out : std::as_const(m_poppedOutPages)) {
        if (out.placeholder == page && out.window) {
            bringPoppedOutWindowForward(out.window);
            return;
        }
    }
    if (page->property("forkmeshDeferredSection").toBool() ||
        page->property("forkmeshDeferredRepoTab").toBool())
        return;
    const bool wasCurrent = stack->currentWidget() == page;
    const QSize size = page->size();

    auto *placeholder = new QWidget;
    placeholder->setObjectName(QStringLiteral("poppedOutPagePlaceholder"));
    placeholder->setProperty("forkmeshPoppedOutKey", key);
    QPointer<QWidget> guardedPlaceholder(placeholder);
    auto *placeholderLayout = new QVBoxLayout(placeholder);
    placeholderLayout->addStretch(1);
    auto *message = new QLabel(
        QStringLiteral("%1 is open in its own window.").arg(title), placeholder);
    message->setAlignment(Qt::AlignCenter);
    placeholderLayout->addWidget(message);
    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto *showWindow = new QPushButton(QStringLiteral("Show window"), placeholder);
    showWindow->setObjectName(QStringLiteral("poppedOutPageShowWindow"));
    showWindow->setCursor(Qt::PointingHandCursor);
    connect(showWindow, &QPushButton::clicked, this, [this, guardedPlaceholder] {
        for (const PoppedOutPage &out : std::as_const(m_poppedOutPages)) {
            if (out.placeholder == guardedPlaceholder && out.window)
                bringPoppedOutWindowForward(out.window);
        }
    });
    buttons->addWidget(showWindow);
    auto *returnHere =
        new QPushButton(QStringLiteral("Return to main window"), placeholder);
    returnHere->setObjectName(QStringLiteral("poppedOutPageReturn"));
    returnHere->setCursor(Qt::PointingHandCursor);
    auto dock = [this, guardedPlaceholder] {
        for (int i = 0; i < m_poppedOutPages.size(); ++i) {
            if (m_poppedOutPages.at(i).placeholder == guardedPlaceholder) {
                dockPoppedOutPage(i);
                return;
            }
        }
    };
    connect(returnHere, &QPushButton::clicked, this, dock);
    buttons->addWidget(returnHere);
    buttons->addStretch(1);
    placeholderLayout->addLayout(buttons);
    placeholderLayout->addStretch(1);

    stack->insertWidget(index, placeholder);
    if (wasCurrent)
        stack->setCurrentWidget(placeholder);
    stack->removeWidget(page);
    const QVariant policy = page->property("forkmeshStackPagePolicy");
    if (policy.isValid())
        page->setSizePolicy(policy.value<QSizePolicy>());

    auto *window = new PoppedOutPageWindow(this, dock);
    window->setObjectName(QStringLiteral("poppedOutPageWindow"));
    window->setProperty("forkmeshPoppedOutKey", key);
    window->setAttribute(Qt::WA_StyledBackground, true);
    window->setWindowTitle(QStringLiteral("%1 - ForkMesh").arg(title));
    auto *windowLayout = new QVBoxLayout(window);
    windowLayout->setContentsMargins(0, 0, 0, 0);
    windowLayout->addWidget(page);
    page->show();
    if (!window->restoreGeometry(
            QSettings().value(poppedOutPageGeometryKey(key)).toByteArray()))
        window->resize(size.isValid() && !size.isEmpty() ? size
                                                         : QSize(1100, 760));
    placePoppedOutWindowAtCursor(window, this);
    m_poppedOutPages.append({stack, page, placeholder, window, key});
    if (!stack->property("forkmeshPopOutHooked").toBool()) {
        stack->setProperty("forkmeshPopOutHooked", true);
        connect(stack, &QStackedWidget::currentChanged, this, [this] {
            QTimer::singleShot(0, this, [this] { raisePoppedOutPages(); });
        });
    }
    bringPoppedOutWindowForward(window);
}

void MainWindow::dockPoppedOutPage(int at)
{
    if (at < 0 || at >= m_poppedOutPages.size())
        return;
    const PoppedOutPage out = m_poppedOutPages.takeAt(at);
    if (out.window) {
        QSettings().setValue(poppedOutPageGeometryKey(out.key),
                             out.window->saveGeometry());
        if (out.page && out.window->layout())
            out.window->layout()->removeWidget(out.page);
    }
    const int index =
        out.stack && out.placeholder ? out.stack->indexOf(out.placeholder) : -1;
    if (out.page && index >= 0) {
        const bool wasCurrent = out.stack->currentWidget() == out.placeholder;
        out.stack->insertWidget(index, out.page);
        if (wasCurrent)
            out.stack->setCurrentWidget(out.page);
    }
    if (out.placeholder) {
        if (out.stack)
            out.stack->removeWidget(out.placeholder);
        out.placeholder->deleteLater();
    }
    if (out.window) {
        out.window->hide();
        out.window->deleteLater();
    }
}

void MainWindow::dockPoppedOutPages()
{
    while (!m_poppedOutPages.isEmpty())
        dockPoppedOutPage(0);
}

void MainWindow::raisePoppedOutPages()
{
    for (const PoppedOutPage &out : std::as_const(m_poppedOutPages)) {
        if (!out.stack || !out.window ||
            out.stack->currentWidget() != out.placeholder)
            continue;
        if (out.stack != m_sectionStack && m_sectionStack &&
            m_sectionStack->currentIndex() != 0)
            continue;
        bringPoppedOutWindowForward(out.window);
    }
}

void MainWindow::placeMovableButtons()
{
    if (!m_appNavigationRailLayout)
        return;
    const QVariantMap placement =
        QSettings().value(kButtonPlacementSetting).toMap();
    const auto place = [&](const QString &key, QWidget *widget,
                           const QString &home) {
        if (!widget || key.isEmpty())
            return;
        widget->setProperty("forkmeshButtonKey", key);
        widget->setProperty("forkmeshButtonHome", home);
        attachButtonPlacementMenu(widget);
        const QString away =
            home == kButtonPanelRail ? kButtonPanelRepoTabs : home;
        QBoxLayout *awayLayout = buttonPanelLayout(away);
        QString panel = placement.value(key, home).toString();
        if (panel != kButtonPanelRail && panel != away)
            panel = home;
        if (!awayLayout)
            panel = kButtonPanelRail;
        const bool toRail = panel == kButtonPanelRail;
        QBoxLayout *rail = m_appNavigationRailLayout;
        QBoxLayout *target = toRail ? rail : awayLayout;
        QBoxLayout *source = toRail ? awayLayout : rail;
        const bool placed = toRail ? isInRail(widget)
                                   : target->indexOf(widget) >= 0;
        if (!placed) {
            if (source)
                source->removeWidget(widget);
            target->addWidget(widget, 0, toRail ? Qt::AlignLeft : Qt::AlignTop);
        }
        if (auto *tile = dynamic_cast<VerticalIconButton *>(widget)) {
            tile->setInRail(toRail);
            if (toRail) {
                tile->setFixedWidth(railColumnWidth());
                tile->setProperty("forkmeshRailFitted", true);
            } else if (tile->property("forkmeshRailFitted").toBool()) {
                tile->setMinimumWidth(0);
                tile->setMaximumWidth(QWIDGETSIZE_MAX);
                tile->setProperty("forkmeshRailFitted", false);
            }
        }
    };
    for (const auto &[key, widget] : railHomeButtons()) {
        if (widget && (isInRail(widget) ||
                       (m_repoTabRowLayout &&
                        m_repoTabRowLayout->indexOf(widget) >= 0)))
            place(key, widget, kButtonPanelRail);
    }
    for (const auto &[key, widget] : m_repoTabRowTabs + m_repoTabRowActions)
        place(key, widget.data(), kButtonPanelRepoTabs);
    for (const auto &[key, widget] : m_repoCodeBarButtons)
        place(key, widget.data(), kButtonPanelRepoCode);
    applyRepoCodeBarOrder();
}

QBoxLayout *MainWindow::buttonPanelLayout(const QString &panel) const
{
    if (panel == kButtonPanelRail)
        return m_appNavigationRailLayout;
    if (panel == kButtonPanelRepoTabs)
        return m_repoTabRowLayout;
    if (panel == kButtonPanelRepoCode)
        return m_repoCodeBarLayout;
    return nullptr;
}

void MainWindow::applyRepoCodeBarOrder()
{
    QBoxLayout *row = m_repoCodeBarLayout;
    if (!row)
        return;
    QList<QWidget *> order;
    for (const auto &entry : std::as_const(m_repoCodeBarButtons)) {
        if (entry.second && row->indexOf(entry.second) >= 0)
            order.append(entry.second.data());
    }
    const bool gapAfterOverview =
        order.size() > 1 && order.first() == m_filesModeOverviewButton;
    bool unchanged = row->count() == order.size() + (gapAfterOverview ? 1 : 0);
    for (int i = 0, widgetAt = 0; unchanged && i < row->count(); ++i) {
        QLayoutItem *item = row->itemAt(i);
        if (gapAfterOverview && i == 1)
            unchanged = item->spacerItem() != nullptr;
        else
            unchanged = item->widget() == order.at(widgetAt++);
    }
    if (unchanged)
        return;
    while (QLayoutItem *item = row->takeAt(0))
        delete item;
    for (int i = 0; i < order.size(); ++i) {
        row->addWidget(order.at(i));
        if (i == 0 && gapAfterOverview)
            row->addSpacing(10);
    }
}

void MainWindow::applyRepoTabRowOrder()
{
    QBoxLayout *row = m_repoTabRowLayout;
    if (!row)
        return;
    QList<QWidget *> order;
    const auto take = [&](QWidget *widget) {
        if (widget && row->indexOf(widget) >= 0 && !order.contains(widget))
            order.append(widget);
    };
    for (const auto &entry : std::as_const(m_repoTabRowTabs))
        take(entry.second.data());
    for (const auto &entry : railHomeButtons())
        take(entry.second);
    QList<QWidget *> actions;
    for (const auto &entry : std::as_const(m_repoTabRowActions)) {
        if (entry.second && row->indexOf(entry.second) >= 0)
            actions.append(entry.second.data());
    }
    for (int i = 0; i < row->count(); ++i) {
        QWidget *widget = row->itemAt(i)->widget();
        if (widget && !actions.contains(widget))
            take(widget);
    }
    const int stretchAt = order.size();
    order += actions;

    bool unchanged = row->count() == order.size() + 1 &&
                     row->itemAt(stretchAt)->spacerItem();
    for (int i = 0, widgetAt = 0; unchanged && i < row->count(); ++i) {
        if (i == stretchAt)
            continue;
        unchanged = row->itemAt(i)->widget() == order.at(widgetAt++);
    }
    if (unchanged)
        return;

    while (QLayoutItem *item = row->takeAt(0))
        delete item;
    for (int i = 0; i < order.size(); ++i) {
        if (i == stretchAt)
            row->addStretch();
        row->addWidget(order.at(i), 0, Qt::AlignTop);
    }
    if (stretchAt == order.size())
        row->addStretch();
}

void MainWindow::moveButtonToPanel(const QString &key, const QString &panel)
{
    QString home;
    for (const auto &entry : railHomeButtons()) {
        if (entry.first == key)
            home = kButtonPanelRail;
    }
    for (const auto &entry : m_repoTabRowTabs + m_repoTabRowActions) {
        if (entry.first == key)
            home = kButtonPanelRepoTabs;
    }
    for (const auto &entry : std::as_const(m_repoCodeBarButtons)) {
        if (entry.first == key)
            home = kButtonPanelRepoCode;
    }
    if (home.isEmpty())
        return;
    QSettings settings;
    QVariantMap placement = settings.value(kButtonPlacementSetting).toMap();
    if (panel == home)
        placement.remove(key);
    else
        placement.insert(key, panel);
    if (placement.isEmpty())
        settings.remove(kButtonPlacementSetting);
    else
        settings.setValue(kButtonPlacementSetting, placement);
    applyRailOrder();
    applyRailCaptionPreference();
    updateRepoActivityRail();
}

void MainWindow::resetButtonPlacement()
{
    QSettings().remove(kButtonPlacementSetting);
    QSettings().remove(kRailOrderSetting);
    applyRailOrder();
    applyRailCaptionPreference();
    updateRepoActivityRail();
}

void MainWindow::attachButtonPlacementMenu(QWidget *button)
{
    if (!button || button->property("forkmeshPlacementMenu").toBool())
        return;
    button->setProperty("forkmeshPlacementMenu", true);
    if (button->contextMenuPolicy() == Qt::CustomContextMenu)
        return;
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(button, &QWidget::customContextMenuRequested, this,
            [this, button](const QPoint &position) {
                auto *menu = new QMenu(button);
                menu->setAttribute(Qt::WA_DeleteOnClose);
                addButtonPlacementActions(menu, button);
                if (menu->isEmpty()) {
                    menu->deleteLater();
                    return;
                }
                menu->popup(button->mapToGlobal(position));
            });
}

void MainWindow::addButtonPlacementActions(QMenu *menu, QWidget *button)
{
    if (!menu || !button)
        return;
    const QString key = button->property("forkmeshButtonKey").toString();
    if (key.isEmpty())
        return;
    const bool inRail = isInRail(button);
    if (!menu->isEmpty())
        menu->addSeparator();
    const QString home = button->property("forkmeshButtonHome").toString();
    const QString away = home == kButtonPanelRepoCode ? kButtonPanelRepoCode
                                                      : kButtonPanelRepoTabs;
    const QString panel = inRail ? away : kButtonPanelRail;
    QAction *move = menu->addAction(
        !inRail                        ? QStringLiteral("Move to left rail")
        : away == kButtonPanelRepoCode ? QStringLiteral("Move to Code toolbar")
                                       : QStringLiteral("Move to repository tab bar"));
    move->setEnabled(buttonPanelLayout(panel) != nullptr);
    connect(move, &QAction::triggered, this, [this, key, panel] {
        QTimer::singleShot(0, this,
                           [this, key, panel] { moveButtonToPanel(key, panel); });
    });
    if (!QSettings().value(kButtonPlacementSetting).toMap().isEmpty() ||
        QSettings().contains(kRailOrderSetting)) {
        QAction *reset = menu->addAction(QStringLiteral("Reset button placement"));
        connect(reset, &QAction::triggered, this, [this] {
            QTimer::singleShot(0, this, [this] { resetButtonPlacement(); });
        });
    }
}

// The top-bar "Log in / Sign up" pill replaces the retired first-run screen
// so it must be honest about state rather than eager: it stays
// hidden until silent auth has actually resolved who this machine is.
// Silent auth runs a few seconds after launch and is what fills
// nodeOwnerDisplayName() on a signed-in machine — offering "Log in" before then
// would flash the pill on every start for a user who is already logged in. A
// headless mirror authenticates with its node key and has nobody at the
// keyboard, so it never gets the pill at all.
void MainWindow::updateSignInButton()
{
    if (!m_navSignInButton)
        return;
    const bool signedIn = hasActiveAccountSession() ||
                         !nodeOwnerDisplayName().trimmed().isEmpty();
    m_navSignInButton->setVisible(m_startupAuthResolved && !signedIn);
}

void MainWindow::showSignInMenu()
{
    if (Theme::isNext()) {
        forkmesh::ui::NextAccess::openSignIn(*this);
        return;
    }
    QMenu menu(this);
    QAction *login = menu.addAction(QString::fromUtf8("Log in to your account\xE2\x80\xA6"));
    QAction *browser = nullptr;
    if (!accountOwner().isEmpty() && hasOwnerSigningCapability(accountOwner()))
        browser = menu.addAction(
            QString::fromUtf8("Link this node in your browser\xE2\x80\xA6"));
    QAction *signup = menu.addAction(QString::fromUtf8("Create an account\xE2\x80\xA6"));

    QAction *chosen = menu.exec(topBarMenuAnchor(m_navSignInButton));
    if (!chosen)
        return;
    if (chosen == login) {
        loginToUserAccount();
        updateUserSwitcher();
        return;
    }
    if (browser && chosen == browser) {
        openLinkNodeInBrowser();
        return;
    }
    if (chosen == signup) {
        QUrl url = catalogApiUrl(); // http(s) on the mainnode host
        url.setPath(QStringLiteral("/signup"));
        QDesktopServices::openUrl(url);
        logSystem("Account: opened the browser to create a ForkMesh account.");
    }
}

void MainWindow::positionFloatingLogButton()
{
    if (!m_floatingLogButton || !m_footerUpdateLog)
        return;
    m_floatingLogButton->adjustSize();
    constexpr int kMargin = 8;
    const QSize sz = m_floatingLogButton->size();
    int scrollbarW = 0;
    if (QScrollBar *sb = m_footerUpdateLog->verticalScrollBar(); sb && sb->isVisible())
        scrollbarW = sb->width();
    m_floatingLogButton->move(
        m_footerUpdateLog->width() - sz.width() - kMargin - scrollbarW,
        m_footerUpdateLog->height() - sz.height() - kMargin);
    m_floatingLogButton->raise();
}

void MainWindow::positionFooterLogPauseButton()
{
    if (!m_footerLogPauseButton || !m_footerUpdateLog)
        return;
    constexpr int kMargin = 3;
    const QSize sz = m_footerLogPauseButton->size();
    int scrollbarW = 0;
    if (QScrollBar *sb = m_footerUpdateLog->verticalScrollBar(); sb && sb->isVisible())
        scrollbarW = sb->width();
    m_footerLogPauseButton->move(
        qMax(0, m_footerUpdateLog->width() - sz.width() - kMargin - scrollbarW),
        qMax(0, m_footerUpdateLog->height() - sz.height() - kMargin));
    m_footerLogPauseButton->raise();
}

void MainWindow::updateFooterLogPauseButton()
{
    if (!m_footerLogPauseButton)
        return;
    const bool paused = m_footerLogScrollPaused;
    m_footerLogPauseButton->setIcon(style()->standardIcon(
        paused ? QStyle::SP_ArrowDown : QStyle::SP_MediaPause));
    m_footerLogPauseButton->setToolTip(
        paused ? QString::fromUtf8(
                     "Auto-scroll paused \xE2\x80\x94 click to follow new log "
                     "lines again")
               : QString::fromUtf8(
                     "Following new log lines \xE2\x80\x94 click to pause "
                     "auto-scroll"));
}

void MainWindow::updateConnectionStatus()
{
    if (!m_connectionDot)
        return;

    bool selfOnline = false;
    int onlineCount = 0;
    for (const MemberInfo &member : std::as_const(m_homeRoster)) {
        if (member.online)
            ++onlineCount;
        if (member.self && member.online)
            selfOnline = true;
    }
    const bool connected = m_backend && selfOnline;
    syncDesktopOfflineDots(!connected);

    QString color, text;
    if (connected) {
        color = "#3fb950"; // green
        text = QString::fromUtf8("Connected \xC2\xB7 %1 %2 online")
                   .arg(onlineCount)
                   .arg(onlineCount == 1 ? "node" : "nodes");
    } else if (m_backend && !projectInstancePending()) {
        color = "#d29922"; // amber: connecting / backing off
        text = QString::fromUtf8("Connecting\xE2\x80\xA6");
    } else {
        color = "#8b949e"; // grey: offline / not started
        text = projectInstancePending() && !m_offlineMode
                   ? QStringLiteral("Online, no instance chosen")
                   : QStringLiteral("Offline");
    }
    m_connectionDot->setToolTip(text);
    if (color == m_connectionStatusColor)
        return;
    m_connectionStatusColor = color;
    m_connectionDot->setStyleSheet(Theme::remapSheet(
        QStringLiteral("background:%1; border-radius:5px;").arg(color)));
}

void MainWindow::updateAdminCrownBadge()
{
    if (!m_isAdmin)
        renderUsersSignupReferralCode(QJsonObject());
    if (!m_isAdmin && !m_usersQuarantinePayload.isEmpty()) {
        // Quarantine rows contain preserved investigation evidence that must
        // not survive an administrator signing out on a shared desktop.
        m_usersQuarantinePayload = QJsonArray();
        renderUsersQuarantinePage(m_usersQuarantinePayload);
    }
    if (!m_isAdmin && m_usersDirectoryAdminPayload) {
        // A shared desktop that signs out of an administrator account must not
        // keep either its protected fields or its private/non-active rows.
        // mergeChatUserDirectory retained the separately scrubbed public
        // projection specifically for this shared-desktop transition.
        m_usersDirectoryPayload = m_usersPublicDirectoryPayload;
        m_usersDirectoryAdminPayload = false;
        m_chatDirectoryFetchedMs = 0;
        renderUsersPage(m_usersDirectoryPayload);
    }
    if (m_adminCrownBadge) {
        m_adminCrownBadge->setVisible(m_isAdmin);
        m_adminCrownBadge->setToolTip(
            m_isAdmin ? QStringLiteral("Admin") : QString());
    }
    if (m_usersNavButton)
        m_usersNavButton->setVisible(m_isAdmin);
    updateAddCoreNodeButton();
    refreshUsersQuarantineButton();
    refreshUsersUnquarantineButton();
    refreshUsersDeleteButton();
    if (!m_isAdmin && m_sectionStack &&
        m_sectionStack->currentIndex() == kUsersSectionIndex)
        showSection(kUsersSectionIndex);
}

// Which offline mode a new session opens in. A reader who switched the network
// off by hand meant it, and that choice is kept -- but the connectivity watch's
// automatic offline mode described the *last* session's relay, and restoring it
// opened the app with every request blocked until something happened to probe
// again (issue #712). A fresh session therefore starts online and lets the
// watch re-decide; it needs three failed probes to switch itself back on.
// State only: the constructor hands the result to the shared network manager
// and the footer switch, which do not exist yet for every caller.
void MainWindow::restorePersistedOfflineMode()
{
    QSettings settings;
    const bool savedOffline =
        settings.value(kOfflineModeSetting, false).toBool();
    m_offlineModeWasAutomaticLastSession =
        savedOffline && settings.value(kOfflineModeAutoSetting, false).toBool();
    m_offlineMode = savedOffline && !m_offlineModeWasAutomaticLastSession;
    m_offlineModeAuto = false;
    // The stored state is what the footer switch and the connectivity watch
    // both read, so leave it agreeing with the mode this session actually runs
    // in rather than with the session that stored it.
    if (m_offlineModeWasAutomaticLastSession) {
        settings.setValue(kOfflineModeSetting, false);
        settings.setValue(kOfflineModeAutoSetting, false);
    }
    m_offlineModeAnnouncementPending = m_offlineMode;
}

// A kept offline mode is silent otherwise: the app comes up with every relay
// panel failing, which reads as a broken install rather than as a switch left
// on days ago (issue #712). One inline notice on the page names the switch,
// and its Go online button goes back online.
void MainWindow::announceRestoredOfflineMode()
{
    if (!m_offlineModeAnnouncementPending)
        return;
    m_offlineModeAnnouncementPending = false;
    if (!m_offlineMode)
        return; // turned off again before the notice could be shown
    showPageNotice(
        QStringLiteral("Offline mode is still on from your last session, so no "
                       "internet requests are being sent. Use Go online to go "
                       "back online, or the switch beside the footer version."),
        false, QStringLiteral("fm:offline:off"));
}

void MainWindow::setOfflineMode(bool offline, bool automatic)
{
    automatic = offline && automatic;
    if (offline == m_offlineMode && automatic == m_offlineModeAuto) {
        updateOfflineModeControls();
        return;
    }

    const bool wasOffline = m_offlineMode;
    const bool wasAutomatic = m_offlineModeAuto;
    m_offlineMode = offline;
    m_offlineModeAuto = automatic;
    QSettings().setValue(kOfflineModeSetting, offline);
    QSettings().setValue(kOfflineModeAutoSetting, automatic);

    if (auto *network =
            qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess)) {
        network->setOfflineMode(offline);
    }

    const auto *networkInfo = QNetworkInformation::instance();
    const bool systemDisconnected =
        networkInfo && networkInfo->reachability() ==
                           QNetworkInformation::Reachability::Disconnected;
    if (m_backend)
        m_backend->setNetworkAvailable(!offline && !systemDisconnected &&
                                       !projectInstancePending());

    if (offline) {
        if (m_connectedAtMs > 0) {
            m_totalConnectionMs +=
                QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs;
            m_connectedAtMs = 0;
            QSettings().setValue(kConnectionTotalSetting, m_totalConnectionMs);
        }
        if (m_heartbeatTimer)
            m_heartbeatTimer->stop();
        if (m_nodeHeartbeatAckTimer)
            m_nodeHeartbeatAckTimer->stop();
        m_nodeHeartbeatSendPending = false;
        finishNodeHeartbeatDelivery();
        stopNodeEventSocket();
        startOfficeChannelMirror();
        if (!wasOffline) {
            logSystem(automatic
                          ? QString::fromUtf8("No relay connection \xE2\x80\x94 offline mode "
                                           "enabled automatically.")
                          : QString::fromUtf8("Offline mode enabled \xE2\x80\x94 network "
                                           "requests are paused."));
        }
    } else {
        m_offlineProbeFailures = 0;
        startNodePresence();
        if (wasOffline) {
            logSystem(wasAutomatic
                          ? QString::fromUtf8("Relay connection restored \xE2\x80\x94 leaving "
                                           "automatic offline mode.")
                          : QString::fromUtf8("Offline mode disabled \xE2\x80\x94 reconnecting."));
        }
        probeRelayLatency();
    }

    updateOfflineModeControls();
    updateNodeOnlineControls();
    updateConnectionStatus();
}

void MainWindow::updateOfflineModeControls()
{
    const QString description =
        !m_offlineMode
            ? QStringLiteral("Offline mode is off. Activate to pause internet "
                             "requests.")
            : m_offlineModeAuto
                  ? QStringLiteral("Offline mode is on automatically because the "
                                   "relay could not be reached. It will turn off "
                                   "when the connection returns.")
                  : QStringLiteral("Offline mode is on. Deactivate to resume "
                                   "internet requests.");
    if (m_statusOfflineToggle) {
        const QSignalBlocker blocker(m_statusOfflineToggle);
        m_statusOfflineToggle->setChecked(m_offlineMode);
        m_statusOfflineToggle->setToolTip(description);
        m_statusOfflineToggle->setAccessibleDescription(description);
    }
    if (m_statusOfflineLabel) {
        const QString onText = QStringLiteral("Offline mode: On");
        const QString offText = QStringLiteral("Offline mode: Off");
        m_statusOfflineLabel->setText(m_offlineMode ? onText : offText);
        m_statusOfflineLabel->ensurePolished();
        const QFontMetrics metrics = m_statusOfflineLabel->fontMetrics();
        m_statusOfflineLabel->setMinimumWidth(
            qMax(metrics.horizontalAdvance(onText),
                 metrics.horizontalAdvance(offText)));
        m_statusOfflineLabel->setToolTip(description);
        m_statusOfflineLabel->setAccessibleDescription(description);
    }
}

void MainWindow::startNodePresence()
{
    const auto *networkInfo = QNetworkInformation::instance();
    const bool systemDisconnected =
        networkInfo && networkInfo->reachability() ==
                           QNetworkInformation::Reachability::Disconnected;
    if (m_nodeOffline || m_offlineMode || systemDisconnected || !m_backend ||
        projectInstancePending() || !hasOwnerSigningCapability()) {
        return;
    }
    if (m_connectedAtMs <= 0)
        m_connectedAtMs = QDateTime::currentMSecsSinceEpoch();
    // The relay's own repository list decides what this machine serves to it,
    // so pull it before presence announces anything.
    refreshRelayRepositoryList();
    startNodeEventSocket();
    if (!m_heartbeatTimer) {
        m_heartbeatTimer = new QTimer(this);
        m_heartbeatTimer->setInterval(60000);
        connect(m_heartbeatTimer, &QTimer::timeout, this,
                &MainWindow::sendNodeHeartbeat);
    }
    m_heartbeatTimer->start();
    sendNodeHeartbeat();
    startPowerAlertWatch();
}

void MainWindow::noteRelayReachability(bool reachable)
{
    if (reachable) {
        m_offlineProbeFailures = 0;
        if (m_offlineMode && m_offlineModeAuto)
            setOfflineMode(false);
        return;
    }

    // A build the version gate retired is not offline, it is blocked, and its
    // refused probes must not turn that into an automatic offline mode whose
    // card tells the reader to check a connection that is fine.
    if (auto *network =
            qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess)) {
        if (network->blockedByClientVersion())
            return;
    }
    // A manual choice remains manual; failed health checks must not replace it
    // with an automatic state that turns itself back off later.
    if (m_offlineMode)
        return;
    if (++m_offlineProbeFailures >= kOfflineProbeFailureLimit)
        setOfflineMode(true, /*automatic=*/true);
}

void MainWindow::setNodeOffline(bool offline)
{
    if (offline == m_nodeOffline) {
        updateNodeOnlineControls();
        return;
    }
    m_nodeOffline = offline;
    QSettings().setValue(kNodeOfflineSetting, offline);

    if (offline) {
        if (m_connectedAtMs > 0) {
            m_totalConnectionMs +=
                QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs;
            m_connectedAtMs = 0;
            QSettings().setValue(kConnectionTotalSetting, m_totalConnectionMs);
        }
        if (m_heartbeatTimer)
            m_heartbeatTimer->stop();
        stopNodeEventSocket();
        logSystem("Node taken offline \xE2\x80\x94 no longer serving repos or "
                  "publishing reward-eligibility signals.");
    } else {
        startNodePresence();
        logSystem(m_offlineMode
                      ? QString::fromUtf8("Node marked online \xE2\x80\x94 it will reconnect "
                                       "when offline mode is disabled.")
                      : QString::fromUtf8("Node back online \xE2\x80\x94 serving repos and "
                                       "publishing reward-eligibility signals; selection is "
                                       "not guaranteed."));
    }
    updateNodeOnlineControls();
    updateConnectionStatus();
}

void MainWindow::updateNodeOnlineControls()
{
    if (!m_nodeOnlineToggle)
        return;
    const bool online = !m_nodeOffline;
    if (m_nodeOnlineToggle->isChecked() != online)
        m_nodeOnlineToggle->setChecked(online);
    m_nodeOnlineToggle->setToolTip(
        online ? QStringLiteral(
                     "This machine is online and may be considered by community "
                     "reward policies; selection is not guaranteed. Click to "
                     "take it offline.")
               : QStringLiteral(
                     "This machine is offline and is not publishing reward-"
                     "eligibility signals. Click to bring it back online."));

    if (m_nodeOnlineStatusLabel) {
        m_nodeOnlineStatusLabel->setText(online ? QStringLiteral("Online")
                                                : QStringLiteral("Offline"));
        const QString sheet =
            online ? QStringLiteral("color:#3fb950; font-size:13px; font-weight:800;")
                   : QStringLiteral("color:#d29922; font-size:13px; font-weight:800;");
        if (m_nodeOnlineStatusLabel->styleSheet() != sheet)
            m_nodeOnlineStatusLabel->setStyleSheet(Theme::remapSheet(sheet));
    }

    if (m_nodeRewardStatus) {
        m_nodeRewardStatus->setText(online
                                        ? QString::fromUtf8(
                                              "may be eligible \xC2\xB7 selection "
                                              "not guaranteed")
                                        : QString::fromUtf8(
                                              "offline \xC2\xB7 not publishing "
                                              "eligibility"));
        const QString sheet =
            online ? QStringLiteral("color:#3fb950; font-size:10px; font-weight:700;")
                   : QStringLiteral("color:#d29922; font-size:10px; font-weight:700;");
        if (m_nodeRewardStatus->styleSheet() != sheet)
            m_nodeRewardStatus->setStyleSheet(Theme::remapSheet(sheet));
    }

    if (m_nodeUptimeLabel) {
        const qint64 sessionMs =
            m_connectedAtMs > 0
                ? QDateTime::currentMSecsSinceEpoch() - m_connectedAtMs
                : 0;
        m_nodeUptimeLabel->setText(
            !online ? QStringLiteral("offline")
            : sessionMs > 0
                ? QStringLiteral("online %1").arg(formatDuration(sessionMs))
                : QString::fromUtf8("connecting\xE2\x80\xA6"));
    }
}

void MainWindow::updateBreadcrumb()
{
    updateRelaySwitcher();
    refreshRepoSyncIndicators();
    if (!m_breadcrumb)
        return;
    m_breadcrumb->clear();
    m_breadcrumb->hide();
}

void MainWindow::updateRelaySwitcher()
{
    if (!m_relayMenuButton)
        return;
    updateNetworkCounts(m_servers.size(), -1, -1);
    QString host;
    QString instanceName;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size()) {
        host = serverHost(m_servers.at(m_activeServer).url);
        instanceName = m_servers.at(m_activeServer).name.trimmed();
    }

    m_relayMenuButton->setIcon(
        (m_activeServer >= 0 && m_activeServer < m_servers.size())
            ? QIcon(faviconFor(m_servers.at(m_activeServer)))
            : QIcon(letterFavicon(host.isEmpty() ? QStringLiteral("ForkMesh")
                                                 : host)));
    if (instanceName.isEmpty())
        instanceName = host.isEmpty() ? QStringLiteral("ForkMesh") : host;
    if (projectInstancePending()) {
        instanceName = QStringLiteral("Choose instance");
        m_relayMenuButton->setIcon(QIcon(letterFavicon(QStringLiteral("?"))));
    }
    const QFontMetrics metrics(m_relayMenuButton->font());
    const int instanceButtonWidth =
        qBound(railItemWidth(), metrics.horizontalAdvance(instanceName) + 48,
               210);
    m_relayMenuButton->setFixedWidth(instanceButtonWidth);
    m_relayMenuButton->setText(
        metrics.elidedText(instanceName, Qt::ElideRight,
                           qMax(20, instanceButtonWidth - 44)));
    refreshRelayMenuTooltip();
}

void MainWindow::refreshRelayMenuTooltip()
{
    if (!m_relayMenuButton)
        return;
    QString host;
    QString instanceName;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size()) {
        host = serverHost(m_servers.at(m_activeServer).url);
        instanceName = m_servers.at(m_activeServer).name.trimmed();
    }
    if (host.isEmpty())
        host = QStringLiteral("ForkMesh");
    if (instanceName.isEmpty())
        instanceName = host;
    if (projectInstancePending()) {
        m_relayMenuButton->setToolTip(QStringLiteral(
            "This project is not connected to an instance yet. Click to choose one."));
        return;
    }
    m_relayMenuButton->setToolTip(
        QStringLiteral("%1 (%2) — active instance. %3 %4 configured.\n"
                       "Accounts, repositories and special agent replies are scoped to "
                       "this instance. Connection speed: %5")
            .arg(instanceName, host)
            .arg(m_servers.size())
            .arg(m_servers.size() == 1 ? QStringLiteral("relay")
                                       : QStringLiteral("relays"),
                 relaySpeedText(host)));
}

void MainWindow::setRelayLinkSpeed(const QString &host, int ms)
{
    const QString key = host.trimmed().toLower();
    if (!key.isEmpty()) {
        m_relayHostLatency.insert(
            key, RelayLatencySample{ms, QDateTime::currentMSecsSinceEpoch()});
    }
    QString activeHost;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size())
        activeHost = serverHost(m_servers.at(m_activeServer).url).toLower();
    if (!key.isEmpty() && !activeHost.isEmpty() && key != activeHost)
        return; // another relay's sample: the cache is all it feeds
    if (m_relaySpeedDot) {
        if (ms < 0)
            m_relaySpeedDot->setUnreachable();
        else
            m_relaySpeedDot->setLatency(ms);
    }
    refreshRelayMenuTooltip();
}

QString MainWindow::relaySpeedText(const QString &host) const
{
    const RelayLatencySample sample =
        m_relayHostLatency.value(host.trimmed().toLower());
    if (sample.stampMs <= 0)
        return QString::fromUtf8("measuring\xE2\x80\xA6");
    if (sample.ms < 0)
        return QStringLiteral("no answer");
    return QStringLiteral("%1 ms").arg(sample.ms);
}

QString MainWindow::relayMenuEntryText(const QString &host) const
{
    return QString::fromUtf8("%1  \xC2\xB7  %2").arg(host, relaySpeedText(host));
}

void MainWindow::onRelayLatencySampled(int ms)
{
    m_lastWsLatencySampleMs = QDateTime::currentMSecsSinceEpoch();
    m_relayProbeFailures = 0;
    QString host;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size())
        host = serverHost(m_servers.at(m_activeServer).url);
    setRelayLinkSpeed(host, ms);
}

void MainWindow::probeRelayHostSpeed(const QString &serverUrl,
                                     std::function<void(int)> done)
{
    QUrl url(serverUrl);
    if (url.scheme() == "ws")
        url.setScheme(QStringLiteral("http"));
    else if (url.scheme() == "wss")
        url.setScheme(QStringLiteral("https"));
    url.setPath(QStringLiteral("/api/version"));
    url.setQuery(QString());
    url.setFragment(QString());

    const QString host = serverHost(serverUrl);
    if (!m_networkAccess || !url.isValid() || url.host().isEmpty()) {
        setRelayLinkSpeed(host, -1);
        if (done)
            done(-1);
        return;
    }
    if (m_relaySpeedProbes.contains(host))
        return;
    m_relaySpeedProbes.insert(host);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("accept", "application/json");
    request.setTransferTimeout(10000); // no answer within 10s counts as down

    auto *clock = new QElapsedTimer;
    clock->start();
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, clock, host, done = std::move(done)] {
                const qint64 elapsed = clock->elapsed();
                delete clock;
                reply->deleteLater();
                m_relaySpeedProbes.remove(host);
                const int ms = reply->error() == QNetworkReply::NoError
                                   ? static_cast<int>(elapsed)
                                   : -1;
                setRelayLinkSpeed(host, ms);
                if (done)
                    done(ms);
            });
}

void MainWindow::refreshNodeDotMatrix()
{
    if (!m_nodeDotMatrix)
        return;
    QVector<NodeDotMatrix::Dot> dots;
    dots.reserve(m_nodeDotEntries.size());
    int online = 0;
    int caution = 0;
    // Same verdict as the Nodes table (relay onlineNodes once fetched), plus
    // whatever the Mirrors panel just saw serving: the relay set is only read
    // while the Nodes tab is open, and e.online alone is the chat roster,
    // which headless mirrors never join -- the whole fleet painted grey.
    QVector<bool> live;
    live.reserve(m_nodeDotEntries.size());
    for (const NodeMenuEntry &e : std::as_const(m_nodeDotEntries))
        live.append(nodeEntryOnline(e) ||
                    m_nodeDotRepoStates.value(e.name.trimmed().toLower())
                        .online);
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < m_nodeDotEntries.size(); ++i) {
            const NodeMenuEntry &e = m_nodeDotEntries.at(i);
            const bool isOnline = live.at(i);
            if (isOnline != (pass == 0))
                continue;
            const NodeDotRepoState repo =
                m_nodeDotRepoStates.value(e.name.trimmed().toLower());
            NodeDotMatrix::Dot dot;
            dot.name = e.name;
            dot.self = e.self;
            // Colours match the Mirrors panel's name-column lights.
            if (repo.integrityFailing) {
                dot.color = QColor("#f85149"); // red: integrity failing
                ++caution;
            } else if (isOnline && repo.behind) {
                dot.color = QColor("#d29922"); // amber: out of sync
                ++caution;
            } else if (isOnline) {
                dot.color = QColor("#3fb950"); // green: serving
            } else {
                dot.color = Theme::remapColor(QColor("#484f58")); // grey: offline
            }
            if (isOnline)
                ++online;
            dots.append(dot);
        }
    }
    m_nodeDotMatrix->setDots(dots);
    m_nodeDotMatrix->setVisible(!dots.isEmpty() &&
                                !topBarIndicatorHidden(QStringLiteral("nodes")));
    updateChromeDotDivider();
    if (dots.isEmpty())
        return;

    QString tip = QString::fromUtf8("%1 node%2 on the network \xE2\x80\x94 "
                                 "%3 online, %4 offline")
                      .arg(dots.size())
                      .arg(dots.size() == 1 ? QString() : QStringLiteral("s"))
                      .arg(online)
                      .arg(dots.size() - online);
    if (caution > 0)
        tip += QStringLiteral(", %1 out of sync").arg(caution);
    const int shown = m_nodeDotMatrix->shownCount();
    if (shown < dots.size())
        tip += QStringLiteral("\n(showing the first %1)").arg(shown);
    tip += QStringLiteral("\nClick a dot to open that node.");
    m_nodeDotMatrix->setToolTip(tip);
}

void MainWindow::updateChromeDotDivider()
{
    const bool agents = m_agentDotMatrix && !m_agentDotMatrix->isHidden();
    const bool nodes = m_nodeDotMatrix && !m_nodeDotMatrix->isHidden();
    const bool actions = m_actionRunStrip && !m_actionRunStrip->isHidden();
    if (m_chromeDotDivider)
        m_chromeDotDivider->setVisible(agents && nodes);
    if (m_chromeActionDivider)
        m_chromeActionDivider->setVisible((agents || nodes) && actions);
}

bool MainWindow::topBarIndicatorHidden(const QString &key) const
{
    return m_hiddenTopBarIndicators.contains(key);
}

void MainWindow::setTopBarIndicatorHidden(const QString &key, bool hidden)
{
    if (topBarIndicatorHidden(key) == hidden)
        return;
    if (hidden)
        m_hiddenTopBarIndicators.insert(key);
    else
        m_hiddenTopBarIndicators.remove(key);
    QStringList saved(m_hiddenTopBarIndicators.cbegin(),
                      m_hiddenTopBarIndicators.cend());
    saved.sort();
    QSettings settings;
    if (saved.isEmpty())
        settings.remove(kHiddenTopBarIndicatorsSetting);
    else
        settings.setValue(kHiddenTopBarIndicatorsSetting, saved);
    refreshAgentDotMatrix();
    refreshNodeDotMatrix();
    refreshActionRunStrip();
}

void MainWindow::showTopBarIndicatorMenu(const QPoint &globalPos)
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSection(QStringLiteral("Show in top bar"));
    const QList<QPair<QString, QString>> indicators = {
        {QStringLiteral("agents"), QStringLiteral("Agents")},
        {QStringLiteral("nodes"), QStringLiteral("Nodes")},
        {QStringLiteral("actions"), QStringLiteral("Actions")},
    };
    for (const auto &indicator : indicators) {
        QAction *action = menu->addAction(indicator.second);
        action->setCheckable(true);
        action->setChecked(!topBarIndicatorHidden(indicator.first));
        connect(action, &QAction::toggled, this,
                [this, key = indicator.first](bool shown) {
                    setTopBarIndicatorHidden(key, !shown);
                });
    }
    menu->popup(globalPos);
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testTopBarIndicatorShown(const QString &key) const
{
    const QWidget *widget = nullptr;
    if (key == QLatin1String("agents"))
        widget = m_agentDotMatrix;
    else if (key == QLatin1String("nodes"))
        widget = m_nodeDotMatrix;
    else if (key == QLatin1String("actions"))
        widget = m_actionRunStrip;
    return widget && !widget->isHidden();
}
#endif

void MainWindow::setNodeDotRepoStates(
    const QHash<QString, NodeDotRepoState> &states)
{
    if (states.isEmpty() && m_nodeDotRepoStates.isEmpty())
        return;
    m_nodeDotRepoStates = states;
    refreshNodeDotMatrix();
}

void MainWindow::probeRelayLatency()
{
    // A reader-selected offline mode does not make even a background health
    // request. Automatic offline mode is different: its one marked probe is
    // how we discover that the connection has returned.
    if (!m_relaySpeedDot || !m_networkAccess || m_relayProbeInFlight ||
        projectInstancePending() || (m_offlineMode && !m_offlineModeAuto)) {
        return;
    }
    if (!(m_offlineMode && m_offlineModeAuto) &&
        QDateTime::currentMSecsSinceEpoch() - m_lastWsLatencySampleMs < 90 * 1000)
        return;

    QUrl url = catalogApiUrl(); // same relay host, http(s) scheme
    const QString host = url.host();
    if (!url.isValid() || host.isEmpty()) {
        setRelayLinkSpeed(host, -1);
        return;
    }
    url.setPath(QStringLiteral("/api/version"));
    url.setQuery(QString());

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(10000); // a no-answer within 10s counts as down
    BackoffNetworkAccessManager::markReachabilityProbe(request);

    m_relayProbeInFlight = true;
    auto *clock = new QElapsedTimer;
    clock->start();
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, clock, host] {
        const qint64 elapsed = clock->elapsed();
        delete clock;
        m_relayProbeInFlight = false;
        const QNetworkReply::NetworkError error = reply->error();
        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool relayResponded =
            error == QNetworkReply::NoError || httpStatus > 0;
        const bool connectivityFailure =
            !BackoffNetworkAccessManager::isBackoffSuppressed(reply) &&
            !BackoffNetworkAccessManager::isOfflineBlocked(reply) &&
            forkmesh::networkReplyLooksOffline(reply);
        if (relayResponded)
            noteRelayReachability(true);
        else if (connectivityFailure)
            noteRelayReachability(false);
        const QByteArray versionBody =
            error == QNetworkReply::NoError ? reply->readAll() : QByteArray();
        reply->deleteLater();
        if (error == QNetworkReply::NoError) {
            noteAppRelease(QJsonDocument::fromJson(versionBody).object());
            m_relayProbeFailures = 0;
            setRelayLinkSpeed(host, static_cast<int>(elapsed));
            if (elapsed >= 300) {
                const int steps = qMin(m_relayProbeElevated++, 6);
                const qint64 delayMs = qMin<qint64>(1000LL << steps, 60 * 1000);
                QTimer::singleShot(static_cast<int>(delayMs), this,
                                   &MainWindow::probeRelayLatency);
            } else {
                m_relayProbeElevated = 0;
            }
        } else {
            if (m_networkAccess)
                m_networkAccess->clearConnectionCache();
            const auto *netInfo = QNetworkInformation::instance();
            const bool osOffline =
                netInfo && netInfo->reachability() ==
                               QNetworkInformation::Reachability::Disconnected;
            if (++m_relayProbeFailures >= 2 || osOffline) {
                setRelayLinkSpeed(host, -1);
                const int backoffSteps = qMax(0, m_relayProbeFailures - 2);
                const qint64 delayMs =
                    qMin<qint64>(3000LL << qMin(backoffSteps, 10),
                                 m_offlineMode && m_offlineModeAuto
                                     ? kAutoOfflineProbeMaxDelayMs
                                     : 5 * 60 * 1000);
                QTimer::singleShot(delayMs, this, &MainWindow::probeRelayLatency);
            } else {
                QTimer::singleShot(2500, this, &MainWindow::probeRelayLatency);
            }
        }
    });
}

void MainWindow::initRelayReachabilityWatch()
{
    if (!QNetworkInformation::loadBackendByFeatures(
            QNetworkInformation::Feature::Reachability))
        return;
    auto *networkInfo = QNetworkInformation::instance();
    if (!networkInfo)
        return;

    const auto applyReachability =
        [this](QNetworkInformation::Reachability reachability) {
            if (reachability ==
                QNetworkInformation::Reachability::Disconnected) {
                m_relayProbeFailures = 2;
                if (m_relaySpeedDot)
                    setRelayLinkSpeed(catalogApiUrl().host(), -1);
                // The OS has a definitive verdict, so it counts as the last
                // failure needed to switch into automatic offline mode rather
                // than making the user wait through three ten-second probes.
                m_offlineProbeFailures =
                    qMax(m_offlineProbeFailures, kOfflineProbeFailureLimit - 1);
                noteRelayReachability(false);
                if (m_backend)
                    m_backend->setNetworkAvailable(false);
                return;
            }

            // A local network is not proof that the relay is reachable. Keep
            // automatic offline mode in place until its marked HTTP probe
            // succeeds, while allowing a normal session to reconnect now.
            if (m_backend)
                m_backend->setNetworkAvailable(!m_offlineMode &&
                                               !projectInstancePending());
            // A machine that started with no network to mesh over has one
            // now; the local room does not care whether the relay answers.
            startLocalMesh();
            if (m_nodeEventSocket)
                m_nodeEventSocket->reconnectNow();
            probeRelayLatency();
        };
    connect(networkInfo, &QNetworkInformation::reachabilityChanged, this,
            applyReachability);
    // Preserve the normal delayed first probe for a connected/unknown state;
    // only an already-definitive disconnected verdict needs handling here.
    if (networkInfo->reachability() ==
        QNetworkInformation::Reachability::Disconnected) {
        applyReachability(networkInfo->reachability());
    }
}

void MainWindow::openServerWebsite(int index)
{
    if (index < 0 || index >= m_servers.size())
        return;
    QUrl url(m_servers.at(index).url);
    if (url.scheme() == "ws")
        url.setScheme(QStringLiteral("http"));
    else if (url.scheme() == "wss")
        url.setScheme(QStringLiteral("https"));
    url.setPath(QStringLiteral("/"));
    url.setQuery(QString());
    url.setFragment(QString());
    if (url.isValid() && !url.host().isEmpty())
        QDesktopServices::openUrl(url);
}

void MainWindow::openRepositoryWebsite()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    const QUrl url(repositoryWebUrl(repo));
    if (url.isValid() && !url.host().isEmpty())
        QDesktopServices::openUrl(url);
}

void MainWindow::showRelayMenu()
{
    if (!m_relayMenuButton)
        return;
    if (projectInstancePending()) {
        promptChooseProjectInstance();
        return;
    }
    constexpr qint64 kRelaySpeedFreshMs = 30 * 1000;
    QMenu menu(this);
    // One rule for every relay row's pick button instead of a sheet per row.
    menu.setStyleSheet(Theme::remapSheet(QStringLiteral(
        "QPushButton#relayMenuPick { text-align:left; padding:4px 6px; }")));

    QString activeHost;
    QString activeName;
    if (m_activeServer >= 0 && m_activeServer < m_servers.size()) {
        activeHost = serverHost(m_servers.at(m_activeServer).url);
        activeName = m_servers.at(m_activeServer).name.trimmed();
    }
    QAction *header = menu.addAction(
        activeHost.isEmpty()
            ? QStringLiteral("Relays (%1)").arg(formatCount(m_servers.size()))
            : QStringLiteral("%1 (%2) — Relays (%3)")
                  .arg(activeName.isEmpty() ? activeHost : activeName,
                       activeHost, formatCount(m_servers.size())));
    header->setEnabled(false);

    auto *searchEdit = new QLineEdit(&menu);
    searchEdit->setPlaceholderText(QStringLiteral("Search relays") +
                                   QString::fromUtf8("\xE2\x80\xA6"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(240);
    auto *searchAction = new QWidgetAction(&menu);
    searchAction->setDefaultWidget(searchEdit);
    menu.addAction(searchAction);
    menu.addSeparator();

    QList<QAction *> relayActions;
    for (int i = 0; i < m_servers.size(); ++i) {
        const ServerConfig &server = m_servers.at(i);
        const QString host = serverHost(server.url);
        const QString instanceName = server.name.trimmed().isEmpty()
                                         ? host
                                         : server.name.trimmed();
        const bool active = i == m_activeServer;

        auto *row = new QWidget(&menu);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(4, 0, 4, 0);
        rowLayout->setSpacing(2);

        auto rowText = [active](const QString &entry) {
            return (active ? QString::fromUtf8("\xE2\x9C\x93  ")
                           : QStringLiteral("     ")) + entry;
        };
        auto *pick = new QPushButton(QIcon(faviconFor(server)),
                                     rowText(QStringLiteral("%1  ·  %2")
                                                 .arg(instanceName,
                                                      relayMenuEntryText(host))),
                                     row);
        pick->setObjectName(QStringLiteral("relayMenuPick"));
        pick->setFlat(true);
        pick->setCursor(Qt::PointingHandCursor);
        pick->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(pick, &QPushButton::clicked, &menu, [this, i, &menu] {
            menu.close();
            switchToServer(i);
        });

        auto *open = new QPushButton(row);
        open->setObjectName(QStringLiteral("issueIconButton"));
        open->setFlat(true);
        open->setCursor(Qt::PointingHandCursor);
        open->setFixedSize(26, 26);
        open->setToolTip(QStringLiteral("Open %1 in your browser")
                             .arg(host.isEmpty() ? server.url : host));
        setOcticon(open, QStringLiteral("link"), 14);
        connect(open, &QPushButton::clicked, &menu, [this, i, &menu] {
            menu.close();
            openServerWebsite(i);
        });

        auto *edit = new QPushButton(row);
        edit->setObjectName(QStringLiteral("issueIconButton"));
        edit->setFlat(true);
        edit->setCursor(Qt::PointingHandCursor);
        edit->setFixedSize(26, 26);
        edit->setToolTip(QStringLiteral("Rename this relay instance"));
        setOcticon(edit, QStringLiteral("pencil"), 14);
        connect(edit, &QPushButton::clicked, &menu, [this, i, &menu] {
            menu.close();
            renameServer(i);
        });

        auto *remove = new QPushButton(row);
        remove->setObjectName(QStringLiteral("issueIconButton"));
        remove->setFlat(true);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setFixedSize(26, 26);
        remove->setEnabled(m_servers.size() > 1);
        remove->setToolTip(QStringLiteral("Remove this relay instance"));
        setOcticon(remove, QStringLiteral("trash"), 14);
        connect(remove, &QPushButton::clicked, &menu, [this, i, &menu] {
            menu.close();
            removeServer(i);
        });

        rowLayout->addWidget(pick, 1);
        rowLayout->addWidget(open, 0);
        rowLayout->addWidget(edit, 0);
        rowLayout->addWidget(remove, 0);

        auto *act = new QWidgetAction(&menu);
        act->setDefaultWidget(row);
        menu.addAction(act);
        relayActions.append(act);

        const RelayLatencySample sample =
            m_relayHostLatency.value(host.trimmed().toLower());
        if (QDateTime::currentMSecsSinceEpoch() - sample.stampMs <
            kRelaySpeedFreshMs)
            continue;
        QPointer<QPushButton> guarded(pick);
        probeRelayHostSpeed(server.url, [this, guarded, host, instanceName,
                                         rowText](int) {
            if (guarded)
                guarded->setText(rowText(QStringLiteral("%1  ·  %2")
                                             .arg(instanceName,
                                                  relayMenuEntryText(host))));
        });
    }

    menu.addSeparator();
    QAction *addAct = menu.addAction(QStringLiteral("Add relay") +
                                     QString::fromUtf8("\xE2\x80\xA6"));
    connect(addAct, &QAction::triggered, this, &MainWindow::promptAddServer);

    connect(searchEdit, &QLineEdit::textChanged, &menu,
            [this, relayActions](const QString &text) {
                const QString needle = text.trimmed().toLower();
                for (int i = 0;
                     i < relayActions.size() && i < m_servers.size(); ++i)
                    relayActions.at(i)->setVisible(
                        needle.isEmpty() ||
                        serverHost(m_servers.at(i).url).toLower().contains(needle) ||
                        m_servers.at(i).name.toLower().contains(needle));
            });
    QTimer::singleShot(0, searchEdit, [searchEdit] { searchEdit->setFocus(); });

    menu.exec(topBarMenuAnchor(m_relayMenuButton));
}

namespace {
// Hovering the rail used to spend a public-RPC read every minute, from every
// running node, forever — which is how one keyless public endpoint became the
// busiest thing ForkMesh talks to. Hover now only repaints what is already
// known; clicking the chip is the manual "check now".
const qint64 kNavSolanaBalanceTtlMs = 60 * 60 * 1000;
// A forced refresh — clicking the chip, or a heartbeat reporting the balance
// moved — still honours a floor, so a repeatedly clicked chip cannot become a
// burst of reads. Matches the relay's SOLANA_DISPLAY_BALANCE_FORCE_MIN_MS.
const qint64 kNavSolanaBalanceForceMinMs = 60 * 1000;

QString compactSolanaBalance(qint64 lamports)
{
    const double sol = lamports / 1000000000.0;
    if (lamports <= 0)
        return QStringLiteral("0 SOL");
    if (sol >= 1000.0)
        return QStringLiteral("%1k SOL").arg(sol / 1000.0, 0, 'f', 1);
    if (sol >= 100.0)
        return QStringLiteral("%1 SOL").arg(sol, 0, 'f', 1);
    if (sol >= 1.0)
        return QStringLiteral("%1 SOL").arg(sol, 0, 'f', 2);
    return QStringLiteral("%1 SOL").arg(sol, 0, 'f', 4);
}

QString lastSolanaBalanceSetting(const QString &address)
{
    return kSolanaLastBalanceSettingPrefix + address.trimmed();
}

QString externalWalletBalanceTooltip(const QString &detail)
{
    return detail +
           QStringLiteral(
               "\nNon-custodial: this is a public external-wallet balance. "
               "ForkMesh never receives or stores its private key.");
}
} // namespace

void MainWindow::updateNodeSwitcher()
{
    updateUserSwitcher();
    refreshNodesTable();
    if (!m_nodeMenuButton)
        return;
    const QString caret = QString::fromUtf8("\xE2\x96\xBE");
    const QString label =
        m_selectedNode.isEmpty() ? QStringLiteral("Nodes") : m_selectedNode;
    m_nodeMenuButton->setText(label + "  " + caret + "  " +
                              QString::number(m_nodeMenuEntries.size()));
    for (const NodeMenuEntry &e : std::as_const(m_nodeMenuEntries)) {
        if (e.name == m_selectedNode) {
            m_nodeMenuButton->setIcon(osBadgeIcon(e.platform, e.online, 16));
            return;
        }
    }
    m_nodeMenuButton->setIcon(QIcon());
}

void MainWindow::updateUserSwitcher()
{
    if (m_backend) {
        const bool userLike =
            m_profileIsUserAccount || !m_profileLinkedNodes.isEmpty();
        m_backend->setAccountKind(userLike ? QStringLiteral("user")
                                  : chatIdentityIsGuest()
                                      ? QStringLiteral("guest")
                                      : QStringLiteral("node"));
    }
    const QString user = topBarUserName();
    // "🌴 Vacationing — until Mon 5:00 PM" / "Away" under the identity line.
    const qint64 statusNowMs = QDateTime::currentMSecsSinceEpoch();
    const QString ownStatusTip =
        m_ownUserStatusKnown
            ? forkmesh::userstatus::tooltip(m_ownUserStatus, statusNowMs)
            : QString();
    const QString statusSuffix =
        ownStatusTip.isEmpty() ? QString() : QLatin1Char('\n') + ownStatusTip;
    if (m_userAvatarNavButton) {
        const QString userEmail = m_accountEmail.trimmed();
        const QString identityLine =
            user.isEmpty()
                ? QString()
                : userEmail.isEmpty()
                      ? user
                      : QStringLiteral("%1 (%2)").arg(user, userEmail);
        m_userAvatarNavButton->setToolTip(
            chatIdentityIsGuest()
                ? QStringLiteral("Chatting as %1 — pick a username in "
                                 "Settings or log in to claim one")
                      .arg(guestChatName())
                : (user.isEmpty()
                       ? QStringLiteral("Your user account")
                       : QStringLiteral("%1 user account").arg(identityLine)) +
                      statusSuffix);
    }
    if (m_accountSwitcherButton) {
        const bool signedIn = hasActiveAccountSession() ||
                              !nodeOwnerDisplayName().trimmed().isEmpty();
        m_accountSwitcherButton->setVisible(signedIn);
        m_accountSwitcherButton->setToolTip(
            user.isEmpty()
                ? QStringLiteral("Account menu and user switcher")
                : QStringLiteral("Signed in as %1 · click to switch users or "
                                 "manage this instance")
                          .arg(user) +
                      statusSuffix);
    }
    updateUserAvatarButton();
    updateChatIdentity();
    updateSignInButton();
    refreshWebUserSolanaAddress();
    updateNavSolanaBalance();
}

void MainWindow::cacheWebUserSolanaProfile(const QString &account,
                                           const QJsonObject &profile)
{
    const QString normalized = account.trimmed().toLower();
    if (normalized.isEmpty() ||
        profile.value(QStringLiteral("kind")).toString() !=
            QStringLiteral("user"))
        return;
    QString address = profile.value(QStringLiteral("solana")).toString().trimmed();
    if (!address.isEmpty() &&
        !forkmesh::control::isValidSolanaPublicAddress(address))
        address.clear();
    m_webSolanaAccount = normalized;
    m_webSolanaAddress = address;
    m_webSolanaKnown = true;
    m_webSolanaFetchedMs = QDateTime::currentMSecsSinceEpoch();
    updateNavSolanaBalance();
}

void MainWindow::refreshWebUserSolanaAddress()
{
    if (!m_networkAccess)
        return;
    const QString account = topBarUserName().trimmed().toLower();
    const bool hasWebUser =
        !m_nodeOwnerUser.trimmed().isEmpty() || m_profileIsUserAccount ||
        (m_accountAuthenticated &&
         settingsAccountName().compare(account, Qt::CaseInsensitive) == 0);
    if (account.isEmpty() || !hasWebUser) {
        m_webSolanaAccount.clear();
        m_webSolanaAddress.clear();
        m_webSolanaKnown = false;
        m_webSolanaFetchedMs = 0;
        return;
    }

    if (m_webSolanaAccount != account) {
        m_webSolanaAccount = account;
        m_webSolanaAddress.clear();
        m_webSolanaKnown = false;
        m_webSolanaFetchedMs = 0;
    }
    // The payout address rides along on the account record, which login already
    // read. Every caller of this function is a repaint (the top bar rebuilds on
    // most navigations), so re-reading the record here — it used to happen both
    // on a 60s timer and whenever the cached copy aged past 55s — meant one URL
    // dominated a session's network log for a value that changes by hand.
    const QJsonObject cached = cachedAccountRecord(account);
    if (!cached.isEmpty()) {
        cacheWebUserSolanaProfile(account, cached);
        m_webSolanaFetchedMs = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    if (m_webSolanaFetchedMs > 0 || m_webSolanaFetchInFlight)
        return;

    m_webSolanaFetchInFlight = true;
    const QUrl profileUrl = accountsApiUrl(account);
    QNetworkRequest request(profileUrl);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-cache");
    const QString sessionToken = accountSessionTokenForUrl(profileUrl);
    if (!sessionToken.isEmpty())
        request.setRawHeader(
            "Authorization",
            QByteArrayLiteral("Bearer ") + sessionToken.toUtf8());
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, account] {
                m_webSolanaFetchInFlight = false;
                const bool ok = reply->error() == QNetworkReply::NoError;
                const QJsonObject profile =
                    QJsonDocument::fromJson(reply->readAll()).object();
                reply->deleteLater();
                if (account != topBarUserName().trimmed().toLower()) {
                    refreshWebUserSolanaAddress();
                    return;
                }
                if (!ok)
                    return; // a failed read is not an answer: let it retry
                // Stamped even when the record says this is a node account (no
                // payout address of its own), so the next repaint doesn't ask
                // the same question again.
                m_webSolanaFetchedMs = QDateTime::currentMSecsSinceEpoch();
                if (profile.value(QStringLiteral("exists")).toBool())
                    cacheAccountRecord(account, profile);
            });
}

void MainWindow::updateNavSolanaBalance()
{
    updateAdminCrownBadge();
    if (!m_navSolanaBalance)
        return;

    const QString account = topBarUserName().trimmed().toLower();
    const QString addr =
        m_webSolanaKnown && m_webSolanaAccount == account
            ? m_webSolanaAddress
            : savedSolanaAddress();
    if (addr != m_navSolanaBalanceAddress) {
        m_navSolanaLamports = -1;
        m_navSolanaFetchedMs = 0;
        const QVariant saved = QSettings().value(lastSolanaBalanceSetting(addr));
        bool savedOk = false;
        const qint64 savedLamports = saved.toString().toLongLong(&savedOk);
        if (saved.isValid() && savedOk && savedLamports >= 0)
            m_navSolanaLamports = savedLamports;
    }
    m_navSolanaBalanceAddress = addr;
    if (addr.isEmpty()) {
        m_navSolanaLamports = -1;
        m_navSolanaBalance->setText(
            QStringLiteral("<a href=\"settings\">Add SOL</a>"));
        m_navSolanaBalance->setToolTip(externalWalletBalanceTooltip(
            QStringLiteral(
                "Add a public self-custodial Solana address to show its balance.")));
        return;
    }
    if (!isLikelySolanaAddress(addr)) {
        m_navSolanaLamports = -1;
        m_navSolanaBalance->setText(QStringLiteral("bad addr"));
        m_navSolanaBalance->setToolTip(externalWalletBalanceTooltip(
            QStringLiteral("Saved public Solana address is invalid.")));
        return;
    }

    renderNavSolanaBalance();
}

void MainWindow::renderNavSolanaBalance()
{
    if (!m_navSolanaBalance)
        return;
    if (m_navSolanaBalanceAddress.isEmpty())
        return; // updateNavSolanaBalance() already painted the empty state
    if (m_navSolanaLamports < 0) {
        if (m_navSolanaFetchInFlight)
            return; // a hover-triggered query is already painting "... SOL"
        m_navSolanaBalance->setText(QString::fromUtf8("\xE2\x80\x94 SOL"));
        m_navSolanaBalance->setToolTip(externalWalletBalanceTooltip(
            QStringLiteral("Click to check your public Solana balance.")));
        return;
    }
    m_navSolanaBalance->setText(compactSolanaBalance(m_navSolanaLamports));
    m_navSolanaBalance->setToolTip(
        externalWalletBalanceTooltip(
            QStringLiteral("Your public Solana balance: %1\nClick to check it "
                           "again; it is otherwise re-read once an hour.")
                .arg(formatSolanaBalance(m_navSolanaLamports))));
}

void MainWindow::refreshNavSolanaBalance(bool force)
{
    if (!m_navSolanaBalance || !m_networkAccess)
        return;
    const QString addr = m_navSolanaBalanceAddress;
    if (addr.isEmpty() || !isLikelySolanaAddress(addr))
        return;
    if (m_navSolanaFetchInFlight)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 minimumAge =
        force ? kNavSolanaBalanceForceMinMs : kNavSolanaBalanceTtlMs;
    if (m_navSolanaFetchedMs > 0 && now - m_navSolanaFetchedMs < minimumAge)
        return;
    if (m_navSolanaLamports < 0) {
        m_navSolanaBalance->setText(QStringLiteral("... SOL"));
        m_navSolanaBalance->setToolTip(externalWalletBalanceTooltip(
            QStringLiteral("Checking your public Solana balance.")));
    }
    m_navSolanaFetchInFlight = true;
    queryNavSolanaBalance(addr, 0);
}

void MainWindow::queryNavSolanaBalance(const QString &addr, int endpointIndex)
{
    const int count = int(sizeof(kSolanaRpcEndpoints) / sizeof(kSolanaRpcEndpoints[0]));
    if (endpointIndex >= count) {
        m_navSolanaFetchInFlight = false;
        m_navSolanaFetchedMs = QDateTime::currentMSecsSinceEpoch();
        if (m_navSolanaBalance && m_navSolanaBalanceAddress == addr &&
            m_navSolanaLamports < 0) {
            m_navSolanaBalance->setText(QStringLiteral("n/a SOL"));
            m_navSolanaBalance->setToolTip(externalWalletBalanceTooltip(
                QStringLiteral("Public Solana balance is temporarily unavailable.")));
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
        if (!m_navSolanaBalance || m_navSolanaBalanceAddress != addr) {
            m_navSolanaFetchInFlight = false;
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(raw).object();
        const QJsonObject result = root.value("result").toObject();
        if (netError != QNetworkReply::NoError || !result.contains("value")) {
            queryNavSolanaBalance(addr, endpointIndex + 1); // stays in-flight
            return;
        }
        m_navSolanaFetchInFlight = false;
        m_navSolanaFetchedMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 lamports = result.value("value").toVariant().toLongLong();
        m_navSolanaLamports = lamports; // cache so re-renders don't re-query
        QSettings settings;
        const QString lastBalanceKey = lastSolanaBalanceSetting(addr);
        const QVariant previousValue = settings.value(lastBalanceKey);
        const qint64 previousLamports = previousValue.toLongLong();
        const QString balance = formatSolanaBalance(lamports);
        if (previousValue.isValid() && lamports > previousLamports &&
            QSettings().value(kDisbursementAlertSetting, false).toBool()) {
            const QString amount = formatSolanaBalance(lamports - previousLamports);
            if (!pingsPaused() && nativeSystemNotificationsEnabled())
                QApplication::alert(this, 0);
            postNotification(QStringLiteral("Public wallet balance increased"),
                             QStringLiteral(
                                 "%1 received by your external self-custodial "
                                 "wallet. New public balance: %2")
                                 .arg(amount, balance),
                             false, QStringLiteral("emblem-default"));
        }
        settings.setValue(lastBalanceKey, QString::number(lamports));
        m_navSolanaBalance->setText(compactSolanaBalance(lamports));
        m_navSolanaBalance->setToolTip(
            externalWalletBalanceTooltip(
                QStringLiteral("Your public Solana balance: %1")
                    .arg(balance)));
    });
}

void MainWindow::showNodeMenu()
{
    if (!m_nodeMenuButton)
        return;
    QMenu menu(this);

    QAction *header =
        menu.addAction(QStringLiteral("Nodes (%1)").arg(formatCount(m_nodeMenuEntries.size())));
    header->setEnabled(false);

    auto *searchEdit = new QLineEdit(&menu);
    searchEdit->setPlaceholderText(QStringLiteral("Search nodes") +
                                   QString::fromUtf8("\xE2\x80\xA6"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(240);
    auto *searchAction = new QWidgetAction(&menu);
    searchAction->setDefaultWidget(searchEdit);
    menu.addAction(searchAction);
    menu.addSeparator();

    if (m_nodeMenuEntries.isEmpty()) {
        QAction *empty = menu.addAction(QStringLiteral("No nodes yet"));
        empty->setEnabled(false);
    }

    QList<QAction *> nodeActions;
    QStringList nodeNames;
    for (const NodeMenuEntry &e : std::as_const(m_nodeMenuEntries)) {
        QString text = e.name;
        if (e.self)
            text += " (this machine)";
        text += QStringLiteral("   %1 repo%2")
                    .arg(e.repoCount)
                    .arg(e.repoCount == 1 ? "" : "s");
        QAction *act = menu.addAction(osBadgeIcon(e.platform, e.online, 16), text);
        act->setCheckable(true);
        act->setChecked(e.name == m_selectedNode);
        const QString node = e.name;
        connect(act, &QAction::triggered, this, [this, node] {
            selectNode(node);                 // fill the repositories column
            showNodeProfile(QString(), node); // and open the node's profile
        });
        nodeActions.append(act);
        nodeNames.append(e.name.toLower());
    }

    connect(searchEdit, &QLineEdit::textChanged, &menu,
            [nodeActions, nodeNames](const QString &text) {
                const QString needle = text.trimmed().toLower();
                for (int i = 0; i < nodeActions.size(); ++i)
                    nodeActions.at(i)->setVisible(needle.isEmpty() ||
                                                  nodeNames.at(i).contains(needle));
            });
    QTimer::singleShot(0, searchEdit, [searchEdit] { searchEdit->setFocus(); });

    menu.exec(m_nodeMenuButton->mapToGlobal(
        QPoint(0, m_nodeMenuButton->height())));
}

void MainWindow::showNodesWindow()
{
    auto *dialog = new QDialog(this);
    dialog->setObjectName("nodesWindow");
    dialog->setWindowTitle(QStringLiteral("Network nodes"));
    dialog->setMinimumSize(560, 520);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto *outer = new QVBoxLayout(dialog);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(10);

    auto *heading = new QLabel;
    heading->setObjectName("nodesWindowHeading");
    heading->setTextFormat(Qt::RichText);
    outer->addWidget(heading);

    auto *scroll = new QScrollArea(dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *listHost = new QWidget;
    auto *listLayout = new QVBoxLayout(listHost);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(8);
    listLayout->addStretch();
    scroll->setWidget(listHost);
    outer->addWidget(scroll, 1);

    const bool dark = currentThemeIsDark();
    const QString cardBg = dark ? "#161b22" : "#ffffff";
    const QString cardBorder = dark ? "#30363d" : "#d0d7de";
    const QString subFg = dark ? "#8b949e" : "#57606a";
    // Every card shares these rules; set them once on the list host rather
    // than parsing a sheet per card each time the list repopulates.
    listHost->setStyleSheet(Theme::remapSheet(
        QStringLiteral("#nodeCard { background:%1; border:1px solid %2; "
                       "border-radius:10px; } "
                       "QLabel#nodeCardDetails { color:%3; font-size:12px; }")
            .arg(cardBg, cardBorder, subFg)));

    auto populate = std::make_shared<std::function<void()>>();
    *populate = [this, listLayout, heading, dialog, subFg, populate] {
        while (listLayout->count() > 1) {
            QLayoutItem *item = listLayout->takeAt(0);
            if (item->widget())
                item->widget()->deleteLater();
            delete item;
        }

        QList<MemberInfo> nodes;
        for (const MemberInfo &m : std::as_const(m_homeRoster)) {
            if (!isTemporaryChatGuest(m))
                nodes.append(m);
        }
        std::sort(nodes.begin(), nodes.end(), [](const MemberInfo &a,
                                                 const MemberInfo &b) {
            if (a.self != b.self)
                return a.self; // you first
            if (a.online != b.online)
                return a.online; // then online
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });

        int online = 0;
        for (const MemberInfo &m : nodes)
            if (m.online || m.self)
                ++online;
        heading->setText(
            QString::fromUtf8("<b>%1</b> node%2 \xC2\xB7 <span style='color:#3fb950'>"
                           "%3 online</span>")
                .arg(nodes.size())
                .arg(nodes.size() == 1 ? "" : "s")
                .arg(online));

        if (nodes.isEmpty()) {
            auto *empty = new QLabel(QStringLiteral("No nodes on this relay yet."));
            empty->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1; padding:24px;").arg(subFg)));
            empty->setAlignment(Qt::AlignCenter);
            listLayout->insertWidget(0, empty);
            return;
        }

        for (const MemberInfo &node : nodes) {
            auto *card = new QWidget;
            card->setObjectName("nodeCard");
            auto *row = new QHBoxLayout(card);
            row->setContentsMargins(12, 12, 12, 12);
            row->setSpacing(12);

            QPixmap avatar = m_avatars.value(node.id);
            if (avatar.isNull())
                avatar = nodeMachineFavicon(node.id.isEmpty() ? node.name : node.id,
                                            48);
            auto *icon = new QLabel;
            icon->setPixmap(roundedRectPixmap(avatar, 48, 12));
            icon->setFixedSize(48, 48);
            row->addWidget(icon, 0, Qt::AlignTop);

            auto *info = new QVBoxLayout;
            info->setSpacing(3);
            const bool isOnline = node.self ? (m_backend != nullptr) : node.online;
            auto *title = new QLabel(
                QString::fromUtf8("<span style='color:%1'>\xE2\x97\x8F</span> "
                               "<b>%2</b>%3")
                    .arg(isOnline ? "#3fb950" : "#8b949e",
                         node.name.toHtmlEscaped(),
                         node.self ? " <span style='color:#8b949e'>(this "
                                     "machine)</span>"
                                   : QString()));
            title->setTextFormat(Qt::RichText);
            info->addWidget(title);

            QStringList lines;
            lines << QStringLiteral("Status: %1")
                         .arg(isOnline ? "Online" : "Offline");
            if (!node.platform.isEmpty())
                lines << QStringLiteral("Platform: %1").arg(node.platform.toHtmlEscaped());
            if (!node.version.isEmpty())
                lines << QStringLiteral("Version: %1").arg(node.version.toHtmlEscaped());
            lines << QStringLiteral("Public wallet balance: %1")
                         .arg(node.solanaBalance.trimmed().isEmpty()
                                  ? QString::fromUtf8("\xE2\x80\x94")
                                  : node.solanaBalance.trimmed().toHtmlEscaped() +
                                        " SOL");
            if (!node.solanaAddress.trimmed().isEmpty())
                lines << QStringLiteral("Solana: %1")
                             .arg(node.solanaAddress.trimmed().toHtmlEscaped());
            lines << QStringLiteral("Mirrors: %1")
                         .arg(node.mirrors.isEmpty()
                                  ? QStringLiteral("none")
                                  : node.mirrors.join(", ").toHtmlEscaped());
            if (!node.id.isEmpty())
                lines << QStringLiteral("Node id: %1")
                             .arg(node.id.left(16).toHtmlEscaped() +
                                  (node.id.size() > 16 ? "\xE2\x80\xA6" : ""));
            if (!node.note.trimmed().isEmpty())
                lines << node.note.trimmed().toHtmlEscaped();

            auto *details = new QLabel(lines.join("<br>"));
            details->setTextFormat(Qt::RichText);
            details->setObjectName(QStringLiteral("nodeCardDetails"));
            details->setWordWrap(true);
            details->setTextInteractionFlags(Qt::TextSelectableByMouse);
            info->addWidget(details);
            row->addLayout(info, 1);

            auto *actions = new QVBoxLayout;
            actions->setSpacing(6);
            auto *profileBtn = new QPushButton(QStringLiteral("Profile"));
            profileBtn->setCursor(Qt::PointingHandCursor);
            const QString nid = node.id;
            const QString nname = node.name;
            connect(profileBtn, &QPushButton::clicked, dialog, [this, nid, nname] {
                showNodeProfile(nid, nname);
            });
            actions->addWidget(profileBtn);
            if (!node.self && !node.id.isEmpty()) {
                auto *delBtn = new QPushButton(QStringLiteral("Delete"));
                delBtn->setCursor(Qt::PointingHandCursor);
                delBtn->setObjectName("dangerButton");
                connect(delBtn, &QPushButton::clicked, dialog,
                        [this, nid, nname, populate] {
                            if (QMessageBox::question(
                                    nullptr, QStringLiteral("Delete node"),
                                    QStringLiteral("Forget %1? It reappears if the "
                                                   "node announces itself again.")
                                        .arg(nname)) != QMessageBox::Yes)
                                return;
                            removeChatMember(nid, nname);
                            (*populate)();
                        });
                actions->addWidget(delBtn);
            }
            actions->addStretch();
            row->addLayout(actions, 0);

            listLayout->insertWidget(listLayout->count() - 1, card);
        }
    };
    (*populate)();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    outer->addWidget(buttons);

    dialog->show();
}

void MainWindow::updateRepoSwitcher()
{
    if (!m_repoMenuButton)
        return;
    if (m_nodeSwitching) {
        m_repoMenuButton->setVisible(true);
        if (m_repoLabel)
            m_repoLabel->hide();
        m_repoMenuButton->setText(QString::fromUtf8("Loading\xE2\x80\xA6"));
        return;
    }
    const bool hasRepos = !m_repoMenuEntries.isEmpty();
    m_repoMenuButton->setVisible(hasRepos);
    if (m_repoLabel)
        m_repoLabel->hide();
    if (!hasRepos)
        return;
    const QString caret = QString::fromUtf8("\xE2\x96\xBE");
    QString label = QStringLiteral("Repositories");
    QString owner;
    if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
        const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
        owner = repo.owner;
        label = repo.owner + QLatin1Char('/') + repo.name;
    }
    int ownerRepoCount = 0;
    for (const RepoMenuEntry &entry : std::as_const(m_repoMenuEntries)) {
        if (entry.index >= 0 && entry.index < m_repositories.size() &&
            m_repositories.at(entry.index).owner == owner)
            ++ownerRepoCount;
    }
    if (ownerRepoCount > 1)
        label += QStringLiteral("  ") + caret;
    m_repoMenuButton->setText(label);
    m_repoMenuButton->setEnabled(ownerRepoCount > 1);
}

bool MainWindow::relayPublishRepo(const RepositoryRecord &repo,
                                  const QString &relayHost,
                                  QString *localBranch, int *unpublished)
{
    if (localBranch)
        localBranch->clear();
    if (unpublished)
        *unpublished = 0;
    if (repo.localPath.isEmpty() || !QDir(repo.localPath).exists(".git"))
        return false;
    if (repo.mirrorPath.isEmpty() || !QDir(repo.mirrorPath).exists())
        return false;

    const bool hasUpstream =
        runGitCapture(repo.localPath,
                      {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
                       QStringLiteral("--symbolic-full-name"),
                       QStringLiteral("@{upstream}")},
                      nullptr, nullptr);
    if (hasUpstream) {
        if (!upstreamPublishesThroughMirror(repo.localPath, repo.mirrorPath,
                                            relayHost))
            return false;
    } else if (!repo.publishToNetwork) {
        return false;
    }

    if (localBranch) {
        QByteArray branchOut;
        runGitCapture(repo.localPath,
                      {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
                       QStringLiteral("HEAD")},
                      &branchOut, nullptr);
        *localBranch = QString::fromUtf8(branchOut).trimmed();
    }
    if (unpublished) {
        const QString mirrorCommit =
            mirrorBranchCommit(repo.mirrorPath, mirrorHeadBranch(repo.mirrorPath));
        QByteArray countOut;
        runGitCapture(repo.localPath,
                      mirrorCommit.isEmpty()
                          ? QStringList{QStringLiteral("rev-list"),
                                        QStringLiteral("--count"), QStringLiteral("HEAD")}
                          : QStringList{QStringLiteral("rev-list"),
                                        QStringLiteral("--count"),
                                        mirrorCommit + QStringLiteral("..HEAD")},
                      &countOut, nullptr);
        *unpublished = QString::fromUtf8(countOut).trimmed().toInt();
    }
    return true;
}

MainWindow::PushTarget MainWindow::readPushTarget(const RepositoryRecord &repo,
                                                  const QString &relayHost)
{
    PushTarget target;
    target.isRelay = relayPublishRepo(repo, relayHost, nullptr, nullptr);
    if (target.isRelay)
        return target;
    QByteArray upstreamOut;
    target.hasUpstream =
        runGitCapture(repo.localPath,
                      {QStringLiteral("rev-parse"), QStringLiteral("--abbrev-ref"),
                       QStringLiteral("--symbolic-full-name"),
                       QStringLiteral("@{upstream}")},
                      &upstreamOut, nullptr);
    if (!target.hasUpstream)
        return target;
    target.upstream = QString::fromUtf8(upstreamOut).trimmed();
    const QString remoteName = target.upstream.section(QLatin1Char('/'), 0, 0);
    QByteArray remoteOut;
    runGitCapture(repo.localPath,
                  {QStringLiteral("remote"), QStringLiteral("get-url"), remoteName},
                  &remoteOut, nullptr);
    target.remoteUrl = QString::fromUtf8(remoteOut).trimmed();
    return target;
}

void MainWindow::refreshRepoSyncIndicators()
{
    if (m_scmPanel)
        refreshSourceControlOutgoing();
    else if (m_railGitButton)
        m_railGitButton->setPendingSyncCount(0);

    refreshCommitMarkersIfStale();

    const int index = m_repoDetailIndex;
    if (m_railGitButton)
        m_railGitButton->setSyncing(
            index >= 0 &&
            (m_pushingRepos.contains(index) ||
             (m_syncingRepos.contains(index) && !m_syncingRepos.value(index))));
}

static QString hashForEachRefOutput(const QByteArray &out)
{
    return PublicMirrorRuntime::refsSha256FromForEachRef(out);
}

QString MainWindow::mirrorStateHash(const QString &mirrorPath)
{
    if (mirrorPath.trimmed().isEmpty())
        return QString();
    // The fingerprint is a pure function of the mirror's refs. Every roster
    // frame asked for it twice (the Mirror nodes panel on the GUI thread —
    // sampled at ~700 ms a call — and the behind-roster sync), and the
    // catalog pass a third time on a worker. Key it on the refs stamp: a
    // few stats to find out nothing moved, a read of the ref files when it
    // did (one `for-each-ref` when they hold anything unusual).
    struct Entry {
        QString stamp;
        QString hash;
    };
    static QHash<QString, Entry> cache;
    static QMutex cacheMutex;
    const QString stamp = gitRefsDirSignature(mirrorPath);
    if (!stamp.isEmpty()) {
        QMutexLocker lock(&cacheMutex);
        const auto cached = cache.constFind(mirrorPath);
        if (cached != cache.constEnd() && cached->stamp == stamp)
            return cached->hash;
    }
    QByteArray out;
    if (!forEachRefHeadsAndTagsFromFiles(mirrorPath, &out) &&
        !runGitCapture(mirrorPath,
                       {"for-each-ref", "--format=%(objectname) %(refname)",
                        "refs/heads/", "refs/tags/"},
                       &out, nullptr))
        return QString();
    const QString hash = hashForEachRefOutput(out);
    if (!stamp.isEmpty()) {
        QMutexLocker lock(&cacheMutex);
        cache.insert(mirrorPath, Entry{stamp, hash});
    }
    return hash;
}

QString MainWindow::mirrorRoutingStateHash(const QString &mirrorPath)
{
    if (mirrorPath.trimmed().isEmpty())
        return QString();

    // Match mirror_gateway.routing_refs_canonical exactly. The direct health
    // proof uses the symbolic default branch (falling back to main) plus tags
    // so routine agent-branch churn does not make a healthy routing endpoint
    // disappear. Comparing that proof with mirrorStateHash(), which covers all
    // heads, makes every fully synchronized mirror look out of sync as soon as
    // the repository has any non-default branch.
    struct Entry {
        QString stamp;
        QString headRef;
        QString hash;
    };
    static QHash<QString, Entry> cache;
    static QMutex cacheMutex;
    // The refs signature already covers HEAD (gitRefsDirSignature notes its
    // mtime), so an unchanged stamp means the same default branch and tags.
    // Answer from the cache before spawning any git: this ran symbolic-ref on
    // the UI thread on every call and stalled it four times on 2026-09-10.
    const QString stamp = gitRefsDirSignature(mirrorPath);
    if (!stamp.isEmpty()) {
        QMutexLocker lock(&cacheMutex);
        const auto cached = cache.constFind(mirrorPath);
        if (cached != cache.constEnd() && cached->stamp == stamp)
            return cached->hash;
    }

    QByteArray headOut;
    QString headRef = QStringLiteral("refs/heads/main");
    if (runGitCapture(mirrorPath,
                      {QStringLiteral("symbolic-ref"), QStringLiteral("--quiet"),
                       QStringLiteral("HEAD")},
                      &headOut, nullptr)) {
        const QString candidate = QString::fromUtf8(headOut).trimmed();
        if (candidate.startsWith(QLatin1String("refs/heads/")))
            headRef = candidate;
    }

    QByteArray out;
    if (!runGitCapture(mirrorPath,
                       {QStringLiteral("for-each-ref"),
                        QStringLiteral("--sort=refname"),
                        QStringLiteral("--format=%(objectname) %(refname)"),
                        headRef, QStringLiteral("refs/tags/")},
                       &out, nullptr))
        return QString();
    const QString hash = hashForEachRefOutput(out);
    if (!stamp.isEmpty()) {
        QMutexLocker lock(&cacheMutex);
        cache.insert(mirrorPath, Entry{stamp, headRef, hash});
    }
    return hash;
}

void MainWindow::refreshRepoPinBanner()
{
    if (!m_pageNoticeArea)
        return;
    const bool wasFlagged = m_repoPinMismatch;
    m_repoPinMismatch = false;
    m_repoPinCheckIndex = -1;
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord repo = m_repositories.at(m_repoDetailIndex);
    if (!repo.publishToNetwork || repo.previewOnly ||
        repo.mirrorPath.trimmed().isEmpty())
        return;
    if (!m_profileIdentity.isValid() || catalogOwner(repo) != accountOwner())
        return;
    if (!repoHasWorkingTree())
        return;

    const int index = m_repoDetailIndex;
    const QString publicOwner = repoSegment(repo.owner, QStringLiteral("owner"));
    const QString name = repoSegment(repo.name, QStringLiteral("repository"));
    if (!m_networkAccess || publicOwner.isEmpty() || name.isEmpty())
        return;
    m_repoPinCheckIndex = index;

    // Ask the relay for its own clone-admission verdict. Comparing local refs
    // with this node's catalog stateHash no longer means anything: the desktop
    // stopped publishing catalog rows on 2026-08-12, so that pin is frozen and
    // the comparison reported rejected clones permanently while every mirror
    // served (forkmesh::mirrorsync::clonesRejected).
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/api/repo/%1/%2/mirrors")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(publicOwner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(name))));
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, index, publicOwner, name, wasFlagged] {
                reply->deleteLater();
                if (!m_pageNoticeArea || m_repoPinCheckIndex != index ||
                    m_repoDetailIndex != index)
                    return;
                const bool readable =
                    reply->error() == QNetworkReply::NoError &&
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                            .toInt() == 200;
                const QJsonObject payload =
                    readable ? QJsonDocument::fromJson(reply->readAll()).object()
                             : QJsonObject();
                if (forkmesh::mirrorsync::clonesRejected(payload)) {
                    m_repoPinMismatch = true;
                    logSystem(
                        "Integrity pin: clones of " + publicOwner + "/" + name +
                        " are being rejected — no online mirror serves refs the "
                        "relay admits.");
                    loadMirrorNodesPanel(); // paint the caution triangle now
                    logSystem(
                        "Integrity pin: waiting for the managed "
                        "mirror service to publish current refs for " +
                        publicOwner + "/" + name + ".");
                } else if (wasFlagged) {
                    loadMirrorNodesPanel();
                }
            });
}

// Auto-heal the integrity pin across ALL of this node's source-of-truth repos,
// not just the one whose detail is open (refreshRepoPinBanner) or reset by hand
// (resetRepoPin). Only the node holding the working copy can re-sign the pin, so
// when a source repo's served refs drift past its published pin — a direct git
// op on the mirror, a sync that landed without a re-publish, a dropped publish —
// every clone of it is rejected until the owner happens to open that repo and
// click "Reset integrity pin". This closes that gap: while the source is online,
// it keeps its own pins in step automatically. Security is unchanged — we only
// re-attest OUR OWN authentic served refs (the same signing every publish does),
// so mirrors are still validated against a pin the source signed; when the source
// is offline this never runs, the pin freezes, and the relay's tamper gate keeps
// protecting clones against a stale or forged mirror exactly as before.
void MainWindow::resetRepoPin()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size())
        return;
    const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
    logSystem("Integrity pin: asking managed mirrors to sync " + repo.owner + "/" +
              repo.name + ".");
    requestMirrorNodesRefresh();
    refreshRepoPinBanner(); // re-read the relay's admission verdict
}

void MainWindow::pushCurrentRepoUpstream()
{
    if (m_repoDetailIndex < 0 || m_repoDetailIndex >= m_repositories.size() ||
        m_pushingRepos.contains(m_repoDetailIndex))
        return;

    const int index = m_repoDetailIndex;
    const RepositoryRecord repo = m_repositories.at(index);
    if (repo.localPath.isEmpty() || !QDir(repo.localPath).exists(".git"))
        return;

    m_pushingRepos.insert(index);
    refreshRepoSyncIndicators();
    const QString relayHost = configuredRelayHost();
    runOffThread<PushTarget>(
        [repo, relayHost] { return readPushTarget(repo, relayHost); },
        [this, index, repo](PushTarget target) {
            continueRepoPush(index, repo, target);
        });
}

void MainWindow::continueRepoPush(int index, const RepositoryRecord &repo,
                                  const PushTarget &target)
{
    if (index < 0 || index >= m_repositories.size() ||
        m_repositories.at(index).localPath != repo.localPath) {
        m_pushingRepos.remove(index);
        refreshRepoSyncIndicators();
        return;
    }
    const bool isRelay = target.isRelay;
    QString upstream;
    if (!isRelay) {
        if (!target.hasUpstream) {
            m_pushingRepos.remove(index);
            flashMessage(QStringLiteral("No upstream branch is configured for %1/%2.")
                             .arg(repo.owner, repo.name),
                         true);
            refreshRepoSyncIndicators();
            return;
        }
        upstream = target.upstream;
        const QString remoteUrl = target.remoteUrl;
        const QUrl parsedRemote(remoteUrl);
        const QStringList remoteParts = parsedRemote.path().split(
            QLatin1Char('/'), Qt::SkipEmptyParts);
        const QString remoteOwner =
            remoteParts.size() >= 2 ? remoteParts.at(remoteParts.size() - 2)
                                     : QString();
        QString remoteRepo = remoteParts.isEmpty() ? QString() : remoteParts.last();
        if (remoteRepo.endsWith(QLatin1String(".git"), Qt::CaseInsensitive))
            remoteRepo.chop(4);
        const QString host = parsedRemote.host().toLower();
        const bool forkMeshRemote = host == QLatin1String("forkmesh.com") ||
                                    host.endsWith(QLatin1String(".forkmesh.com"));
        if (forkMeshRemote && !remoteOwner.isEmpty() &&
            remoteOwner.compare(accountOwner(), Qt::CaseInsensitive) != 0 &&
            repo.forkedFrom.trimmed().isEmpty()) {
            m_pushingRepos.remove(index);
            refreshRepoSyncIndicators();
            QMessageBox box(this);
            box.setWindowTitle(QStringLiteral("Read-only upstream"));
            box.setIcon(QMessageBox::Information);
            box.setText(QStringLiteral(
                "You do not own %1/%2, so ForkMesh will not push directly to "
                "its mirror. Prepare this checkout as your local fork instead?")
                            .arg(remoteOwner, remoteRepo));
            box.setInformativeText(QStringLiteral(
                "Your commits stay intact. Publish the fork, then create a "
                "feature branch and open a pull request to propose them upstream."));
            QPushButton *prepare = box.addButton(QStringLiteral("Prepare fork for PR"),
                                                 QMessageBox::AcceptRole);
            box.addButton(QMessageBox::Cancel);
            box.setDefaultButton(prepare);
            box.exec();
            if (box.clickedButton() != prepare)
                return;
            if (index >= m_repositories.size() ||
                m_repositories.at(index).localPath != repo.localPath)
                return;
            RepositoryRecord &localFork = m_repositories[index];
            localFork.forkedFrom =
                (remoteOwner + QLatin1Char('/') + remoteRepo).toLower();
            localFork.cloneUrl = remoteUrl;
            localFork.publishToNetwork = true;
            stampRepositoryRelay(localFork);
            saveRepositories();
            configureForkSource(index, remoteUrl, true);
            refreshRepositoryList();
            flashMessage(QStringLiteral(
                "Prepared %1/%2 as your fork of %3. Use a feature branch, then "
                "choose Pulls → New pull request.")
                             .arg(localFork.owner, localFork.name,
                                  localFork.forkedFrom));
            return;
        }
    }

    // The secret scan walks the pushed commits (or every tracked file for a relay
    // repo) and the ahead-count runs rev-list — both are git reads heavy enough to
    // freeze the GUI thread on a large repo (StallWatchdog's top push offender,
    // Run them off-thread and resume on the GUI thread for the
    // (possibly modal) result. Mark the repo "pushing" now so the button flips to
    // its busy state and the entry guard blocks a second click during the scan.
    m_pushingRepos.insert(index);
    setRepoSyncActivity(index,
                        repo.secretScanningEnabled
                            ? QStringLiteral("Scanning the outgoing commits for "
                                             "secrets…")
                            : QStringLiteral("Counting the outgoing commits…"));
    refreshRepoSyncIndicators();

    struct PushScan {
        QList<RepoSecurityFinding> findings;
        int ahead = 0;
    };
    const bool scanEnabled = repo.secretScanningEnabled;
    const QString localPath = repo.localPath;
    runOffThread<PushScan>(
        [scanEnabled, localPath, upstream, isRelay]() {
            PushScan scan;
            if (scanEnabled)
                scan.findings = RepoSecurity::findSecretsInPush(localPath, upstream);
            if (!isRelay) {
                QByteArray countOut;
                runGitCapture(localPath,
                              {QStringLiteral("rev-list"), QStringLiteral("--count"),
                               QStringLiteral("@{upstream}..HEAD")},
                              &countOut, nullptr);
                scan.ahead = QString::fromUtf8(countOut).trimmed().toInt();
            }
            return scan;
        },
        [this, index, repo, upstream, isRelay](PushScan scan) {
            if (index < 0 || index >= m_repositories.size() ||
                m_repositories.at(index).localPath != repo.localPath) {
                m_pushingRepos.remove(index);
                clearRepoSyncActivity(index);
                refreshRepoSyncIndicators();
                return;
            }
            if (!scan.findings.isEmpty()) {
                QString detail;
                const int shown = qMin(scan.findings.size(), 5);
                for (int i = 0; i < shown; ++i) {
                    const RepoSecurityFinding &f = scan.findings.at(i);
                    detail += QStringLiteral("• %1 in %2 (line %3)\n")
                                  .arg(f.title, f.path)
                                  .arg(f.line);
                }
                if (scan.findings.size() > shown)
                    detail += QStringLiteral("  … and %1 more\n")
                                  .arg(scan.findings.size() - shown);

                QMessageBox box(this);
                box.setWindowTitle(QStringLiteral("Secret scanning: push blocked"));
                box.setIcon(QMessageBox::Critical);
                box.setText(
                    QStringLiteral(
                        "Push protection detected %1 probable secret%2 in the "
                        "commits being pushed for %3/%4.\n\n%5\n"
                        "Rotate any exposed credentials before pushing.")
                        .arg(scan.findings.size())
                        .arg(scan.findings.size() == 1 ? QString() : QStringLiteral("s"))
                        .arg(repo.owner, repo.name, detail));
                auto *viewBtn = box.addButton(QStringLiteral("View code"),
                                              QMessageBox::ActionRole);
                // "&&" — a single "&" would be eaten as a mnemonic and render
                // as "Mark safe _commit".
                auto *markSafeBtn = box.addButton(QStringLiteral("Mark safe && commit"),
                                                  QMessageBox::YesRole);
                auto *cancelBtn = box.addButton(QStringLiteral("Cancel push"),
                                                QMessageBox::RejectRole);
                auto *bypassBtn = box.addButton(QStringLiteral("Push anyway"),
                                                QMessageBox::DestructiveRole);
                box.setDefaultButton(cancelBtn);
                box.exec();
                if (box.clickedButton() == markSafeBtn) {
                    QSet<QString> touchedPaths;
                    int marked = 0;
                    for (const RepoSecurityFinding &f : scan.findings) {
                        if (RepoSecurity::markFindingSafe(repo.localPath, f)) {
                            touchedPaths.insert(f.path);
                            ++marked;
                        }
                    }
                    const int total = scan.findings.size();
                    if (touchedPaths.isEmpty()) {
                        m_pushingRepos.remove(index);
                        clearRepoSyncActivity(index);
                        refreshRepoSyncIndicators();
                        flashMessage(QStringLiteral("Could not mark the flagged "
                                                    "lines as safe — open one to "
                                                    "check it still reads as "
                                                    "flagged."),
                                     true);
                        return;
                    }
                    struct MarkSafeCommit {
                        bool clean = false;
                        bool committed = false;
                        QString error;
                    };
                    const QString localPath = repo.localPath;
                    runOffThread<MarkSafeCommit>(
                        [localPath, touchedPaths] {
                            MarkSafeCommit result;
                            QStringList statusArgs{QStringLiteral("status"),
                                                   QStringLiteral("--porcelain"),
                                                   QStringLiteral("--")};
                            for (const QString &path : std::as_const(touchedPaths))
                                statusArgs << path;
                            QByteArray statusOut;
                            runGitCapture(localPath, statusArgs, &statusOut, nullptr);
                            if (QString::fromUtf8(statusOut).trimmed().isEmpty()) {
                                result.clean = true;
                                return result;
                            }
                            QStringList commitArgs{QStringLiteral("commit"),
                                                   QStringLiteral("-m"),
                                                   QStringLiteral("Mark flagged secret scan "
                                                                  "findings as reviewed/safe"),
                                                   QStringLiteral("--")};
                            for (const QString &path : std::as_const(touchedPaths))
                                commitArgs << path;
                            result.committed = runGitCapture(localPath, commitArgs,
                                                             nullptr, &result.error);
                            return result;
                        },
                        [this, index, repo, marked, total](MarkSafeCommit result) {
                            m_pushingRepos.remove(index);
                            clearRepoSyncActivity(index);
                            refreshRepoSyncIndicators();
                            if (result.clean) {
                                flashMessage(QStringLiteral("The flagged lines already "
                                                            "carry the reviewed-safe "
                                                            "marker; nothing to commit."),
                                             true);
                                return;
                            }
                            if (!result.committed) {
                                flashMessage(QStringLiteral("Could not commit the "
                                                            "reviewed-safe markers: %1")
                                                 .arg(result.error.trimmed()),
                                             true);
                                return;
                            }
                            logSystem(QStringLiteral(
                                          "Git: marked %1 of %2 finding%3 as reviewed/safe "
                                          "in %4/%5 and committed the change.")
                                          .arg(marked)
                                          .arg(total)
                                          .arg(total == 1 ? QString() : QStringLiteral("s"))
                                          .arg(repo.owner, repo.name));
                            if (marked < total)
                                flashMessage(QStringLiteral("%1 of %2 flagged lines could "
                                                            "not be marked safe; rescanning.")
                                                 .arg(total - marked)
                                                 .arg(total),
                                             true);
                            if (index == m_repoDetailIndex && index >= 0 &&
                                index < m_repositories.size() &&
                                m_repositories.at(index).localPath == repo.localPath)
                                pushCurrentRepoUpstream();
                        });
                    return;
                }
                if (box.clickedButton() == viewBtn) {
                    // Jumping to the code cancels the push: the point is to remove
                    // the credential first. Copy the path/line out before the
                    // navigation below, which pumps the event loop (and can rebuild
                    // the findings' owning state) across its git reads.
                    const QString path = scan.findings.first().path;
                    const int line = scan.findings.first().line;
                    m_pushingRepos.remove(index);
                    clearRepoSyncActivity(index);
                    refreshRepoSyncIndicators();
                    openRepoDetail(index);
                    openRepoFileAtLine(path, line);
                    return;
                }
                if (box.clickedButton() != bypassBtn) {
                    m_pushingRepos.remove(index);
                    clearRepoSyncActivity(index);
                    refreshRepoSyncIndicators();
                    return;
                }
                logSystem(
                    QStringLiteral(
                        "Git: secret-scan bypass: pushing %1/%2 despite %3 finding%4.")
                        .arg(repo.owner, repo.name)
                        .arg(scan.findings.size())
                        .arg(scan.findings.size() == 1 ? QString() : QStringLiteral("s")));
            }

            if (isRelay) {
                m_pushingRepos.remove(index);
                setRepoSyncActivity(
                    index, QStringLiteral("Publishing local commits to the "
                                          "served mirror…"));
                logSystem(QStringLiteral("Git: publishing local commits for %1/%2 to "
                                         "the served mirror.")
                              .arg(repo.owner, repo.name));
                syncRepository(index, /*quiet=*/false);
                refreshRepoSyncIndicators();
                return;
            }
            startRepoPush(index, repo, upstream, scan.ahead);
        });
}

// Kick off the actual (already-async) `git push`, wiring up the completion/error
// handlers. Split out of pushCurrentRepoUpstream so the off-thread secret scan can
// resume here on the GUI thread once the push is approved. The repo is assumed to
// already be marked in m_pushingRepos.
void MainWindow::startRepoPush(int index, const RepositoryRecord &repo,
                               const QString &upstream, int ahead)
{
    refreshRepoSyncIndicators();
    logSystem(QStringLiteral("Git: pushing %1/%2 to %3.")
                  .arg(repo.owner, repo.name, upstream));

    auto *process = new QProcess(this);
    setRepoSyncActivity(index, QStringLiteral("Pushing to %1…").arg(upstream));
    auto pushErrors = streamGitProgressActivity(process, index, QString());
    connect(process, &QProcess::finished, this,
            [this, process, index, repo, upstream, ahead, pushErrors](
                int exitCode, QProcess::ExitStatus status) {
                if (process->property("handled").toBool())
                    return;
                process->setProperty("handled", true);
                pushErrors->append(
                    QString::fromUtf8(process->readAllStandardError()));
                const QString errors = gitErrorsWithoutProgress(*pushErrors);
                process->deleteLater();
                m_pushingRepos.remove(index);
                clearRepoSyncActivity(index);

                if (status == QProcess::NormalExit && exitCode == 0) {
                    const QString count =
                        ahead > 0 ? QString::number(ahead) + QLatin1Char(' ') : QString();
                    logSystem(QStringLiteral("Git: pushed %1commit%2 from %3/%4 to %5.")
                                  .arg(count,
                                       ahead == 1 ? QString() : QStringLiteral("s"),
                                       repo.owner, repo.name, upstream));
                    flashMessage(QStringLiteral("Pushed %1/%2 to %3.")
                                     .arg(repo.owner, repo.name, upstream));
                    if (index >= 0 && index < m_repositories.size()) {
                        if (!m_repositories.at(index).mirrorPath.isEmpty())
                            syncRepository(index, /*quiet=*/true);
                        else if (index == m_repoDetailIndex)
                            refreshOpenRepoDetail();
                    }
                } else {
                    const QString detail =
                        errors.isEmpty() ? QStringLiteral("git push failed")
                                         : errors.right(300);
                    const QString explained =
                        forkmesh::gitrefusal::explainFailure(errors);
                    logSystem(QStringLiteral("Git: push failed for %1/%2: %3")
                                  .arg(repo.owner, repo.name, detail));
                    flashMessage(QStringLiteral("Push failed for %1/%2: %3")
                                     .arg(repo.owner, repo.name,
                                          explained.isEmpty() ? detail.left(160)
                                                              : explained),
                                 true);
                }
                refreshRepoSyncIndicators();
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, index, repo](QProcess::ProcessError) {
                if (process->property("handled").toBool())
                    return;
                process->setProperty("handled", true);
                process->deleteLater();
                m_pushingRepos.remove(index);
                clearRepoSyncActivity(index);
                logSystem(QStringLiteral("Git: could not start push for %1/%2.")
                              .arg(repo.owner, repo.name));
                flashMessage(QStringLiteral("Could not run git push for %1/%2.")
                                 .arg(repo.owner, repo.name),
                             true);
                refreshRepoSyncIndicators();
            });
    process->start(QStringLiteral("git"),
                   {QStringLiteral("-C"), repo.localPath, QStringLiteral("push"),
                    QStringLiteral("--progress")});
}

void MainWindow::showRepoMenu()
{
    if (!m_repoMenuButton || m_repoDetailIndex < 0 ||
        m_repoDetailIndex >= m_repositories.size())
        return;
    const QString owner = m_repositories.at(m_repoDetailIndex).owner;
    QList<const RepoMenuEntry *> choices;
    for (const RepoMenuEntry &entry : std::as_const(m_repoMenuEntries)) {
        if (entry.index >= 0 && entry.index < m_repositories.size() &&
            m_repositories.at(entry.index).owner == owner)
            choices.append(&entry);
    }
    if (choices.size() <= 1)
        return;
    QMenu menu(this);
    menu.setToolTipsVisible(true);

    QAction *header = menu.addAction(
        QStringLiteral("%1 repositories (%2)")
            .arg(owner, formatCount(choices.size())));
    header->setEnabled(false);

    auto *searchEdit = new QLineEdit(&menu);
    searchEdit->setPlaceholderText(QStringLiteral("Search repositories") +
                                   QString::fromUtf8("\xE2\x80\xA6"));
    searchEdit->setClearButtonEnabled(true);
    searchEdit->setMinimumWidth(260);
    auto *searchAction = new QWidgetAction(&menu);
    searchAction->setDefaultWidget(searchEdit);
    menu.addAction(searchAction);
    menu.addSeparator();

    QList<QAction *> repoActions;
    QStringList repoNames;
    QList<QPair<QString, QString>> statInputs; // display label, checkout path
    QList<QPointer<QAction>> statActions;
    for (const RepoMenuEntry *entry : std::as_const(choices)) {
        const RepoMenuEntry &e = *entry;
        QString actionLabel = e.label;
        QString statDir;
        if (e.index >= 0 && e.index < m_repositories.size()) {
            statDir = m_repositories.at(e.index).localPath.trimmed();
            if (!statDir.isEmpty())
                actionLabel += QStringLiteral("   | Loading...");
        }
        QAction *act = menu.addAction(e.icon, actionLabel);
        if (!statDir.isEmpty()) {
            statInputs.append(qMakePair(e.label, statDir));
            statActions.append(QPointer<QAction>(act));
        }
        if (!e.detail.isEmpty())
            act->setToolTip(e.detail);
        const int index = e.index;
        const QString advertised = e.advertised;
        connect(act, &QAction::triggered, this, [this, index, advertised] {
            if (index >= 0 && index < m_repositories.size())
                openRepoDetailDeferred(index, /*preservePage=*/true);
            else if (index == -2)              // advertised mirror: temporary preview
                previewAdvertisedRepo(advertised);
            updateRepoSwitcher();
        });
        repoActions.append(act);
        repoNames.append(e.label.toLower());
    }

    // A repository may contain millions of files or sit on slow storage. The
    // menu must already be interactive while git computes these summaries.
    runOffThread<QStringList>(
        [statInputs] {
            QStringList labels;
            labels.reserve(statInputs.size());
            for (const auto &input : statInputs) {
                const QString &baseLabel = input.first;
                const QString &dir = input.second;
                QByteArray statusOut;
                QByteArray numstatOut;
                if (!runGitCapture(
                        dir,
                        {QStringLiteral("status"),
                         QStringLiteral("--porcelain=v1"),
                         QStringLiteral("--untracked-files=all")},
                        &statusOut, nullptr))
                    statusOut.clear();
                runGitCapture(dir,
                              {QStringLiteral("diff"), QStringLiteral("--numstat"),
                               QStringLiteral("HEAD"), QStringLiteral("--")},
                              &numstatOut, nullptr);
                const int changedFiles = QString::fromUtf8(statusOut)
                                             .split(QLatin1Char('\n'),
                                                    Qt::SkipEmptyParts)
                                             .size();
                qlonglong additions = 0;
                qlonglong deletions = 0;
                for (const QString &line :
                     QString::fromUtf8(numstatOut).split(QLatin1Char('\n'),
                                                           Qt::SkipEmptyParts)) {
                    const QStringList fields = line.split(QLatin1Char('\t'));
                    bool additionsOk = false;
                    bool deletionsOk = false;
                    const qlonglong added =
                        fields.value(0).toLongLong(&additionsOk);
                    const qlonglong deleted =
                        fields.value(1).toLongLong(&deletionsOk);
                    if (additionsOk)
                        additions += added;
                    if (deletionsOk)
                        deletions += deleted;
                }
                // One no-index Git process per untracked file made generated
                // trees (for example Rust target/) take minutes and flooded
                // telemetry with BGBLOCK entries. The tracked numstat is a
                // bounded two-process summary; the file count still includes
                // every staged, unstaged, and untracked path.
                labels.append(baseLabel + QStringLiteral("   | %1 %2 | ")
                                  .arg(changedFiles)
                                  .arg(changedFiles == 1
                                           ? QStringLiteral("file")
                                           : QStringLiteral("files")) +
                              QStringLiteral("+%1 -%2")
                                  .arg(additions)
                                  .arg(deletions));
            }
            return labels;
        },
        [statActions](const QStringList &labels) {
            const int count = qMin(statActions.size(), labels.size());
            for (int i = 0; i < count; ++i)
                if (statActions.at(i))
                    statActions.at(i)->setText(labels.at(i));
        });

    menu.addSeparator();
    QAction *newAct = menu.addAction(QStringLiteral("New repository") +
                                     QString::fromUtf8("\xE2\x80\xA6"));
    connect(newAct, &QAction::triggered, this, &MainWindow::createNewRepository);
    QAction *addAct = menu.addAction(QStringLiteral("Add local repo") +
                                     QString::fromUtf8("\xE2\x80\xA6"));
    connect(addAct, &QAction::triggered, this, &MainWindow::promptAddRepository);

    connect(searchEdit, &QLineEdit::textChanged, &menu,
            [repoActions, repoNames](const QString &text) {
                const QString needle = text.trimmed().toLower();
                for (int i = 0; i < repoActions.size(); ++i)
                    repoActions.at(i)->setVisible(needle.isEmpty() ||
                                                  repoNames.at(i).contains(needle));
            });
    QTimer::singleShot(0, searchEdit, [searchEdit] { searchEdit->setFocus(); });

    menu.exec(topBarMenuAnchor(m_repoMenuButton));
}

// Somebody just created a ForkMesh account while this node was connected. The
// relay signs and originates that frame — peers can only send encrypted
// envelopes — so this is the same celebration the World and the website run at
// the same moment: confetti and fireworks across the window, and a ping on the
// Pings page that opens #welcome so this node can be one of the people who
// says hello.
void MainWindow::onNewMemberJoined(const QString &accountName)
{
    const QString name = accountName.trimmed();
    if (name.isEmpty())
        return;
    const QString room = welcomeChannelForIdentity();
    const QString roomLabel = chatConversationLabel(room);
    logSystem(QStringLiteral("🎉 %1 just joined ForkMesh. Say hello in %2.")
                  .arg(name, roomLabel));
    new forkmesh::ui::CelebrationOverlay(this);
    AppNotification item;
    item.title = QStringLiteral("🎉 Welcome %1 — they just joined ForkMesh.").arg(name);
    item.body = QStringLiteral("Open %1 and say hello.").arg(roomLabel);
    item.kind = QStringLiteral("desktop");
    item.link.kind = QStringLiteral("chat");
    item.link.ref = room;
    item.quiet = true;
    recordNotification(item);
}

void MainWindow::showChatView()
{
    const QString unread = mostRecentUnreadConversation();
    refreshChatWorkspaceUser();
    showSection(2);
    if (!unread.isEmpty())
        switchConversation(unread);
    if (m_officeChannelMirror)
        m_officeChannelMirror->refresh();
    if (m_discordClient)
        m_discordClient->refresh();
}

QString MainWindow::mostRecentUnreadConversation() const
{
    if (m_unread.isEmpty() || m_unread.contains(m_currentConversation))
        return QString();
    QStringList ordered = m_channels;
    ordered.reserve(m_channels.size() + m_openDms.size());
    for (const QString &peerId : std::as_const(m_openDms))
        ordered.append(dmKey(peerId));
    // A conversation can hold unread while its sidebar row is missing: an
    // office room the mirror has not re-registered yet, a channel the relay
    // has not re-advertised since launch. Those still have to be reachable,
    // otherwise the badge counts messages no click can open or clear. Sorted,
    // so the pick stays the same across runs when timestamps tie.
    QStringList unlisted;
    for (const QString &conversation : std::as_const(m_unread)) {
        if (!ordered.contains(conversation))
            unlisted.append(conversation);
    }
    unlisted.sort();
    ordered += unlisted;

    QString best;
    qint64 bestStamp = -1;
    for (const QString &conversation : std::as_const(ordered)) {
        if (!m_unread.contains(conversation))
            continue;
        qint64 stamp = 0;
        const auto it = m_history.constFind(conversation);
        if (it != m_history.constEnd() && !it->isEmpty())
            stamp = it->last().timestampMs;
        if (stamp > bestStamp) {
            bestStamp = stamp;
            best = conversation;
        }
    }
    return best;
}

void MainWindow::updateChatButton()
{
    if (!m_chatButton)
        return;

    int total = 0;
    for (const int n : std::as_const(m_unreadCounts))
        total += n;

    if (auto *railButton = dynamic_cast<ActivityRailButton *>(m_chatButton))
        railButton->setBadgeCount(total);

    m_chatButton->setToolTip(
        total > 0 ? QString::fromUtf8("Chat \xE2\x80\x94 %1 unread message%2")
                        .arg(total)
                        .arg(total == 1 ? QString() : QStringLiteral("s"))
                  : QStringLiteral("Chat"));

    updateChatUnreadBanner();
}

void MainWindow::updateChatUnreadBanner()
{
    refreshChatSidebarChrome();
    if (!m_chatUnreadBanner)
        return;
    int total = 0;
    QList<std::pair<qint64, QString>> where;
    for (auto it = m_unreadCounts.constBegin(); it != m_unreadCounts.constEnd();
         ++it) {
        if (it.value() <= 0)
            continue;
        total += it.value();
        const auto messages = m_history.constFind(it.key());
        const qint64 stamp = messages != m_history.constEnd() && !messages->isEmpty()
                                 ? messages->last().timestampMs
                                 : 0;
        where.append({stamp, it.key()});
    }
    if (total <= 0) {
        m_chatUnreadBanner->hide();
        return;
    }
    std::sort(where.begin(), where.end(), [](const auto &a, const auto &b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    QStringList names;
    for (const auto &entry : std::as_const(where)) {
        if (names.size() == 3)
            break;
        names.append(chatConversationLabel(entry.second));
    }
    QString place;
    if (where.size() == 1)
        place = names.value(0);
    else if (where.size() <= 3)
        place = names.mid(0, names.size() - 1).join(QStringLiteral(", ")) +
                QStringLiteral(" and ") + names.last();
    else
        place = names.join(QStringLiteral(", ")) +
                QStringLiteral(" and %1 more").arg(where.size() - 3);
    if (m_chatUnreadBannerLabel)
        m_chatUnreadBannerLabel->setText(
            QStringLiteral("%1 unread message%2 in %3")
                .arg(total)
                .arg(total == 1 ? QString() : QStringLiteral("s"))
                .arg(place));
    bool elsewhere = false;
    for (const auto &entry : std::as_const(where))
        elsewhere = elsewhere || entry.second != m_currentConversation;
    if (m_chatUnreadJumpButton) {
        m_chatUnreadJumpButton->setVisible(elsewhere);
        m_chatUnreadJumpButton->setToolTip(
            QStringLiteral("Open %1").arg(names.value(0)));
    }
    m_chatUnreadBanner->show();
}

void MainWindow::openNextUnreadConversation()
{
    QString best;
    qint64 bestStamp = -1;
    QStringList candidates;
    for (auto it = m_unreadCounts.constBegin(); it != m_unreadCounts.constEnd();
         ++it) {
        if (it.value() > 0 && it.key() != m_currentConversation)
            candidates.append(it.key());
    }
    for (const QString &conversation : std::as_const(m_unread)) {
        if (conversation != m_currentConversation &&
            !candidates.contains(conversation))
            candidates.append(conversation);
    }
    candidates.sort();
    for (const QString &conversation : std::as_const(candidates)) {
        const auto messages = m_history.constFind(conversation);
        const qint64 stamp = messages != m_history.constEnd() && !messages->isEmpty()
                                 ? messages->last().timestampMs
                                 : 0;
        if (stamp > bestStamp) {
            bestStamp = stamp;
            best = conversation;
        }
    }
    if (best.isEmpty()) {
        m_stickToBottom = true;
        scrollToBottom();
        return;
    }
    if (m_sectionStack && m_sectionStack->currentIndex() != 2)
        showSection(2);
    switchConversation(best);
    m_stickToBottom = true;
    scrollToBottom();
}

QStringList MainWindow::unlistedChatConversations() const
{
    QStringList candidates(m_unread.cbegin(), m_unread.cend());
    if (!m_currentConversation.isEmpty() &&
        !candidates.contains(m_currentConversation))
        candidates.append(m_currentConversation);
    QStringList unlisted;
    for (const QString &conversation : std::as_const(candidates)) {
        if (conversation.isEmpty() || m_channels.contains(conversation))
            continue;
        if (isDirectConversation(conversation)) {
            if (m_openDms.contains(dmPeerId(conversation)))
                continue;
        } else if (forkmesh::office::isDirectOfficeConversation(conversation)) {
            if (m_officeDirectConversations.contains(conversation))
                continue;
        } else if (!conversation.startsWith(QLatin1Char('#'))) {
            continue;
        }
        if (m_history.value(conversation).isEmpty())
            continue;
        unlisted.append(conversation);
    }
    unlisted.sort();
    return unlisted;
}

QString MainWindow::chatConversationLabel(const QString &conversation) const
{
    if (isDirectConversation(conversation))
        return kDmPrefix + m_dmNames.value(dmPeerId(conversation),
                                           QStringLiteral("unknown"));
    if (forkmesh::office::isDirectOfficeConversation(conversation))
        return QStringLiteral("@") +
               forkmesh::office::directPeerForConversation(conversation);
    if (isDiscordConversation(conversation) && m_discordClient)
        return m_discordClient->displayName(conversation);
    if (forkmesh::office::isOfficeConversation(conversation))
        return officeChannelLabel(conversation);
    if (conversation == forkmesh::office::worldConversation())
        return forkmesh::office::worldLabel();
    return conversation;
}

QString MainWindow::officeChannelLabel(const QString &conversation) const
{
    const QString room = forkmesh::office::roomForConversation(conversation);
    if (room.isEmpty())
        return conversation;
    const QString label = QStringLiteral("#") + room;
    const QString organization =
        forkmesh::office::organizationForConversation(conversation);
    if (organization.isEmpty())
        return label;
    for (const QString &other : m_channels) {
        if (other == conversation)
            continue;
        const QString otherLabel =
            forkmesh::office::isOfficeConversation(other)
                ? QStringLiteral("#") +
                      forkmesh::office::roomForConversation(other)
            : other == forkmesh::office::worldConversation()
                ? forkmesh::office::worldLabel()
                : other;
        if (otherLabel == label)
            return label + QStringLiteral(" · ") + organization;
    }
    return label;
}

bool MainWindow::chatPeerOnline(const QString &peerId,
                                const QString &peerName) const
{
    for (const MemberInfo &member : m_homeRoster) {
        if (!member.online)
            continue;
        if (!peerId.isEmpty() && member.id == peerId)
            return true;
        if (!peerName.isEmpty() &&
            member.name.compare(peerName, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void MainWindow::refreshChatWorkspaceUser()
{
    if (!m_chatWorkspaceUser)
        return;
    const QString name = chatDisplayName().trimmed();
    const QString shown = name.isEmpty() ? QStringLiteral("you") : name;
    const bool online = m_backend != nullptr;
    m_chatWorkspaceUser->setText(
        QStringLiteral("<div style='color:%1; font-weight:600'>%2</div>"
                       "<div style='color:%3; font-size:12px'>%4</div>")
            .arg(Theme::chatColor(QStringLiteral("chatTextStrong")).name(),
                 shown.toHtmlEscaped(),
                 Theme::chatColor(QStringLiteral("chatMuted")).name(),
                 online ? QStringLiteral("Online") : QStringLiteral("Offline")));
    if (m_chatWorkspaceAvatar) {
        const QString owner = accountOwner().trimmed().toLower();
        const QPixmap face = chatActorAvatar(
            owner.isEmpty() ? QString() : QStringLiteral("user:") + owner, shown,
            kChatUserAvatarSide);
        m_chatWorkspaceAvatar->setPixmap(withPresenceDot(
            face,
            Theme::chatColor(online ? QStringLiteral("chatOnline")
                                    : QStringLiteral("chatFaint")),
            Theme::chatColor(QStringLiteral("chatRailFooter"))));
    }
}

void MainWindow::refreshChatSidebarChrome()
{
    if (!m_chatAllUnreadButton)
        return;
    int total = 0;
    for (const int n : std::as_const(m_unreadCounts))
        total += qMax(0, n);
    const bool unread = total > 0;
    if (m_chatAllUnreadButton->property("unread").toBool() != unread) {
        m_chatAllUnreadButton->setProperty("unread", unread);
        m_chatAllUnreadButton->style()->unpolish(m_chatAllUnreadButton);
        m_chatAllUnreadButton->style()->polish(m_chatAllUnreadButton);
    }
    if (m_chatAllUnreadBadge) {
        m_chatAllUnreadBadge->setText(total > 99 ? QStringLiteral("99+")
                                                 : QString::number(total));
        m_chatAllUnreadBadge->setVisible(unread);
    }
}

#ifdef FORKMESH_WINDOW_TESTS
QString MainWindow::testChatUnreadBannerText() const
{
    return m_chatUnreadBanner && !m_chatUnreadBanner->isHidden() &&
                   m_chatUnreadBannerLabel
               ? m_chatUnreadBannerLabel->text()
               : QString();
}

bool MainWindow::testChatUnreadJumpShown() const
{
    return m_chatUnreadJumpButton && !m_chatUnreadJumpButton->isHidden();
}

QString MainWindow::testChatAllUnreadBadge() const
{
    return m_chatAllUnreadBadge && !m_chatAllUnreadBadge->isHidden()
               ? m_chatAllUnreadBadge->text()
               : QString();
}

QStringList MainWindow::testChatDmRows() const
{
    QStringList rows;
    for (int i = 0; m_dmList && i < m_dmList->count(); ++i)
        rows.append(m_dmList->item(i)->data(Qt::UserRole).toString());
    return rows;
}

namespace {

QVariant chatListRowData(QListWidget *dms, QTreeWidget *channels,
                         const QString &conversation, int role)
{
    for (int row = 0; dms && row < dms->count(); ++row)
        if (dms->item(row)->data(Qt::UserRole).toString() == conversation)
            return dms->item(row)->data(role);
    if (!channels)
        return QVariant();
    for (QTreeWidgetItemIterator it(channels); *it; ++it)
        if (!ChannelGroupTree::isGroupItem(*it) &&
            (*it)->data(0, Qt::UserRole).toString() == conversation)
            return (*it)->data(0, role);
    return QVariant();
}

}

QString MainWindow::testChatListPreview(const QString &conversation) const
{
    return chatListRowData(m_dmList, m_channelList, conversation,
                           ChannelGroupTree::kPreviewRole)
        .toString();
}

qint64 MainWindow::testChatListPreviewTime(const QString &conversation) const
{
    return chatListRowData(m_dmList, m_channelList, conversation,
                           ChannelGroupTree::kPreviewTimeRole)
        .toLongLong();
}

QPixmap MainWindow::testChatListAvatar(const QString &conversation) const
{
    return chatListRowData(m_dmList, m_channelList, conversation,
                           ChannelGroupTree::kAvatarRole)
        .value<QPixmap>();
}

int MainWindow::testChatListRowHeight(const QString &conversation) const
{
    for (int row = 0; m_dmList && row < m_dmList->count(); ++row)
        if (m_dmList->item(row)->data(Qt::UserRole).toString() == conversation)
            return m_dmList->visualItemRect(m_dmList->item(row)).height();
    if (!m_channelList)
        return -1;
    for (QTreeWidgetItemIterator it(m_channelList); *it; ++it)
        if (!ChannelGroupTree::isGroupItem(*it) &&
            (*it)->data(0, Qt::UserRole).toString() == conversation)
            return m_channelList->visualItemRect(*it).height();
    return -1;
}

QString MainWindow::testChatTopicText() const
{
    return m_channelTopicLabel ? m_channelTopicLabel->text() : QString();
}
#endif

void MainWindow::markAllChatRead()
{
    for (const QString &conversation : std::as_const(m_unread))
        queueReadMark(conversation);
    if (!m_unread.isEmpty() || !m_unreadCounts.isEmpty()) {
        m_unread.clear();
        m_unreadCounts.clear();
        refreshChannelList();
        refreshDmList();
        updateChatButton();
        scheduleChatSave();
    }
    m_stickToBottom = true;
    scrollToBottom();
}

bool MainWindow::isChatViewVisible() const
{
    return isActiveWindow() && m_sectionStack &&
           m_sectionStack->currentIndex() == 2;
}

void MainWindow::clearActiveConversationUnread()
{
    if (!isChatViewVisible() || m_currentConversation.isEmpty())
        return;
    queueReadMark(m_currentConversation);
    if (m_unread.remove(m_currentConversation)) {
        m_unreadCounts.remove(m_currentConversation);
        refreshChannelList();
        refreshDmList();
        scheduleChatSave();
    }
    updateChatButton();
}

qint64 MainWindow::newestChatTimestamp(const QString &conversation) const
{
    const auto it = m_history.constFind(conversation);
    return it == m_history.constEnd() || it->isEmpty()
               ? 0
               : it->constLast().timestampMs;
}

void MainWindow::queueReadMark(const QString &conversation)
{
    const qint64 ts = newestChatTimestamp(conversation);
    if (ts <= m_readMarks.value(conversation) ||
        ts > QDateTime::currentMSecsSinceEpoch() + 24LL * 60 * 60 * 1000)
        return;
    m_pendingReadMarks.insert(conversation, ts);
    if (!m_readMarkTimer) {
        m_readMarkTimer = new QTimer(this);
        m_readMarkTimer->setSingleShot(true);
        m_readMarkTimer->setInterval(1000);
        connect(m_readMarkTimer, &QTimer::timeout, this,
                &MainWindow::flushReadMarks);
    }
    if (!m_readMarkTimer->isActive())
        m_readMarkTimer->start();
}

void MainWindow::flushReadMarks()
{
    const QHash<QString, qint64> marks = std::exchange(m_pendingReadMarks, {});
    for (auto it = marks.constBegin(); it != marks.constEnd(); ++it)
        m_readMarks.insert(it.key(),
                           qMax(m_readMarks.value(it.key()), it.value()));
    if (m_officeChannelMirror && !marks.isEmpty())
        m_officeChannelMirror->postReadMarks(marks);
}

void MainWindow::fetchReadMarks()
{
    if (m_officeChannelMirror)
        m_officeChannelMirror->fetchReadMarks();
}

void MainWindow::applyReadMarks(const QHash<QString, qint64> &marks)
{
    bool changed = false;
    for (auto it = marks.constBegin(); it != marks.constEnd(); ++it) {
        const qint64 mark = qMax(m_readMarks.value(it.key()), it.value());
        m_readMarks.insert(it.key(), mark);
        if (m_unread.contains(it.key()) &&
            newestChatTimestamp(it.key()) <= mark) {
            m_unread.remove(it.key());
            m_unreadCounts.remove(it.key());
            changed = true;
        }
    }
    if (!changed)
        return;
    refreshChannelList();
    refreshDmList();
    updateChatButton();
    scheduleChatSave();
}

QWidget *MainWindow::buildSolanaNotice()
{
    m_solanaBanner = new QWidget;
    m_solanaBanner->setObjectName("solanaBanner");
    m_solanaBannerLabel = new QLabel(
        "Add a public self-custodial Solana payout address if you want this "
        "mirror to be considered by community reward policies.");
    m_solanaBannerLabel->setObjectName("solanaBannerLabel");
    m_solanaBannerLabel->setWordWrap(true);

    auto *addButton = new QPushButton("Add Solana address");
    addButton->setObjectName("primaryButton");
    addButton->setCursor(Qt::PointingHandCursor);
    setOcticon(addButton, "plus", 16);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::promptSetSolanaAddress);

    auto *dismissButton = new QPushButton(QString());
    dismissButton->setObjectName("ghostButton");
    dismissButton->setCursor(Qt::PointingHandCursor);
    dismissButton->setToolTip("Hide for now");
    setOcticon(dismissButton, "x", 16);
    connect(dismissButton, &QPushButton::clicked, m_solanaBanner, &QWidget::hide);

    auto *layout = new QHBoxLayout(m_solanaBanner);
    layout->setContentsMargins(16, 10, 12, 10);
    layout->setSpacing(10);
    layout->addWidget(m_solanaBannerLabel, 1);
    layout->addWidget(addButton);
    layout->addWidget(dismissButton);
    m_solanaBanner->hide();
    return m_solanaBanner;
}

void MainWindow::updateSolanaNotice()
{
    if (!m_solanaBanner)
        return;
    // Crypto is strictly opt-in, so we no longer nag every account-less node to
    // add a payout address. The only prompt to set one is the explicit mirror
    // reward-settings button on the node profile; the banner stays hidden here.
    m_solanaBanner->setVisible(false);
    updateWalletVerifyNotice();
    updateNavSolanaBalance();
}

QWidget *MainWindow::buildWalletVerifyNotice()
{
    m_walletVerifyBanner = new QWidget;
    m_walletVerifyBanner->setObjectName("walletVerifyBanner");

    auto *icon = new QLabel;
    icon->setObjectName("walletVerifyIcon");
    icon->setPixmap(themedOcticon("alert", QColor("#d29922"), 22).pixmap(22, 22));

    auto *title = new QLabel("Check payout-address eligibility");
    title->setObjectName("walletVerifyTitle");
    auto *body = new QLabel(
        "Some community reward policies require an active public address. "
        "ForkMesh can validate its format and publish the existing signed node "
        "heartbeat, but neither proves wallet control nor guarantees selection "
        "or payment.<br><b>Non-custodial:</b> ForkMesh accepts only the public "
        "address here. Never enter a private key or recovery phrase; seeds and "
        "mnemonics are also prohibited.");
    body->setObjectName("walletVerifyBody");
    body->setWordWrap(true);
    body->setTextFormat(Qt::RichText);

    auto *textCol = new QVBoxLayout;
    textCol->setContentsMargins(0, 0, 0, 0);
    textCol->setSpacing(2);
    textCol->addWidget(title);
    textCol->addWidget(body);

    auto *verifyButton = new QPushButton("Check settings");
    verifyButton->setObjectName("primaryButton");
    verifyButton->setCursor(Qt::PointingHandCursor);
    setOcticon(verifyButton, "shield-check", 16);
    connect(verifyButton, &QPushButton::clicked, this, &MainWindow::verifyWallet);

    auto *dismissButton = new QPushButton(QString());
    dismissButton->setObjectName("ghostButton");
    dismissButton->setCursor(Qt::PointingHandCursor);
    dismissButton->setToolTip("Hide for now");
    setOcticon(dismissButton, "x", 16);
    connect(dismissButton, &QPushButton::clicked, m_walletVerifyBanner,
            &QWidget::hide);

    auto *layout = new QHBoxLayout(m_walletVerifyBanner);
    layout->setContentsMargins(16, 12, 12, 12);
    layout->setSpacing(12);
    layout->addWidget(icon, 0, Qt::AlignTop);
    layout->addLayout(textCol, 1);
    layout->addWidget(verifyButton, 0, Qt::AlignVCenter);
    layout->addWidget(dismissButton, 0, Qt::AlignTop);

    m_walletVerifyBanner->hide();
    return m_walletVerifyBanner;
}

void MainWindow::updateWalletVerifyNotice()
{
    if (!m_walletVerifyBanner)
        return;
    // Like the Solana banner above, the payout-address eligibility nag is
    // retired: crypto is opt-in, and the reward-settings button on the node
    // profile is the only place to check it.
    m_walletVerifyBanner->setVisible(false);
}

void MainWindow::promptSetSolanaAddress()
{
    bool ok = false;
    const QString current = savedSolanaAddress();
    const QString address = QInputDialog::getText(
        this, "Self-custodial payout address",
        "Enter your public Solana payout address only. Never enter a private "
        "key, seed, mnemonic, or recovery phrase:",
        QLineEdit::Normal,
        current, &ok);
    if (!ok)
        return;
    const QString trimmed = address.trimmed();
    if (!trimmed.isEmpty() &&
        !forkmesh::control::isValidSolanaPublicAddress(trimmed)) {
        flashMessage(
            QStringLiteral(
                "That is not a valid Solana public address. No private key or "
                "recovery phrase was accepted."),
            /*error=*/true);
        return;
    }
    saveSolanaAddress(trimmed);
    if (m_solanaEdit)
        m_solanaEdit->setText(trimmed);
    if (m_settingsSolanaEdit)
        m_settingsSolanaEdit->setText(trimmed);
    updateSolanaNotice();
    updateHomeStats();
}

void MainWindow::enablePaidMirroring()
{
    // Crypto is strictly opt-in: the core flow (clone, mirror, issues, PRs, chat)
    // never routes here. This page only configures a public payout address for
    // an already registered, locally signable mirror identity. It must never
    // launch the historical reserve/donation/finalize funnel.
    QString address = savedSolanaAddress().trimmed();
    if (address.isEmpty()) {
        promptSetSolanaAddress();
        address = savedSolanaAddress().trimmed();
        if (address.isEmpty())
            return; // user cancelled the address prompt — stay opted out
    }
    if (!forkmesh::control::isValidSolanaPublicAddress(address)) {
        flashMessage(
            QStringLiteral(
                "That saved value is not a valid Solana public address. No "
                "private key, seed, mnemonic, or recovery phrase can be used."),
            /*error=*/true);
        return;
    }

    if (!hasOwnerSigningCapability(accountOwner())) {
        flashMessage(
            QStringLiteral(
                "Register or sign in to this node from Account settings first. "
                "Mirror reward settings never create a deposit wallet, reserve "
                "a paid account, or run a donation flow."),
            /*error=*/true);
        return;
    }

    startNodeEventSocket();

    updateSolanaNotice();
    updateHomeStats();
    if (m_nodeProfilePanel && m_nodeProfilePanel->isVisible())
        showNodeProfile(m_profileNodeId, m_profileNodeName,
                        /*navigate=*/!profilePanelInSettings());
    flashMessage(QStringLiteral(
        "Reward eligibility configured. Selection and payment are not guaranteed."));
}

void MainWindow::ensureSectionBuilt(int index)
{
    if (!m_sectionStack || index < 0 || index >= m_sectionStack->count())
        return;
    QWidget *placeholder = m_sectionStack->widget(index);
    if (!placeholder ||
        !placeholder->property("forkmeshDeferredSection").toBool())
        return;

    QWidget *section = nullptr;
    switch (index) {
    case 1: {
        auto *scroll = new QScrollArea;
        scroll->setObjectName("settingsScroll");
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setWidgetResizable(true);
        scroll->setMinimumHeight(0);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
        m_sectionStack->insertWidget(index, scroll);
        m_sectionStack->removeWidget(placeholder);
        placeholder->deleteLater();
        scroll->setWidget(buildSettingsSection());
        return;
    }
    case 2: section = buildChatSection(); break;
    case 3: section = buildNotificationsSection(); break;
    case 4: section = buildLogSection(); break;
    case 6: section = buildSearchResultsSection(); break;
    case 10: section = buildNodeProfileSection(); break;
    case 11: section = buildNetworkReposSection(); break;
    case 12: section = buildNetworkDiagnosticsSection(); break;
    case 14: section = buildControlNodeSection(); break;
    case 15: section = buildOrganizationTasksSection(); break;
    case 16: section = buildNotesSection(); break;
    case 17: section = buildUsersSection(); break;
    case 18: section = buildFilesSection(); break;
    case 19: section = buildCalendarSection(); break;
    case kMarketSectionIndex: section = buildMarketSection(); break;
    case kLedgerSectionIndex: section = buildLedgerSection(); break;
    case kTeamSectionIndex: section = buildTeamSection(); break;
    case kStatsSectionIndex: section = buildStatsSection(); break;
    case kComponentHealthSectionIndex:
        section = buildComponentHealthSection();
        break;
    case kMemorySectionIndex: section = buildMemorySection(); break;
    case kHomeGridSectionIndex: section = buildHomeGridSection(); break;
    case kGovernorSectionIndex: section = buildGovernorSection(); break;
    case kProjectSettingsSectionIndex: section = buildProjectSettingsSection(); break;
    default: break;
    }
    if (!section)
        return;
    m_sectionStack->insertWidget(index, section);
    m_sectionStack->removeWidget(placeholder);
    placeholder->deleteLater();
}

void MainWindow::catchUpLogPage()
{
    if (!m_settingsLog)
        return;
    if (m_networkLogViewStale) {
        m_networkLogViewStale = false;
        rebuildNetworkLogView();
    }
    if (m_logTimelineViewStale) {
        m_logTimelineViewStale = false;
        updateLogTimelineSummary();
        if (m_logViewTabs &&
            m_logViewTabs->currentIndex() == m_logAggregationTabIndex)
            refreshLogFrequencyChart();
    }
    m_settingsLog->moveCursor(QTextCursor::End);
    setNetworkLogFollowing(true); // opening the page starts on the live tail
    refreshCurrentDesktopPackageLogPage();
}

void MainWindow::showSection(int index)
{
    if (index == kUsersSectionIndex && !m_isAdmin) {
        if (m_sectionStack &&
            m_sectionStack->currentIndex() == kUsersSectionIndex)
            index = kNetworkDiagnosticsSectionIndex;
        else
            return;
    }
    // Ledger is a removable package, not permanent chrome. Removing it while
    // its page is open must not leave the reader parked on a section whose
    // rail entry has just disappeared.
    if (index == kLedgerSectionIndex && !m_ledgerFeature) {
        if (m_sectionStack &&
            m_sectionStack->currentIndex() == kLedgerSectionIndex)
            index = kMarketSectionIndex;
        else
            return;
    }
    if (index == 9)
        index = kNetworkDiagnosticsSectionIndex;
    if (index == 7)
        return showNetworkTab(kNetworkHostsTab);
    if (index == 8)
        return showNodesForRole(QStringLiteral("relay"));
    if (index == kNodesSectionIndex)
        return showNetworkTab(kNetworkNodesTab);
    if (index == 5)
        index = 0;
    ensureSectionBuilt(index);
    if (index != 1 && m_voiceTestRecording)
        stopMicTest();
    if (m_navGroup && m_navGroup->button(index))
        m_navGroup->button(index)->setChecked(true);
    if (m_sectionStack)
        m_sectionStack->setCurrentIndex(index);
    scheduleNavRecord();
    updateBreadcrumb();
    if (index == 0)
        updateHomeStats();
    else if (index == 1) {
        syncSettingsProfileTab();
    } else if (index == 2) {
        clearActiveConversationUnread();
    } else if (index == 3) {
        markLocalPingsSeen();
        updateNotificationButton();
        refreshWebAlerts(true);
        refreshNotificationsTable();
    } else if (index == 4) {
        catchUpLogPage();
    } else if (index == 10) {
        hostNodeProfilePanel(false);
    } else if (index == kNetworkReposSectionIndex) {
        refreshNetworkReposPage();
    } else if (index == kNetworkDiagnosticsSectionIndex) {
        refreshFirewallTables();
        refreshNetworkDiagnostics();
        refreshNetworkWebRequests();
        // Roster frames that arrived while this section was off-screen left
        // the Nodes table unbuilt; bring it current now that it can be seen.
        if (m_nodesTableStale && m_networkTabs &&
            m_networkTabs->currentIndex() == kNetworkNodesTab)
            refreshNodesTable();
    } else if (index == kControlNodeSectionIndex) {
        refreshControlNode();
    } else if (index == kOrganizationTasksSectionIndex) {
        // Load the list once — on startup, or here if that pull never landed.
        // Coming back to the page later reuses it; Refresh is the way to ask
        // the relay again.
        if (!m_organizationTasksLoaded)
            refreshOrganizationTasks();
        refreshOrganizationTaskAgentRows();
        refreshChatUserDirectory();
    } else if (index == kNotesSectionIndex) {
        refreshNotes();
    } else if (index == kUsersSectionIndex) {
        refreshUsersPage();
    } else if (index == kFilesSectionIndex) {
        refreshFilesPage();
    } else if (index == kStatsSectionIndex) {
        refreshStatsPage();
    } else if (index == kComponentHealthSectionIndex) {
        refreshComponentHealth();
    } else if (index == kMemorySectionIndex) {
        refreshMemoryPage();
    } else if (index == kHomeGridSectionIndex) {
        refreshHomeGridPage();
        if (m_homeGridBoard && m_homeGridSearch && m_homeGridSearch->isHidden())
            m_homeGridBoard->setFocus(Qt::OtherFocusReason);
    } else if (index == kGovernorSectionIndex) {
        refreshGovernorPage();
    } else if (index == kCalendarSectionIndex) {
        refreshCalendar();
    } else if (index == kMarketSectionIndex) {
        refreshMarketPage();
        refreshAccountPackages(true);
    } else if (index == kLedgerSectionIndex) {
        refreshLedgerPage();
    } else if (index == kTeamSectionIndex) {
        refreshTeammateRepoChooser();
        loadTeammatesPanel();
    }
}

void MainWindow::showNetworkTab(int tabIndex)
{
    showSection(kNetworkDiagnosticsSectionIndex);
    if (!m_networkTabs || tabIndex < 0 || tabIndex >= m_networkTabs->count())
        return;
    const bool alreadyOpen = m_networkTabs->currentIndex() == tabIndex;
    m_networkTabs->setCurrentIndex(tabIndex);
    if (alreadyOpen)
        refreshNetworkTab(tabIndex);
}

void MainWindow::refreshCurrentNetworkTab()
{
    const int tabIndex = m_networkTabs ? m_networkTabs->currentIndex() : -1;
    if (tabIndex == kNetworkNodesTab) {
        // Switching to Nodes reuses what the relay last told us; asking for a
        // refresh means the user wants it fetched again.
        refreshChatUserDirectory();
        fetchRelayOnlineNodes(true); // refreshes the table again on reply
        fetchNodesCatalogInfo(true);
        m_catalogMirrorsFetchedMs = 0;
    }
    // The endpoint and websocket counters sit in this button's own header row,
    // so keep them current whichever tab is open. refreshNetworkTab() already
    // does it for the tabs that show diagnostics themselves.
    if (tabIndex >= 0 && tabIndex <= kNetworkHostsTab)
        refreshNetworkDiagnostics();
    refreshNetworkTab(tabIndex);
}

void MainWindow::clearRepoDetail()
{
    m_repoDetailIndex = -1;
    renderPageNotices();
    m_repoInfo = RepoInfo();
    m_repoBranch.clear();
    if (m_repoHeaderTitle)
        m_repoHeaderTitle->clear();
    loadBranchesAndTags();
    updateRepoCodeSize();
    updateRepoDetailStatus();
    updateFooterGitIdentity();
    updateFooterCommitInfo();
    reloadIssues();
    reloadAgents();
    updateRepoIssueCount();
    m_currentDiscussions.clear();
    reloadDiscussions();
    updateRepoDiscussionCount();
    m_currentPulls.clear();
    refreshPullList();
    updateRepoPullCount();
    loadCommits();
    loadRepoOverview(QString());
    for (QWidget *editor : std::as_const(m_openCoveExplorerTabs)) {
        const int tab = m_repoFileTabs ? m_repoFileTabs->indexOf(editor) : -1;
        if (tab >= 0)
            m_repoFileTabs->removeTab(tab);
        if (editor)
            editor->deleteLater();
    }
    m_openCoveExplorerTabs.clear();
    if (m_coveExplorerTree)
        m_coveExplorerTree->clear();
    if (m_coveExplorerSelector)
        m_coveExplorerSelector->clear();
    m_coveExplorerCoves.clear();
    m_coveExplorerCurrentId.clear();
    if (m_commitBar)
        m_commitBar->setText(
            "<span style='color:#8b949e'>This node has no repositories.</span>");
    if (m_commitBarAvatar)
        m_commitBarAvatar->hide(); // no commit, so no author to lead the line
    if (m_repoDetailStack)
        m_repoDetailStack->setCurrentIndex(0); // Code/overview
    if (m_filesStack)
        m_filesStack->setCurrentIndex(0);
    updateBreadcrumb();
}
