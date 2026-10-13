
#include "AddCoreNodeDialog.h"
#include "RemoveCoreNodeDialog.h"
#include "FleetAutoscalePanel.h"
#include "CoreNodeLogs.h"
#include "ForkMeshVersion.h"
#include "Tooltips.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
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
#include "HostLogSources.h"
#include "Http3Get.h"
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
#include "RunnerListView.h"
#include "RunnerLogin.h"
#include "WorldSpeechBridge.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QFormLayout>
#include <QInputDialog>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <memory>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSharedPointer>
#include <QUrlQuery>
#include <QUuid>
#include <QtConcurrentRun>

#include <atomic>
#include <utility>

#include <cmath>

#if defined(Q_OS_UNIX)
#include <signal.h>
#endif

using namespace forkmesh::ui;

namespace {
const QString kVultrInstallAgentClisSetting =
    QStringLiteral("hosts/vultrProvision/installAgentClis");
} // namespace

QString MainWindow::installScriptUrl() const
{
    QUrl url = catalogApiUrl(); // same relay host, http(s) scheme
    url.setPath(QStringLiteral("/install.sh"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString();
}

QString MainWindow::uninstallScriptUrl() const
{
    QUrl url = catalogApiUrl(); // same relay host, http(s) scheme
    url.setPath(QStringLiteral("/uninstall.sh"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url.toString();
}

QWidget *MainWindow::buildHostsSection()
{
    auto *page = new QWidget;
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 4, 0, 0);
    outer->setSpacing(6);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(6);

    auto *hostsLabel = new QLabel(QStringLiteral("Hosts"));
    QFont hlf = hostsLabel->font();
    hlf.setBold(true);
    hostsLabel->setFont(hlf);
    titleRow->addWidget(hostsLabel);
    titleRow->addWidget(makeInlineHelpButton(
        QStringLiteral("About saved hosts"), QString::fromUtf8(
            "Click Update on a saved host to re-run the installer and bring it up to "
            "the latest ForkMesh release; it also refreshes the Claude Code and Codex "
            "CLIs a mirror runs agents with. Click Uninstall to completely remove "
            "ForkMesh \xE2\x80\x94 binary, launcher and ALL data \xE2\x80\x94 from "
            "that host. Click Actions to enable its executor and optionally replace "
            "its device-local variables through a one-shot SSH stdin request; secret "
            "values are never saved by this controller. Click Logs to open a live "
            "SSH tail for that host, or Size map to browse what is filling that "
            "host's disk. Click Remove to drop a host from this list without "
            "touching it \xE2\x80\x94 no SSH session is opened. Double-click a host "
            "instead to reload it into the form below for editing.")));
    m_hostsFleetSummary = new QLabel;
    m_hostsFleetSummary->setObjectName(
        QStringLiteral("hostsFleetHealthSummary"));
    m_hostsFleetSummary->setWordWrap(false);
    titleRow->addWidget(m_hostsFleetSummary);
    titleRow->addStretch(1);
    m_hostsLiveViewButton = new QPushButton(QStringLiteral("Live view"));
    m_hostsLiveViewButton->setObjectName(QStringLiteral("hostsLiveViewButton"));
    m_hostsLiveViewButton->setCheckable(true);
    m_hostsLiveViewButton->setCursor(Qt::PointingHandCursor);
    m_hostsLiveViewButton->setToolTip(QStringLiteral(
        "Show every saved host side by side with a live tail of its node log "
        "and its network activity, each over its own SSH connection."));
    setOcticon(m_hostsLiveViewButton, QStringLiteral("broadcast"), 14);
    connect(m_hostsLiveViewButton, &QPushButton::toggled, this,
            &MainWindow::setHostsLiveViewActive);
    titleRow->addWidget(m_hostsLiveViewButton);
    m_hostsProbeButton = new QPushButton(QStringLiteral("Check all now"));
    m_hostsProbeButton->setObjectName(
        QStringLiteral("hostsProbeAllButton"));
    m_hostsProbeButton->setCursor(Qt::PointingHandCursor);
    m_hostsProbeButton->setToolTip(QStringLiteral(
        "Check SSH reachability, ForkMesh, Claude, and Codex on every saved "
        "host."));
    setOcticon(m_hostsProbeButton, QStringLiteral("sync"), 14);
    connect(m_hostsProbeButton, &QPushButton::clicked, this, [this] {
        probeSavedHosts();
    });
    titleRow->addWidget(m_hostsProbeButton);
    m_hostInstallAllButton = new QPushButton(QStringLiteral("Install from binary (all hosts)"));
    m_hostInstallAllButton->setCursor(Qt::PointingHandCursor);
    m_hostInstallAllButton->setToolTip(QStringLiteral(
        "Install the published ForkMesh v" FORKMESH_VERSION
        " binary on every saved host. Each host verifies the release checksum, "
        "version and exact source commit, keeps its identity, keys and mirrored "
        "data, and restarts its node."));
    setOcticon(m_hostInstallAllButton, "download", 14);
    connect(m_hostInstallAllButton, &QPushButton::clicked, this,
            &MainWindow::runHostInstallAllFromBinary);
    titleRow->addWidget(m_hostInstallAllButton);
    m_hostReinstallAllButton =
        new QPushButton(QStringLiteral("Uninstall + reinstall (all hosts)"));
    m_hostReinstallAllButton->setCursor(Qt::PointingHandCursor);
    m_hostReinstallAllButton->setToolTip(QStringLiteral(
        "For every saved host: remove ForkMesh and ALL of its data, then "
        "install the published ForkMesh v" FORKMESH_VERSION
        " binary and re-link it to your account."));
    setOcticon(m_hostReinstallAllButton, "sync", 14);
    connect(m_hostReinstallAllButton, &QPushButton::clicked, this,
            &MainWindow::runHostReinstallAllFromBinary);
    titleRow->addWidget(m_hostReinstallAllButton);
    m_hostUpdateAllSourceButton =
        new QPushButton(QStringLiteral("Update from source (all hosts)"));
    m_hostUpdateAllSourceButton->setCursor(Qt::PointingHandCursor);
    m_hostUpdateAllSourceButton->setToolTip(QStringLiteral(
        "For every saved host: SSH in, pull the latest ForkMesh source, rebuild "
        "the client from it and restart the node — without publishing a release. "
        "The node's identity key and mirrored data are kept."));
    setOcticon(m_hostUpdateAllSourceButton, "sync", 14);
    connect(m_hostUpdateAllSourceButton, &QPushButton::clicked, this,
            &MainWindow::runHostUpdateAllFromSource);
    titleRow->addWidget(m_hostUpdateAllSourceButton);
    outer->addLayout(titleRow);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("hostsPageBody"));
    auto *bodyCol = new QVBoxLayout(body);
    bodyCol->setObjectName(QStringLiteral("hostsPageLayout"));
    bodyCol->setContentsMargins(0, 0, 0, 0);
    bodyCol->setSpacing(10);

    auto *formCard = new QFrame;
    formCard->setObjectName("leaderboardCard");
    formCard->setFrameShape(QFrame::StyledPanel);
    auto *formCol = new QVBoxLayout(formCard);
    formCol->setContentsMargins(12, 10, 12, 10);
    formCol->setSpacing(6);

    auto *formTitleRow = new QHBoxLayout;
    formTitleRow->setContentsMargins(0, 0, 0, 0);
    formTitleRow->setSpacing(6);
    auto *formTitle = new QLabel(QStringLiteral("Add a host"));
    QFont ftf = formTitle->font();
    ftf.setBold(true);
    formTitle->setFont(ftf);
    formTitleRow->addWidget(formTitle);
    formTitleRow->addWidget(makeInlineHelpButton(
        QStringLiteral("About adding a host"), QString::fromUtf8(
            "Provision a remote machine onto the network. Enter its address and SSH "
            "login and give it a node name, then click Add host to save it. With the "
            "host saved, click Install ForkMesh and it will SSH in and run the hosted "
            "installer in a plain shell. ForkMesh remembers the first host key in "
            "its private trust store and rejects later mismatches. Your SSH agent, "
            "default keys and ~/.ssh/config are used when the optional password is "
            "blank; entered passwords remain in memory only. When installation "
            "finishes the new node joins the network and shows up in each "
            "Network > Nodes page. Install (binary) on one saved host "
            "uploads this app's own binary. Install from binary (all hosts) instead "
            "makes every host download and checksum-verify the current published "
            "release, then confirms the installed version and exact source commit.")));
    formTitleRow->addStretch(1);
    formCol->addLayout(formTitleRow);

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);
    form->setSpacing(6);

    m_hostIpEdit = new QLineEdit;
    m_hostIpEdit->setPlaceholderText(QStringLiteral("203.0.113.10"));
    form->addRow(QStringLiteral("Host IP / address"), m_hostIpEdit);

    m_hostUserEdit = new QLineEdit;
    m_hostUserEdit->setPlaceholderText(QStringLiteral("root"));
    form->addRow(QStringLiteral("SSH username"), m_hostUserEdit);

    m_hostPassEdit = new QLineEdit;
    m_hostPassEdit->setEchoMode(QLineEdit::Password);
    m_hostPassEdit->setPlaceholderText(
        QStringLiteral("Optional — the shared ForkMesh key is used first"));
    m_hostPassEdit->setToolTip(QStringLiteral(
        "Leave blank when this host already authorizes the shared ForkMesh SSH "
        "key (every fleet host does); ForkMesh falls back to your SSH agent, "
        "default key, or ~/.ssh/config. A password entered here is only used "
        "if the shared key is refused, is kept until ForkMesh exits, and is "
        "never written to settings."));
    form->addRow(QStringLiteral("SSH password (optional)"), m_hostPassEdit);

    m_hostNameEdit = new QLineEdit;
    m_hostNameEdit->setPlaceholderText(QStringLiteral("my-mirror-1"));
    form->addRow(QStringLiteral("Node name"), m_hostNameEdit);

    m_hostUploadBinaryCheck =
        new QCheckBox(QStringLiteral("Upload the release from this app"));
    m_hostUploadBinaryCheck->setToolTip(QString::fromUtf8(
        "Stream this app's own release binary to the host over the SSH "
        "session, instead of the host downloading the release from the "
        "network. Useful when the host cannot reach the release download, or "
        "to push exactly the build you are running. If the host's OS or "
        "architecture does not match this machine, the installer falls back "
        "to the normal download."));
    form->addRow(QString(), m_hostUploadBinaryCheck);
    formCol->addLayout(form);

    auto *runRow = new QHBoxLayout;
    runRow->setContentsMargins(0, 0, 0, 0);
    m_hostAddButton = new QPushButton(QStringLiteral("Add host"));
    m_hostAddButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_hostAddButton, "plus", 14);
    connect(m_hostAddButton, &QPushButton::clicked, this,
            &MainWindow::addHostFromForm);
    runRow->addWidget(m_hostAddButton);
    m_hostInstallButton = new QPushButton(QStringLiteral("Install ForkMesh"));
    m_hostInstallButton->setObjectName("primaryButton");
    m_hostInstallButton->setCursor(Qt::PointingHandCursor);
    setOcticon(m_hostInstallButton, "rocket", 14);
    connect(m_hostInstallButton, &QPushButton::clicked, this,
            [this] { runHostInstall(); });
    runRow->addWidget(m_hostInstallButton);
    m_hostInstallStatus = new QLabel;
    m_hostInstallStatus->setObjectName("mutedLabel");
    m_hostInstallStatus->setWordWrap(true);
    runRow->addWidget(m_hostInstallStatus, 1);
    formCol->addLayout(runRow);

    auto *vultrCard = new QFrame;
    vultrCard->setObjectName("leaderboardCard");
    vultrCard->setFrameShape(QFrame::StyledPanel);
    auto *vultrCol = new QVBoxLayout(vultrCard);
    vultrCol->setContentsMargins(12, 10, 12, 10);
    vultrCol->setSpacing(6);

    auto *vultrTitleRow = new QHBoxLayout;
    vultrTitleRow->setContentsMargins(0, 0, 0, 0);
    vultrTitleRow->setSpacing(6);
    auto *vultrTitle = new QLabel(QStringLiteral("Create a Vultr mirror"));
    QFont vtf = vultrTitle->font();
    vtf.setBold(true);
    vultrTitle->setFont(vtf);
    vultrTitleRow->addWidget(vultrTitle);
    vultrTitleRow->addWidget(makeInlineHelpButton(
        QStringLiteral("About Vultr mirrors"), QString::fromUtf8(
            "One click deploys a brand-new cloud mirror on your Vultr account: "
            "ForkMesh picks the cheapest available US IPv4 plan with at least 1 GB "
            "RAM, preferring New Jersey/New York metro and then Atlanta (smaller "
            "plans cannot hold the encrypted mirror's temporary working set; "
            "Vultr's IPv6-only tiers are also unreachable for the mesh) running the "
            "latest Debian, creates and manages the SSH key for it automatically, "
            "boots the instance, installs ForkMesh over SSH and links the new node "
            "to your account so it starts mirroring and syncing right away. The API "
            "key (Vultr panel \xE2\x86\x92 Account \xE2\x86\x92 API) is saved once "
            "Vultr accepts it \xE2\x80\x94 into this device's VULTR_API_KEY variable "
            "(Settings \xE2\x86\x92 Variables / Secrets) and into "
            "app/.env.production \xE2\x80\x94 so you never have to "
            "enter it again; it travels only in the Authorization header of this "
            "app's HTTPS calls to Vultr, never in a command line. When a "
            "CLOUDFLARE_API_TOKEN device variable and a Cloudflare zone are "
            "configured, the new node also gets a <node>.<zone> DNS record so it "
            "joins the mesh under a stable name like your other mirrors. The "
            "instance is billed by Vultr to your account until you destroy it there.")));
    vultrTitleRow->addStretch(1);
    vultrCol->addLayout(vultrTitleRow);

    m_vultrProgressPanel = new QWidget(vultrCard);
    m_vultrProgressPanel->setObjectName(
        QStringLiteral("vultrDeployProgress"));
    auto *stageRow = new QHBoxLayout(m_vultrProgressPanel);
    stageRow->setContentsMargins(0, 6, 0, 6);
    stageRow->setSpacing(5);
    m_vultrStageNumbers.clear();
    m_vultrStageLabels.clear();
    m_vultrStageDetails.clear();
    const QStringList stageNames = {
        QStringLiteral("Credentials"), QStringLiteral("Plan + image"),
        QStringLiteral("Create server"), QStringLiteral("Boot + connect"),
        QStringLiteral("Install"), QStringLiteral("Live traffic")};
    m_vultrStageDetailText.clear();
    m_vultrStageDetailText.reserve(stageNames.size());
    for (int i = 0; i < stageNames.size(); ++i)
        m_vultrStageDetailText.append(QString());
    for (int i = 0; i < stageNames.size(); ++i) {
        auto *stage = new QWidget(m_vultrProgressPanel);
        stage->setObjectName(QStringLiteral("vultrDeployStage%1").arg(i + 1));
        stage->setSizePolicy(QSizePolicy::Preferred,
                             QSizePolicy::MinimumExpanding);
        auto *col = new QVBoxLayout(stage);
        col->setContentsMargins(2, 0, 2, 0);
        col->setSpacing(3);
        auto *number = new QLabel(QString::number(i + 1), stage);
        number->setAlignment(Qt::AlignCenter);
        number->setFixedSize(28, 28);
        number->setAccessibleName(
            QStringLiteral("Deployment stage %1").arg(i + 1));
        auto *label = new QLabel(stageNames.at(i), stage);
        label->setAlignment(Qt::AlignCenter);
        label->setWordWrap(true);
        label->setMinimumWidth(72);
        auto *detail = new QLabel(stage);
        detail->setAlignment(Qt::AlignCenter);
        detail->setWordWrap(true);
        detail->setMinimumWidth(72);
        detail->setVisible(false);
        col->addWidget(number, 0, Qt::AlignHCenter);
        // Wrapped captions span the full column: an aligned layout item
        // sizes to sizeHint() and drops height-for-width, which clipped
        // multi-line stage details.
        col->addWidget(label);
        col->addWidget(detail);
        col->addStretch(1);
        m_vultrStageNumbers.append(number);
        m_vultrStageLabels.append(label);
        m_vultrStageDetails.append(detail);
        stageRow->addWidget(stage, 1);
        if (i + 1 < stageNames.size()) {
            auto *connector = new QFrame(m_vultrProgressPanel);
            connector->setFrameShape(QFrame::HLine);
            connector->setObjectName(QStringLiteral("vultrStageConnector"));
            connector->setMaximumWidth(22);
            stageRow->addWidget(connector);
        }
    }
    vultrCol->addWidget(m_vultrProgressPanel);
    m_vultrLiveBadge = new QLabel(vultrCard);
    m_vultrLiveBadge->setObjectName(QStringLiteral("vultrMirrorLiveBadge"));
    m_vultrLiveBadge->setAlignment(Qt::AlignCenter);
    m_vultrLiveBadge->setVisible(false);
    vultrCol->addWidget(m_vultrLiveBadge);

    auto *vultrForm = new QFormLayout;
    vultrForm->setLabelAlignment(Qt::AlignRight);
    vultrForm->setSpacing(6);
    m_vultrApiKeyEdit = new QLineEdit;
    m_vultrApiKeyEdit->setEchoMode(QLineEdit::Password);
    m_vultrApiKeyEdit->setPlaceholderText(
        QStringLiteral("Vultr API key — saved after the first successful run"));
    m_vultrApiKeyEdit->setText(
        forkmesh::control::vultrApiKeyFromVariables(ActionStore::variables()));
    vultrForm->addRow(QStringLiteral("Vultr API key"), m_vultrApiKeyEdit);
    m_vultrNameEdit = new QLineEdit;
    m_vultrNameEdit->setPlaceholderText(QString::fromUtf8(
        "Optional \xE2\x80\x94 defaults to the next free mirrorN "
        "(mirror5, mirror6, \xE2\x80\xA6)"));
    vultrForm->addRow(QStringLiteral("Node name"), m_vultrNameEdit);

    vultrCol->addLayout(vultrForm);

    m_vultrAgentClisCheck = new QCheckBox(QString::fromUtf8(
        "Also install Claude Code + Codex and sign them in with this device's "
        "access"));
    m_vultrAgentClisCheck->setObjectName(
        QStringLiteral("vultrInstallAgentClisCheck"));
    m_vultrAgentClisCheck->setChecked(
        QSettings().value(kVultrInstallAgentClisSetting, false).toBool());
    m_vultrAgentClisCheck->setToolTip(QString::fromUtf8(
        "After ForkMesh is installed, the official Claude Code and Codex CLIs "
        "are installed on the new mirror and this device's own logins "
        "(~/.claude/.credentials.json, ~/.codex/auth.json, and the agent API "
        "keys from Settings for a provider you have no CLI login for) are "
        "copied to it, so it can run agent sessions immediately. The "
        "credentials travel only on the SSH session's stdin \xE2\x80\x94 never "
        "in a command line or in the log below."));
    connect(m_vultrAgentClisCheck, &QCheckBox::toggled, this, [](bool enabled) {
        QSettings().setValue(kVultrInstallAgentClisSetting, enabled);
    });
    vultrCol->addWidget(m_vultrAgentClisCheck);

    auto *vultrRow = new QHBoxLayout;
    vultrRow->setContentsMargins(0, 0, 0, 0);
    m_vultrCreateButton = new QPushButton(QStringLiteral("Create Vultr mirror"));
    m_vultrCreateButton->setObjectName("primaryButton");
    m_vultrCreateButton->setCursor(Qt::PointingHandCursor);
    m_vultrCreateButton->setToolTip(QStringLiteral(
        "Deploy the cheapest Debian instance on your Vultr account, install "
        "ForkMesh v" FORKMESH_VERSION " on it and link it to your account. "
        "Progress streams into Live output below."));
    setOcticon(m_vultrCreateButton, "rocket", 14);
    connect(m_vultrCreateButton, &QPushButton::clicked, this,
            &MainWindow::createVultrMirrorFromForm);
    vultrRow->addWidget(m_vultrCreateButton);
    m_vultrEndDeploymentButton = new QPushButton(QStringLiteral("End deployment"));
    m_vultrEndDeploymentButton->setObjectName("ghostButton");
    m_vultrEndDeploymentButton->setCursor(Qt::PointingHandCursor);
    m_vultrEndDeploymentButton->setToolTip(QStringLiteral(
        "Stop ForkMesh from tracking the current deployment attempt so you can "
        "start a fresh one. Any Vultr server already created for it keeps "
        "running and billing until destroyed separately."));
    setOcticon(m_vultrEndDeploymentButton, "stop", 12);
    m_vultrEndDeploymentButton->setVisible(false);
    connect(m_vultrEndDeploymentButton, &QPushButton::clicked, this,
            &MainWindow::endVultrProvision);
    vultrRow->addWidget(m_vultrEndDeploymentButton);
    m_vultrStatus = new QLabel;
    m_vultrStatus->setObjectName("mutedLabel");
    m_vultrStatus->setWordWrap(true);
    m_vultrStatus->setStyleSheet(Theme::remapSheet(QStringLiteral("color:%1; font-size:11px;")
                                     .arg(currentThemeIsDark()
                                              ? QStringLiteral("#c9d1d9")
                                              : QStringLiteral("#57606a"))));
    vultrRow->addWidget(m_vultrStatus, 1);
    vultrCol->addLayout(vultrRow);
    auto *logLabel = new QLabel(QStringLiteral("Live output"));
    QFont llf = logLabel->font();
    llf.setBold(true);
    logLabel->setFont(llf);

    m_hostInstallLog = new QPlainTextEdit;
    m_hostInstallLog->setObjectName("actionLog");
    m_hostInstallLog->setReadOnly(true);
    m_hostInstallLog->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_hostInstallLog->setMinimumHeight(170);
    applyLogFont(m_hostInstallLog);
    m_hostInstallLog->setPlaceholderText(QString::fromUtf8(
        "The SSH session and installer output will stream here\xE2\x80\xA6"));

    m_hostDeployLabel = new QLabel(QStringLiteral("Live output — per host"));
    QFont dlf = m_hostDeployLabel->font();
    dlf.setBold(true);
    m_hostDeployLabel->setFont(dlf);
    m_hostDeployLabel->setVisible(false);

    m_hostDeployPanel = new QWidget;
    m_hostDeployGrid = new QGridLayout(m_hostDeployPanel);
    m_hostDeployGrid->setContentsMargins(0, 0, 0, 0);
    m_hostDeployGrid->setSpacing(10);
    m_hostDeployPanel->setVisible(false);

    m_hostsTable = new QTableWidget(0, 7);
    installColumnHeaderMenu(m_hostsTable);
    m_hostsTable->setObjectName("issueTable");
    m_hostsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Node name"), QStringLiteral("Address"),
         QStringLiteral("User"), QStringLiteral("Status"),
         QStringLiteral("Claude"), QStringLiteral("Codex"), QString()});
    m_hostsTable->verticalHeader()->setVisible(false);
    m_hostsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_hostsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hostsTable->setShowGrid(false);
    m_hostsTable->horizontalHeader()->setStretchLastSection(false);
    m_hostsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_hostsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_hostsTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_hostsTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    makeColumnsResizable(m_hostsTable); // spreadsheet-style draggable columns (#263)
    connect(m_hostsTable, &QTableWidget::cellDoubleClicked, this,
            &MainWindow::loadHostIntoForm);

    bodyCol->addWidget(m_hostsTable);
    auto *runnersTitleRow = new QHBoxLayout;
    runnersTitleRow->setContentsMargins(0, 8, 0, 0);
    auto *runnersLabel = new QLabel(QStringLiteral("Runners"));
    runnersLabel->setObjectName(QStringLiteral("hostsRunnersLabel"));
    runnersLabel->setFont(hlf);
    runnersTitleRow->addWidget(runnersLabel);
    runnersTitleRow->addWidget(makeInlineHelpButton(
        QStringLiteral("About runners"),
        QStringLiteral(
            "Runners are the app's cloud agent machines (for example "
            "fmnext-n1). They are not saved hosts and are never added to the "
            "list above. Each row shows whether its Claude Code and Codex "
            "CLIs are installed and signed in, as the runner last reported. "
            "Sign in runs the provider's own login on the runner and relays "
            "its terminal here; the credential stays on the runner. Listed "
            "for app admins, and for the account that registered a personal "
            "runner.")));
    runnersTitleRow->addStretch(1);
    auto *runnersRefresh = new QPushButton(QStringLiteral("Refresh runners"));
    runnersRefresh->setObjectName(QStringLiteral("hostsRunnersRefreshButton"));
    runnersRefresh->setCursor(Qt::PointingHandCursor);
    setOcticon(runnersRefresh, QStringLiteral("sync"), 14);
    connect(runnersRefresh, &QPushButton::clicked, this,
            [this] { refreshRunners(); });
    runnersTitleRow->addWidget(runnersRefresh);
    bodyCol->addLayout(runnersTitleRow);
    m_runnersStatus = new QLabel;
    m_runnersStatus->setObjectName(QStringLiteral("hostsRunnersStatus"));
    m_runnersStatus->setWordWrap(true);
    m_runnersStatus->setStyleSheet(Theme::remapSheet(
        QStringLiteral("color:#8b949e; font-size:11px;")));
    bodyCol->addWidget(m_runnersStatus);
    m_runnersTable = new QTableWidget(0, forkmesh::runners::kRunnerColumnCount);
    m_runnersTable->setObjectName(QStringLiteral("hostsRunnersTable"));
    forkmesh::runners::setUpRunnersTable(m_runnersTable);
    m_runnersTable->setMinimumHeight(90);
    bodyCol->addWidget(m_runnersTable);
    auto *fleetLabel = new QLabel(QStringLiteral(
        "ForkMesh fleet (cores, runners and autoscaled nodes; managed by the "
        "fleet tools, operators can add and remove cores)"));
    fleetLabel->setStyleSheet(Theme::remapSheet(QStringLiteral("color:#8b949e; font-size:11px;")));
    m_fleetNodesTable = new QTableWidget(0, 6);
    m_fleetNodesTable->setObjectName(QStringLiteral("fleetNodesTable"));
    m_fleetNodesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Node"), QStringLiteral("Role"), QStringLiteral("Label"),
         QStringLiteral("Address"), QStringLiteral("Status"), QString()});
    m_fleetNodesTable->verticalHeader()->setVisible(false);
    m_fleetNodesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fleetNodesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fleetNodesTable->setShowGrid(false);
    m_fleetNodesTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_fleetNodesTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    auto *fleetTitleRow = new QHBoxLayout;
    fleetTitleRow->setContentsMargins(0, 8, 0, 0);
    fleetTitleRow->addWidget(fleetLabel, 1);
    m_addCoreNodeButton = new QPushButton(QStringLiteral("Add core node"));
    m_addCoreNodeButton->setObjectName(QStringLiteral("hostsAddCoreNodeButton"));
    m_addCoreNodeButton->setCursor(Qt::PointingHandCursor);
    m_addCoreNodeButton->setToolTip(QStringLiteral(
        "Create a new ForkMesh core on Vultr and join it to the fleet: WireGuard "
        "mesh, rqlite, secrets, IDS, mirror daemon, the served release and every "
        "repository, then DNS once it is healthy. Operators only."));
    setOcticon(m_addCoreNodeButton, QStringLiteral("plus"), 14);
    connect(m_addCoreNodeButton, &QPushButton::clicked, this,
            &MainWindow::openAddCoreNodeDialog);
    fleetTitleRow->addWidget(m_addCoreNodeButton);
    bodyCol->addLayout(fleetTitleRow);
    bodyCol->addWidget(m_fleetNodesTable);
    if (QFileInfo::exists(forkmesh::corelogs::fleetInstancesPath())) {
        ensureAddCoreNodeController();
        ensureRemoveCoreNodeController();
    }
    updateAddCoreNodeButton();
    auto *provisioningRow = new QHBoxLayout;
    provisioningRow->setObjectName(QStringLiteral("hostsProvisioningRow"));
    provisioningRow->setContentsMargins(0, 0, 0, 0);
    provisioningRow->setSpacing(10);
    provisioningRow->addWidget(vultrCard, 1);
    provisioningRow->addWidget(formCard, 1);
    auto *autoscalePanel = new FleetAutoscalePanel(body);
    autoscalePanel->setScriptPath(forkmesh::control::findNextAutoscaleScript(
        QStringLiteral(FORKMESH_SOURCE_DIR), QCoreApplication::applicationDirPath()));
    bodyCol->addWidget(autoscalePanel);
    bodyCol->addLayout(provisioningRow);
    bodyCol->addWidget(logLabel);
    bodyCol->addWidget(m_hostInstallLog);
    bodyCol->addWidget(m_hostDeployLabel);
    bodyCol->addWidget(m_hostDeployPanel);
    if (!m_hostProbeTimer) {
        m_hostProbeTimer = new QTimer(this);
        m_hostProbeTimer->setInterval(30000);
        // Each probe is one SSH session per saved host, and the only things it
        // feeds are the Hosts table and its fleet summary. Off-screen that is
        // a remote process launch per host for nobody, so a hidden page does
        // not probe at all; refreshHostsTable() probes when the page is shown
        // and the page's Refresh button probes on demand.
        connect(m_hostProbeTimer, &QTimer::timeout, this, [this] {
            if (m_hostsTable && m_hostsTable->isVisible()) {
                probeSavedHosts();
                refreshFleetNodesTable();
                refreshRunners();
            }
        });
        m_hostProbeTimer->start();
    }

    scroll->setWidget(body);
    m_hostsViewStack = new QStackedWidget;
    m_hostsViewStack->setObjectName(QStringLiteral("hostsViewStack"));
    m_hostsViewStack->addWidget(scroll);
    m_hostsViewStack->addWidget(buildHostsLiveView());
    outer->addWidget(m_hostsViewStack, 1);

    refreshHostsTable();
    refreshFleetNodesTable();
    renderRunnersTable();
    QTimer::singleShot(0, this, [this] { refreshRunners(); });
    renderVultrProvisionProgress();
    restoreVultrProvision();
    QTimer::singleShot(0, this, &MainWindow::probeSavedHosts);
    return page;
}

void MainWindow::refreshHostsTable()
{
    // Every change to the saved host list ends up here, so the main log's
    // host feeds follow it too.
    syncHostLogFeeds();
    if (!m_hostsTable)
        return;
    // Coming back to the page after the slow off-screen cadence: bring the
    // reachability column current straight away rather than at the next tick.
    if (m_hostsTable->isVisible() && m_hostProbeTimer &&
        QDateTime::currentMSecsSinceEpoch() - m_hostProbeLastMs >
            m_hostProbeTimer->interval())
        QTimer::singleShot(0, this, [this] { probeSavedHosts(); });
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    m_hostsTable->setRowCount(hosts.size());
    for (int i = 0; i < hosts.size(); ++i) {
        const QJsonObject h = hosts.at(i).toObject();
        const QString status = h.value("status").toString(QStringLiteral("installed"));
        const QString name = h.value("name").toString();
        const QString ip = h.value("ip").toString();
        const QString user = h.value("user").toString();
        const QString key =
            forkmesh::control::savedHostCredentialKey(name, ip, user);
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setData(Qt::UserRole, key);
        m_hostsTable->setItem(i, 0, nameItem);
        m_hostsTable->setItem(i, 1,
            new QTableWidgetItem(ip));
        m_hostsTable->setItem(i, 2,
            new QTableWidgetItem(user));
        const QString reachability = m_hostReachability.value(key);
        const QString provider = h.value(QStringLiteral("provider")).toString();
        const QString planType = h.value(QStringLiteral("planType")).toString();
        const QString region = h.value(QStringLiteral("region")).toString();
        const double monthlyCost =
            h.value(QStringLiteral("monthlyCost")).toDouble(-1.0);
        QStringList hostFacts;
        if (!provider.isEmpty())
            hostFacts.append(provider);
        if (!planType.isEmpty())
            hostFacts.append(planType);
        if (!region.isEmpty())
            hostFacts.append(region);
        if (monthlyCost >= 0.0)
            hostFacts.append(QStringLiteral("$%1/mo").arg(
                QString::number(monthlyCost, 'f',
                                monthlyCost == qFloor(monthlyCost) ? 0 : 2)));
        const QString displayedStatus =
            reachability.isEmpty() ? status : reachability;
        m_hostsTable->setItem(
            i, 3,
            new QTableWidgetItem(
                hostFacts.isEmpty()
                    ? displayedStatus
                    : displayedStatus + QStringLiteral("\n") +
                          hostFacts.join(QStringLiteral(" · "))));
        m_hostsTable->setItem(
            i, 4,
            new QTableWidgetItem(
                m_hostClaudeAvailability.value(
                    key, QString::fromUtf8("Checking\xE2\x80\xA6"))));
        m_hostsTable->setItem(
            i, 5,
            new QTableWidgetItem(
                m_hostCodexAvailability.value(
                    key, QString::fromUtf8("Checking\xE2\x80\xA6"))));

        auto *cell = new QWidget;
        auto *cellRow = new QHBoxLayout(cell);
        cellRow->setContentsMargins(4, 2, 4, 2);
        cellRow->setSpacing(6);
        auto *updateBtn = new QPushButton(QStringLiteral("Update"));
        updateBtn->setCursor(Qt::PointingHandCursor);
        setOcticon(updateBtn, "sync", 12);
        connect(updateBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] {
                loadHostIntoForm(i, 0);
                runHostInstall();
            });
        });
        cellRow->addWidget(updateBtn);

        auto *installBinaryBtn = new QPushButton(QStringLiteral("Install (binary)"));
        installBinaryBtn->setCursor(Qt::PointingHandCursor);
        setOcticon(installBinaryBtn, "upload", 12);
        connect(installBinaryBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] {
                loadHostIntoForm(i, 0);
                runHostInstall(/*forceUploadBinary=*/true);
            });
        });
        cellRow->addWidget(installBinaryBtn);

        auto *uninstallBtn = new QPushButton(QStringLiteral("Uninstall"));
        uninstallBtn->setCursor(Qt::PointingHandCursor);
        setOcticon(uninstallBtn, "trash", 12);
        connect(uninstallBtn, &QPushButton::clicked, this, [this, i] {
            const QString name =
                m_hostsTable->item(i, 0) ? m_hostsTable->item(i, 0)->text() : QString();
            const auto reply = QMessageBox::question(
                this, QStringLiteral("Uninstall ForkMesh"),
                QString::fromUtf8(
                    "This completely removes ForkMesh from \"%1\": the binary, "
                    "launcher, node identity key and ALL mirrored repositories "
                    "and chat history on that host. This cannot be undone. "
                    "Continue?")
                    .arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes)
                return;
            QTimer::singleShot(0, this, [this, i] {
                loadHostIntoForm(i, 0);
                runHostUninstall();
            });
        });
        cellRow->addWidget(uninstallBtn);

        auto *destroyBtn = new QPushButton(QStringLiteral("Destroy"));
        destroyBtn->setObjectName(QStringLiteral("hostDestroyButton"));
        destroyBtn->setCursor(Qt::PointingHandCursor);
        destroyBtn->setToolTip(QStringLiteral(
            "Destroy this server on Vultr: the instance is deleted, billing "
            "stops and everything on it is gone permanently. Needs the Vultr "
            "API key from the field above (or a stored VULTR_API_KEY "
            "variable)."));
        setOcticon(destroyBtn, "alert", 12);
        connect(destroyBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] { destroyVultrHostAtRow(i); });
        });
        cellRow->addWidget(destroyBtn);

        auto *removeBtn = new QPushButton(QStringLiteral("Remove"));
        removeBtn->setCursor(Qt::PointingHandCursor);
        setOcticon(removeBtn, "x", 12);
        connect(removeBtn, &QPushButton::clicked, this, [this, i] {
            const QString name =
                m_hostsTable->item(i, 0) ? m_hostsTable->item(i, 0)->text() : QString();
            const auto reply = QMessageBox::question(
                this, QStringLiteral("Remove saved host"),
                QString::fromUtf8(
                    "Remove \"%1\" from this list? This only forgets it here "
                    "\xE2\x80\x94 ForkMesh is NOT uninstalled from that host.")
                    .arg(name),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (reply != QMessageBox::Yes)
                return;
            QTimer::singleShot(0, this, [this, i] { forgetHostAtRow(i); });
        });
        cellRow->addWidget(removeBtn);

        auto *viewLogsBtn = new QPushButton(QStringLiteral("Logs"));
        viewLogsBtn->setCursor(Qt::PointingHandCursor);
        setOcticon(viewLogsBtn, "terminal", 12);
        connect(viewLogsBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] {
                viewHostLogsForSelection(i);
            });
        });
        cellRow->addWidget(viewLogsBtn);

        // Per-row Size map button: browse that host's disk usage over the same
        // authenticated SSH channel, one read-only `du` level at a time, so a
        // host that is filling up can be diagnosed from here.
        auto *diskBtn = new QPushButton(QStringLiteral("Size map"));
        diskBtn->setObjectName(QStringLiteral("hostDiskUsageButton"));
        diskBtn->setCursor(Qt::PointingHandCursor);
        diskBtn->setToolTip(QStringLiteral(
            "Browse what is using disk space on this host over SSH. Read-only: "
            "it only runs du, one directory level at a time."));
        setOcticon(diskBtn, "pie-chart", 12);
        connect(diskBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] {
                browseHostDiskUsageForSelection(i);
            });
        });
        cellRow->addWidget(diskBtn);

        auto *actionsBtn = new QPushButton(QStringLiteral("Actions"));
        actionsBtn->setObjectName(QStringLiteral("hostActionsButton"));
        actionsBtn->setCursor(Qt::PointingHandCursor);
        actionsBtn->setToolTip(QStringLiteral(
            "Configure this mirror's Actions state and device-local variables "
            "over authenticated SSH. Values travel only on stdin."));
        setOcticon(actionsBtn, "workflow", 12);
        connect(actionsBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] {
                configureHostActionsForSelection(i);
            });
        });
        cellRow->addWidget(actionsBtn);

        auto *installAgentsBtn =
            new QPushButton(QStringLiteral("Install Claude + Codex"));
        installAgentsBtn->setObjectName(
            QStringLiteral("hostInstallAgentClisButton"));
        installAgentsBtn->setCursor(Qt::PointingHandCursor);
        installAgentsBtn->setToolTip(QStringLiteral(
            "Install the official user-scoped Claude Code and Codex CLI "
            "binaries on this mirror over its pinned SSH connection. This "
            "does not copy tokens or sign either provider in."));
        setOcticon(installAgentsBtn, "terminal", 12);
        connect(installAgentsBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] {
                installAgentClisForHost(i);
            });
        });
        cellRow->addWidget(installAgentsBtn);

        // The installer copies no provider tokens, so signing in is a separate
        // interactive step. This opens the same live shell the install opens on
        // its own, for a host whose CLIs are already there.
        auto *signInBtn = new QPushButton(QStringLiteral("Sign in"));
        signInBtn->setObjectName(QStringLiteral("hostAgentLoginButton"));
        signInBtn->setCursor(Qt::PointingHandCursor);
        signInBtn->setToolTip(QStringLiteral(
            "Open a live terminal on this mirror to finish the Claude Code and "
            "Codex sign-ins. What you type goes only to the host over its "
            "pinned SSH connection."));
        setOcticon(signInBtn, "key", 12);
        connect(signInBtn, &QPushButton::clicked, this, [this, i] {
            QTimer::singleShot(0, this, [this, i] {
                openHostAgentLoginTerminalForSelection(i);
            });
        });
        cellRow->addWidget(signInBtn);
        for (QPushButton *b : cell->findChildren<QPushButton *>())
            b->setProperty("buttonSize", "sm");
        m_hostsTable->setCellWidget(i, 6, cell);
    }
    if (!hosts.isEmpty()) {
        QTimer::singleShot(0, m_hostsTable, [this] {
            QWidget *cell = m_hostsTable ? m_hostsTable->cellWidget(0, 6) : nullptr;
            if (!cell)
                return;
            m_hostsTable->horizontalHeader()->setSectionResizeMode(
                6, QHeaderView::Interactive);
            m_hostsTable->setColumnWidth(6, cell->sizeHint().width() + 16);
            const int rowHeight = cell->sizeHint().height() + 12;
            for (int r = 0; r < m_hostsTable->rowCount(); ++r)
                m_hostsTable->setRowHeight(r, rowHeight);
        });
    }

    updateNetworkCounts(-1, -1, hosts.size());
    updateHostsFleetSummary();
}

void MainWindow::updateAddCoreNodeButton()
{
    if (!m_addCoreNodeButton)
        return;
    const QString script = forkmesh::control::findNextFleetScript(
        QStringLiteral(FORKMESH_SOURCE_DIR), QCoreApplication::applicationDirPath());
    const bool operatorDesktop =
        !script.isEmpty() &&
        QFileInfo::exists(forkmesh::corelogs::fleetInstancesPath());
    const bool running =
        m_addCoreNodeController && m_addCoreNodeController->isRunning();
    m_addCoreNodeButton->setVisible((m_isAdmin && operatorDesktop) || running);
    m_addCoreNodeButton->setText(running ? QStringLiteral("Adding core node\u2026")
                                         : QStringLiteral("Add core node"));
}

void MainWindow::ensureAddCoreNodeController()
{
    if (m_addCoreNodeController)
        return;
    m_addCoreNodeController = new forkmesh::fleet::AddCoreNodeController(
        forkmesh::control::findNextFleetScript(
            QStringLiteral(FORKMESH_SOURCE_DIR),
            QCoreApplication::applicationDirPath()),
        this);
    m_addCoreNodeController->setRunDirectory(
        QDir(forkmesh::corelogs::fleetLogsDirectory()).filePath(QStringLiteral("add-core")));
    connect(m_addCoreNodeController,
            &forkmesh::fleet::AddCoreNodeController::runChanged, this,
            &MainWindow::updateAddCoreNodeButton);
    connect(m_addCoreNodeController,
            &forkmesh::fleet::AddCoreNodeController::finished, this,
            [this](bool) {
                updateAddCoreNodeButton();
                refreshFleetNodesTable();
            });
    m_addCoreNodeController->reattach();
}

void MainWindow::openAddCoreNodeDialog()
{
    ensureAddCoreNodeController();
    if (!m_isAdmin && !m_addCoreNodeController->isRunning())
        return;
    if (m_addCoreNodeDialog) {
        m_addCoreNodeDialog->show();
        m_addCoreNodeDialog->raise();
        m_addCoreNodeDialog->activateWindow();
        return;
    }
    auto *dialog = new forkmesh::fleet::AddCoreNodeDialog(m_addCoreNodeController, this);
    m_addCoreNodeDialog = dialog;
    dialog->show();
}

void MainWindow::ensureRemoveCoreNodeController()
{
    if (m_removeCoreNodeController)
        return;
    m_removeCoreNodeController = new forkmesh::fleet::RemoveCoreNodeController(
        forkmesh::control::findNextFleetScript(
            QStringLiteral(FORKMESH_SOURCE_DIR),
            QCoreApplication::applicationDirPath()),
        this);
    m_removeCoreNodeController->setRunDirectory(
        QDir(forkmesh::corelogs::fleetLogsDirectory()).filePath(QStringLiteral("remove-core")));
    connect(m_removeCoreNodeController,
            &forkmesh::fleet::RemoveCoreNodeController::finished, this,
            [this](bool) { refreshFleetNodesTable(); });
    if (m_removeCoreNodeController->reattach())
        refreshFleetNodesTable();
}

void MainWindow::openRemoveCoreNodeDialog(const QString &name)
{
    ensureRemoveCoreNodeController();
    const bool running = m_removeCoreNodeController->isRunning();
    if (!m_isAdmin && !running)
        return;
    if (running && m_removeCoreNodeController->runName() != name)
        return;
    if (m_removeCoreNodeDialog) {
        auto *open = qobject_cast<forkmesh::fleet::RemoveCoreNodeDialog *>(
            m_removeCoreNodeDialog.data());
        if (open && open->nodeName() == name) {
            m_removeCoreNodeDialog->show();
            m_removeCoreNodeDialog->raise();
            m_removeCoreNodeDialog->activateWindow();
            return;
        }
        m_removeCoreNodeDialog->close();
    }
    auto *dialog =
        new forkmesh::fleet::RemoveCoreNodeDialog(m_removeCoreNodeController, name, this);
    m_removeCoreNodeDialog = dialog;
    dialog->show();
}

void MainWindow::refreshFleetNodesTable()
{
    updateAddCoreNodeButton();
    if (!m_fleetNodesTable)
        return;
    const QList<forkmesh::corelogs::CoreNode> nodes =
        forkmesh::corelogs::loadFleetNodes();
    const bool canRemove =
        m_isAdmin && QFileInfo::exists(forkmesh::corelogs::fleetInstancesPath()) &&
        !forkmesh::control::findNextFleetScript(QStringLiteral(FORKMESH_SOURCE_DIR),
                                                QCoreApplication::applicationDirPath())
             .isEmpty();
    int coreCount = 0;
    for (const forkmesh::corelogs::CoreNode &node : nodes) {
        if (forkmesh::fleet::isRemovableCoreName(node.name))
            ++coreCount;
    }
    const bool removing =
        m_removeCoreNodeController && m_removeCoreNodeController->isRunning();
    m_fleetNodesTable->setRowCount(nodes.size());
    for (int row = 0; row < nodes.size(); ++row) {
        const forkmesh::corelogs::CoreNode &node = nodes.at(row);
        const QStringList cells = {node.name, node.role, node.label, node.address};
        for (int column = 0; column < cells.size(); ++column)
            m_fleetNodesTable->setItem(row, column, new QTableWidgetItem(cells.at(column)));
        const bool core = node.name.startsWith(QLatin1Char('n'));
        m_fleetNodesTable->setItem(row, 4, new QTableWidgetItem(
            core ? QStringLiteral("Checking...") : QStringLiteral("Listed")));
        m_fleetNodesTable->removeCellWidget(row, 5);
        const bool runningHere =
            removing && m_removeCoreNodeController->runName() == node.name;
        if (forkmesh::fleet::isRemovableCoreName(node.name) && (canRemove || runningHere)) {
            auto *removeBtn = new QPushButton(
                runningHere ? QStringLiteral("Removing\u2026") : QStringLiteral("Remove\u2026"));
            removeBtn->setObjectName(QStringLiteral("fleetNodeRemoveButton"));
            removeBtn->setProperty("buttonSize", "sm");
            removeBtn->setCursor(Qt::PointingHandCursor);
            setOcticon(removeBtn, QStringLiteral("trash"), 12);
            const QString blocker =
                forkmesh::fleet::coreCountBlocker(node.name, coreCount);
            const bool otherRunning = removing && !runningHere;
            removeBtn->setEnabled(runningHere || (blocker.isEmpty() && !otherRunning));
            removeBtn->setToolTip(
                runningHere ? QStringLiteral("Show the running removal of %1.").arg(node.name)
                : otherRunning
                    ? QStringLiteral("Another core is being removed; one removal at a time.")
                : !blocker.isEmpty()
                    ? blocker
                    : QStringLiteral("Take %1 out of the fleet: DNS drain, rqlite removal, "
                                     "peer update and a fleet.toml branch, with an option "
                                     "to delete its cloud instance. Shows the plan first.")
                          .arg(node.name));
            const QString name = node.name;
            connect(removeBtn, &QPushButton::clicked, this, [this, name] {
                QTimer::singleShot(0, this, [this, name] { openRemoveCoreNodeDialog(name); });
            });
            m_fleetNodesTable->setCellWidget(row, 5, removeBtn);
        }
        if (!core)
            continue;
        if (!m_networkAccess)
            m_networkAccess = new QNetworkAccessManager(this);
        QNetworkRequest request(QUrl(QStringLiteral("https://%1.next.forkmesh.com/readyz")
                                         .arg(node.name)));
        request.setTransferTimeout(8000);
        QNetworkReply *reply = m_networkAccess->get(request);
        const QString name = node.name;
        connect(reply, &QNetworkReply::finished, this, [this, reply, name] {
            reply->deleteLater();
            if (!m_fleetNodesTable)
                return;
            const QJsonObject body =
                QJsonDocument::fromJson(reply->read(64 * 1024)).object();
            QString status;
            if (reply->error() != QNetworkReply::NoError)
                status = QStringLiteral("Unreachable");
            else if (body.value(QStringLiteral("ok")).toBool())
                status = QString::fromUtf8("\xE2\x9C\x93 Ready");
            else
                status = QStringLiteral("Not ready: ") +
                         body.value(QStringLiteral("reasons")).toVariant()
                             .toStringList().join(QStringLiteral(", "));
            for (int row = 0; row < m_fleetNodesTable->rowCount(); ++row) {
                const QTableWidgetItem *item = m_fleetNodesTable->item(row, 0);
                if (item && item->text() == name)
                    m_fleetNodesTable->setItem(row, 4, new QTableWidgetItem(status));
            }
        });
    }
}

void MainWindow::updateHostsFleetSummary()
{
    const auto applyFleetSummarySheet = [this](const QString &sheet) {
        if (m_hostsFleetSummary->styleSheet() != sheet)
            m_hostsFleetSummary->setStyleSheet(Theme::remapSheet(sheet));
    };
    if (!m_hostsFleetSummary)
        return;
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, nullptr);
    int online = 0;
    int checking = 0;
    int attention = 0;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        const QString key = forkmesh::control::savedHostCredentialKey(
            host.value(QStringLiteral("name")).toString(),
            host.value(QStringLiteral("ip")).toString(),
            host.value(QStringLiteral("user")).toString());
        const QString status = m_hostReachability.value(key);
        if (status.isEmpty() || status.contains(QStringLiteral("Checking")))
            ++checking;
        else if (status.contains(QStringLiteral("Online")) &&
                 !status.contains(QStringLiteral("missing"),
                                  Qt::CaseInsensitive))
            ++online;
        else
            ++attention;
    }
    if (hosts.isEmpty()) {
        m_hostsFleetSummary->setText(QStringLiteral("No saved mirror servers"));
        applyFleetSummarySheet(
            QStringLiteral("color:#8b949e; font-size:11px;"));
        return;
    }
    if (online == hosts.size()) {
        m_hostsFleetSummary->setText(
            QString::fromUtf8("\xE2\x9C\x93 All %1 mirror servers online")
                .arg(online));
        applyFleetSummarySheet(
            QStringLiteral("color:#2da44e; font-size:11px; font-weight:600;"));
        return;
    }
    m_hostsFleetSummary->setText(
        QStringLiteral("%1 saved · %2 online · %3 checking · %4 need attention")
            .arg(hosts.size())
            .arg(online)
            .arg(checking)
            .arg(attention));
    applyFleetSummarySheet(
        QStringLiteral("color:%1; font-size:11px;")
            .arg(attention > 0 ? QStringLiteral("#d29922")
                               : QStringLiteral("#8b949e")));
}

forkmesh::control::AgentCliCredentials MainWindow::localAgentCliCredentials()
{
    forkmesh::control::AgentCliCredentials credentials;
    const auto readFile = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    credentials.claudeCredentials = readFile(
        agentAccountCredentialPath(
            QStringLiteral("claude-code"),
            activeAgentAccount(QStringLiteral("claude-code")).configDir));
    credentials.codexAuth =
        readFile(agentAccountCredentialPath(
            QStringLiteral("codex"),
            activeAgentAccount(QStringLiteral("codex")).configDir));
    QSettings settings;
    if (credentials.claudeCredentials.trimmed().isEmpty()) {
        const QString key =
            settings.value(kClaudeApiKeySetting).toString().trimmed();
        if (!key.isEmpty())
            credentials.env.insert(QStringLiteral("ANTHROPIC_API_KEY"), key);
    }
    if (credentials.codexAuth.trimmed().isEmpty()) {
        const QString key =
            settings.value(kCodexApiKeySetting).toString().trimmed();
        if (!key.isEmpty())
            credentials.env.insert(QStringLiteral("OPENAI_API_KEY"), key);
    }
    return credentials;
}

void MainWindow::probeSavedHosts()
{
    if (!m_hostsTable)
        return;
    m_hostProbeLastMs = QDateTime::currentMSecsSinceEpoch();
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        probeSavedHost(
            host.value(QStringLiteral("name")).toString().trimmed(),
            host.value(QStringLiteral("ip")).toString().trimmed(),
            host.value(QStringLiteral("user")).toString().trimmed(),
            host.value(QStringLiteral("status")).toString().trimmed());
    }
    updateHostsFleetSummary();
}

void MainWindow::probeSavedHost(const QString &name, const QString &ip,
                                const QString &user,
                                const QString &savedStatus)
{
    if (!m_hostsTable || name.isEmpty() || ip.isEmpty() || user.isEmpty())
        return;
    const QString key =
        forkmesh::control::savedHostCredentialKey(name, ip, user);
    if (m_hostProbesInFlight.contains(key))
        return;
    m_hostProbesInFlight.insert(key);

    const auto rowForKey = [this](const QString &candidate) -> int {
        if (!m_hostsTable)
            return -1;
        for (int row = 0; row < m_hostsTable->rowCount(); ++row) {
            const QTableWidgetItem *item = m_hostsTable->item(row, 0);
            if (item && item->data(Qt::UserRole).toString() == candidate)
                return row;
        }
        return -1;
    };
    const auto render = [this, rowForKey, key](
                            const QString &status,
                            const QString &claude,
                            const QString &codex,
                            const QColor &color) {
        m_hostReachability.insert(key, status);
        m_hostClaudeAvailability.insert(key, claude);
        m_hostCodexAvailability.insert(key, codex);
        updateHostsFleetSummary();
        const int row = rowForKey(key);
        if (row < 0 || !m_hostsTable)
            return;
        for (const auto &entry : {
                 qMakePair(3, status),
                 qMakePair(4, claude),
                 qMakePair(5, codex),
             }) {
            if (QTableWidgetItem *item =
                    m_hostsTable->item(row, entry.first)) {
                item->setText(entry.second);
                item->setForeground(color);
            }
        }
    };
    render(QString::fromUtf8("\xE2\x97\x8C Checking\xE2\x80\xA6"),
           QString::fromUtf8("Checking\xE2\x80\xA6"),
           QString::fromUtf8("Checking\xE2\x80\xA6"),
           QColor(QStringLiteral("#8b949e")));

    QString password;
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("name")).toString().trimmed() == name &&
            host.value(QStringLiteral("ip")).toString().trimmed() == ip &&
            host.value(QStringLiteral("user")).toString().trimmed() == user) {
            password = m_hostSessionPasswords.value(key);
            break;
        }
    }
    const QString identityFile = savedHostIdentityFile(name, ip, user);
    // A managed mirror never gets the desktop/CLI `forkmesh` binary: the Vultr
    // deploy installs the Go mirror node at /usr/local/bin/forkmesh-mirror-node
    // and supervises it with forkmesh-mirror-node.service. Probe for that too,
    // otherwise every healthy mirror in the fleet reports "ForkMesh missing".
    const QString remoteCommand = QStringLiteral(
        "sh -lc 'probe_home=\"$HOME\"; "
        "if test \"$(id -u)\" = 0 && id forkmesh-node >/dev/null 2>&1; then "
        "probe_home=$(getent passwd forkmesh-node | cut -d: -f6); fi; "
        "export PATH=\"$probe_home/.local/bin:$probe_home/.claude/bin:$PATH\"; "
        "mirror=0; mirror_service=0; "
        "if command -v forkmesh-mirror-node >/dev/null 2>&1 || "
        "test -x /usr/local/bin/forkmesh-mirror-node; then mirror=1; fi; "
        "if systemctl is-active --quiet forkmesh-mirror-node.service "
        ">/dev/null 2>&1; then mirror_service=1; fi; "
        "printf \"FORKMESH=%s MIRROR=%s MIRRORSVC=%s CLAUDE=%s CODEX=%s\\\\n\" "
        "\"$(command -v forkmesh >/dev/null 2>&1 && echo 1 || echo 0)\" "
        "\"$mirror\" \"$mirror_service\" "
        "\"$(command -v claude >/dev/null 2>&1 && echo 1 || echo 0)\" "
        "\"$(command -v codex >/dev/null 2>&1 && echo 1 || echo 0)\"'");
    QString error;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            ip, user, password, remoteCommand, &error, identityFile);
    if (ssh.program.isEmpty()) {
        m_hostProbesInFlight.remove(key);
        render(
            QString::fromUtf8("\xE2\x97\x8F Attention"),
            QString::fromUtf8("\xE2\x80\x94"),
            QString::fromUtf8("\xE2\x80\x94"),
            QColor(QStringLiteral("#d29922")));
        const int row = rowForKey(key);
        if (row >= 0) {
            if (QTableWidgetItem *item = m_hostsTable->item(row, 3))
                item->setToolTip(error);
        }
        return;
    }

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    auto *watcher = new QFutureWatcher<WorkerProcessResult>(this);
    // Destroying the page/window cancels and reaps the SSH child on its worker;
    // MainWindow teardown never waits on this periodic health probe.
    connect(watcher, &QObject::destroyed, [cancelled](QObject *) {
        cancelled->store(true, std::memory_order_relaxed);
    });
    connect(watcher, &QFutureWatcher<WorkerProcessResult>::finished, this,
            [this, watcher, key, savedStatus, render, rowForKey] {
        const WorkerProcessResult result = watcher->result();
        watcher->deleteLater();
        if (!result.started) {
            m_hostProbesInFlight.remove(key);
            render(
                QString::fromUtf8(
                    "\xE2\x97\x8F Attention \xC2\xB7 SSH unavailable"),
                QString::fromUtf8("\xE2\x80\x94"),
                QString::fromUtf8("\xE2\x80\x94"),
                QColor(QStringLiteral("#cf222e")));
            return;
        }
        const QString output = QString::fromUtf8(result.output);
        const bool online =
            result.ok &&
            output.contains(QStringLiteral("FORKMESH="));
        if (online) {
            const bool forkmesh =
                output.contains(QStringLiteral("FORKMESH=1"));
            // Mirror hosts run the supervised Go mirror node instead, so
            // "installed" means the mirror binary is staged and its unit is up.
            const bool mirrorNode =
                output.contains(QStringLiteral("MIRROR=1"));
            const bool mirrorRunning =
                output.contains(QStringLiteral("MIRRORSVC=1"));
            const bool claude =
                output.contains(QStringLiteral("CLAUDE=1"));
            const bool codex =
                output.contains(QStringLiteral("CODEX=1"));
            const bool healthy = forkmesh || (mirrorNode && mirrorRunning);
            const QString status =
                forkmesh
                    ? QString::fromUtf8("\xE2\x97\x8F Online")
                    : mirrorNode
                        ? (mirrorRunning
                               ? QString::fromUtf8(
                                     "\xE2\x97\x8F Online \xC2\xB7 Mirror node")
                               : QString::fromUtf8(
                                     "\xE2\x97\x8F Attention \xC2\xB7 Mirror "
                                     "node stopped"))
                        : QString::fromUtf8(
                              "\xE2\x97\x8F Online \xC2\xB7 ForkMesh missing");
            render(
                status,
                claude
                    ? QString::fromUtf8("\xE2\x9C\x93 Installed")
                    : QStringLiteral("Not installed"),
                codex
                    ? QString::fromUtf8("\xE2\x9C\x93 Installed")
                    : QStringLiteral("Not installed"),
                healthy
                    ? QColor(QStringLiteral("#2da44e"))
                    : QColor(QStringLiteral("#d29922")));
            const int row = rowForKey(key);
            if (row >= 0 && m_hostsTable) {
                if (QTableWidgetItem *item = m_hostsTable->item(row, 4)) {
                    item->setForeground(QColor(
                        claude ? QStringLiteral("#2da44e")
                               : QStringLiteral("#8b949e")));
                }
                if (QTableWidgetItem *item = m_hostsTable->item(row, 5)) {
                    item->setForeground(QColor(
                        codex ? QStringLiteral("#2da44e")
                              : QStringLiteral("#8b949e")));
                }
            }
        } else {
            const bool rejected =
                output.contains(QStringLiteral("Permission denied"),
                                Qt::CaseInsensitive);
            const bool timedOut =
                result.timedOut ||
                output.contains(QStringLiteral("Connection timed out"),
                                Qt::CaseInsensitive);
            const bool provisioning =
                savedStatus.contains(QStringLiteral("install"),
                                     Qt::CaseInsensitive) ||
                savedStatus.contains(QStringLiteral("vultr"),
                                     Qt::CaseInsensitive) ||
                savedStatus == QStringLiteral("added");
            const QString status =
                rejected
                    ? QString::fromUtf8("\xE2\x97\x8F Attention \xC2\xB7 SSH key rejected")
                    : timedOut && provisioning
                        ? QString::fromUtf8("\xE2\x97\x8C Provisioning \xC2\xB7 waiting for SSH")
                        : QString::fromUtf8("\xE2\x97\x8F Offline \xC2\xB7 unreachable");
            render(
                status,
                QString::fromUtf8("\xE2\x80\x94"),
                QString::fromUtf8("\xE2\x80\x94"),
                rejected
                    ? QColor(QStringLiteral("#cf222e"))
                    : QColor(QStringLiteral("#8b949e")));
            const int row = rowForKey(key);
            if (row >= 0) {
                if (QTableWidgetItem *item = m_hostsTable->item(row, 3))
                    item->setToolTip(Tooltips::preview(output));
            }
        }
        m_hostProbesInFlight.remove(key);
    });

    WorkerProcessRequest processRequest;
    processRequest.program = ssh.program;
    processRequest.arguments = ssh.arguments;
    processRequest.environment = ssh.environment;
    processRequest.channelMode = QProcess::MergedChannels;
    processRequest.timeoutMs = 35000;
    processRequest.maxOutputBytes = 4096;
    watcher->setFuture(QtConcurrent::run(
        [processRequest = std::move(processRequest), cancelled] {
            return runWorkerProcess(processRequest, cancelled);
        }));
}

void MainWindow::installAgentClisForHost(int row)
{
    if (m_hostAgentInstallProcess &&
        m_hostAgentInstallProcess->state() != QProcess::NotRunning) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("A mirror agent-CLI install is already running."));
        return;
    }
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;
    const auto cellText = [this, row](int column) {
        const QTableWidgetItem *item = m_hostsTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };
    const QString node = cellText(0);
    const QString ip = cellText(1);
    const QString user = cellText(2);
    if (node.isEmpty() || ip.isEmpty() || user.isEmpty())
        return;
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QString pass;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("name")).toString() == node &&
            host.value(QStringLiteral("ip")).toString() == ip &&
            host.value(QStringLiteral("user")).toString() == user) {
            pass = m_hostSessionPasswords.value(
                forkmesh::control::savedHostCredentialKey(node, ip, user));
            break;
        }
    }
    const QString identityFile = savedHostIdentityFile(node, ip, user);
    // Every mirror authorizes the shared ForkMesh key, so that key is what the
    // installer authenticates with — the same transport every later
    // headless-agent connection uses. A session password is still passed when
    // one was entered, and ssh only reaches it if the shared key is refused.
    const QString sshPassword = pass;
    if (m_hostInstallLog)
        m_hostInstallLog->clear();
    if (m_hostInstallStatus)
        m_hostInstallStatus->setText(
            QStringLiteral("Installing agent CLIs on %1...").arg(node));
    runAgentCliInstall(node, ip, user, sshPassword, identityFile,
                       /*copyCredentials=*/false,
                       [this](bool, QString message) {
                           if (m_hostInstallStatus)
                               m_hostInstallStatus->setText(message);
                       });
}

void MainWindow::runAgentCliInstall(
    const QString &node, const QString &ip, const QString &user,
    const QString &sshPassword, const QString &identityFile,
    bool copyCredentials, std::function<void(bool, QString)> onFinished)
{
    const auto report = [onFinished](bool ok, const QString &message) {
        if (onFinished)
            onFinished(ok, message);
    };
    if (m_hostAgentInstallProcess &&
        m_hostAgentInstallProcess->state() != QProcess::NotRunning) {
        report(false,
               QStringLiteral("A mirror agent-CLI install is already running."));
        return;
    }
    // Secrets are collected here and live only in the payload byte array and
    // the child's stdin pipe; the remote command below is fixed and secret-free.
    QByteArray payload;
    QString credentialSummary;
    if (copyCredentials) {
        const forkmesh::control::AgentCliCredentials credentials =
            localAgentCliCredentials();
        if (forkmesh::control::agentCliCredentialsAreEmpty(credentials)) {
            report(false, QStringLiteral(
                "This device has no Claude Code or Codex login to copy to %1. "
                "Sign in here first, then use Install Claude + Codex on that "
                "host.").arg(node));
            return;
        }
        QString payloadError;
        payload = forkmesh::control::buildAgentCliBootstrapPayload(
            credentials, &payloadError);
        if (payload.isEmpty()) {
            report(false, payloadError);
            return;
        }
        credentialSummary =
            forkmesh::control::describeAgentCliCredentials(credentials);
    }
    const QString remoteCmd =
        forkmesh::control::agentCliBootstrapRemoteCommand(copyCredentials);
    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            ip, user, sshPassword, remoteCmd, &sshError, identityFile);
    if (ssh.program.isEmpty()) {
        payload.fill('\0');
        report(false, sshError);
        return;
    }
    appendHostInstallLog(
        QStringLiteral("Installing Claude Code and Codex on %1 (%2@%3)...\n")
            .arg(node, user, ip));
    appendHostInstallLog(
        identityFile.isEmpty()
            ? QStringLiteral(
                  "The shared ForkMesh SSH key has not been generated on this "
                  "device; using the current session credential or the system "
                  "SSH agent.\n")
            : QStringLiteral(
                  "Using the shared ForkMesh SSH identity (%1).\n")
                  .arg(identityFile));
    if (copyCredentials)
        appendHostInstallLog(
            QStringLiteral("Copying this device's agent access (%1) over the "
                           "SSH session's stdin.\n")
                .arg(credentialSummary));
    auto *proc = new QProcess(this);
    m_hostAgentInstallProcess = proc;
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProcessEnvironment(ssh.environment);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        const QByteArray chunk = proc->readAllStandardOutput();
        QByteArray transcript =
            proc->property("forkmeshAgentInstallOutput").toByteArray();
        transcript += chunk;
        if (transcript.size() > 8192)
            transcript = transcript.right(8192);
        proc->setProperty("forkmeshAgentInstallOutput", transcript);
        appendHostInstallLog(QString::fromUtf8(chunk));
    });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, report](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        if (m_hostAgentInstallProcess == proc)
            m_hostAgentInstallProcess = nullptr;
        report(false,
               QStringLiteral("Could not start the pinned SSH installer."));
    });
    connect(proc, &QProcess::finished, this,
            [this, proc, node, ip, user, identityFile, copyCredentials,
             report](
                int code, QProcess::ExitStatus status) {
        if (m_hostAgentInstallProcess == proc)
            m_hostAgentInstallProcess = nullptr;
        const bool ok =
            code == 0 && status == QProcess::NormalExit;
        appendHostInstallLog(
            ok ? QStringLiteral("\nAgent CLI installation finished.\n")
               : QStringLiteral("\nAgent CLI installation failed (exit %1).\n")
                     .arg(code));
        const QString output = QString::fromUtf8(
            proc->property("forkmeshAgentInstallOutput").toByteArray());
        const bool timedOut = output.contains(
            QStringLiteral("Connection timed out"),
            Qt::CaseInsensitive);
        const bool keyRejected = output.contains(
            QStringLiteral("Permission denied"),
            Qt::CaseInsensitive);
        report(ok,
            ok
                ? (copyCredentials
                       ? QStringLiteral(
                             "Claude Code and Codex are installed on %1 and "
                             "signed in with this device's access.")
                             .arg(node)
                       : QStringLiteral(
                             "Claude Code and Codex are installed on %1. "
                             "Finish the provider sign-ins in the terminal "
                             "that just opened.")
                             .arg(node))
                : timedOut
                    ? QStringLiteral(
                          "SSH could not reach %1 on port 22. The saved "
                          "key was not reached; use the host's reachable "
                          "public/stable address or connect this device to "
                          "the private network, then retry.")
                          .arg(ip)
                    : keyRejected && !identityFile.isEmpty()
                        ? QStringLiteral(
                              "The mirror rejected the shared ForkMesh SSH "
                              "key. Add that key to %1's authorized_keys, "
                              "then retry.")
                              .arg(node)
                : QStringLiteral(
                      "Agent CLI installation failed on %1; see Live output.")
                      .arg(node));
        QTimer::singleShot(0, this, &MainWindow::probeSavedHosts);
        proc->deleteLater();
        // A run without copied credentials leaves the mirror with the binaries
        // and no provider session. Hand the operator the shell to enter them in
        // as soon as the binaries exist, instead of leaving "login is still
        // required" as a dead end. A copyCredentials run is already signed in,
        // so it gets no window.
        if (ok && !copyCredentials)
            openHostAgentLoginTerminal(node, ip, user);
    });
    proc->start(ssh.program, ssh.arguments);
    // The credentials leave this process only here, on the child's stdin, and
    // the buffer is wiped as soon as it is handed over.
    if (!payload.isEmpty()) {
        proc->write(payload);
        payload.fill('\0');
    }
    proc->closeWriteChannel();
}

void MainWindow::openHostAgentLoginTerminalForSelection(int row)
{
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;
    const auto cellText = [this, row](int column) {
        const QTableWidgetItem *item = m_hostsTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };
    openHostAgentLoginTerminal(cellText(0), cellText(1), cellText(2));
}

void MainWindow::openAgentLoginFromSystemLink(const QString &node,
                                              const QString &provider)
{
    const QString cleanNode = node.trimmed().toLower();
    const QString cleanProvider = provider.trimmed().toLower();
    if (cleanNode.isEmpty() ||
        (cleanProvider != QLatin1String("claude-code") &&
         cleanProvider != QLatin1String("codex"))) {
        flashMessage(QStringLiteral("The agent sign-in link is invalid."), true);
        return;
    }

    // A website opened on this machine can authenticate this machine directly.
    // The provider owns the login UI; ForkMesh only launches its CLI command.
    if (cleanNode == machineNodeName().trimmed().toLower() ||
        cleanNode == accountOwner().trimmed().toLower()) {
        launchAgentSystemTerminal(cleanProvider, QStringLiteral("login"));
        return;
    }

    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("name")).toString().trimmed().toLower() !=
            cleanNode) {
            continue;
        }
        openHostAgentLoginTerminal(
            host.value(QStringLiteral("name")).toString().trimmed(),
            host.value(QStringLiteral("ip")).toString().trimmed(),
            host.value(QStringLiteral("user")).toString().trimmed(),
            cleanProvider);
        return;
    }
    flashMessage(
        QStringLiteral(
            "%1 is not this desktop or a saved host. Add it in Hosts before "
            "starting the provider login.")
            .arg(cleanNode),
        true);
}

// --- streamed provider sign-in ---------------------------------------------
//
// The website cannot open a window on this machine, and the
// `forkmesh://agent-login/...` handoff above only works where this app is
// already installed — on any other computer the browser hands that URL to the
// operating system, which answers "No Apps Available". So the owner's browser
// runs the sign-in here instead and watches it over the relay socket both ends
// already hold. Everything the frame can choose is the provider; the command
// is this app's own login command, never a shell, and the credential is still
// created on the provider's site.
void MainWindow::onNodeTerminalFrame(const QJsonObject &frame)
{
    const QString session =
        frame.value(QStringLiteral("session")).toString().trimmed();
    const QString node =
        frame.value(QStringLiteral("node")).toString().trimmed().toLower();
    const QString action =
        frame.value(QStringLiteral("action")).toString().trimmed();
    if (session.isEmpty() || node.isEmpty())
        return;
    // The relay broadcasts to every node socket on the account because it
    // cannot tell which machine holds which one. A frame naming another node
    // is not ours to answer.
    const QString self = machineNodeName().trimmed().toLower();
    const QString owner = accountOwner().trimmed().toLower();
    if (node != self && (owner.isEmpty() || node != owner))
        return;

    if (action == QLatin1String("close")) {
        if (session == m_nodeTerminalSession)
            stopNodeTerminalSession();
        return;
    }
    if (action == QLatin1String("input")) {
        if (session != m_nodeTerminalSession || !m_nodeTerminalPty)
            return;
        m_nodeTerminalPty->write(QByteArray::fromBase64(
            frame.value(QStringLiteral("data")).toString().toLatin1()));
        return;
    }
    if (action == QLatin1String("resize")) {
        if (session != m_nodeTerminalSession || !m_nodeTerminalPty)
            return;
        m_nodeTerminalPty->resize(frame.value(QStringLiteral("cols")).toInt(),
                                  frame.value(QStringLiteral("rows")).toInt());
        return;
    }
    if (action != QLatin1String("open"))
        return;

    // The same consent that lets the website queue agent work on this node.
    // Without it the website may not drive this machine at all, and saying so
    // by name is more useful than a silent refusal.
    if (!QSettings().value(kPublishAgentsToWebSetting, false).toBool()) {
        sendNodeTerminalFrame(
            session, QStringLiteral("error"),
            {{QStringLiteral("message"),
              QStringLiteral("This node does not accept agent work from the "
                             "website. Enable it in Settings, then retry.")}});
        return;
    }
    const QString provider =
        frame.value(QStringLiteral("provider")).toString().trimmed();
    const int cols = frame.value(QStringLiteral("cols")).toInt(80);
    const int rows = frame.value(QStringLiteral("rows")).toInt(24);

    // A reconnected tab re-announces the session it already had. The pty
    // outlived its socket, so repaint it instead of restarting a sign-in that
    // is halfway through.
    if (m_nodeTerminalPty && m_nodeTerminalPty->isRunning() &&
        session == m_nodeTerminalSession) {
        m_nodeTerminalPty->resize(cols, rows);
        sendNodeTerminalFrame(session, QStringLiteral("ready"));
        if (!m_nodeTerminalReplay.isEmpty()) {
            sendNodeTerminalFrame(
                session, QStringLiteral("output"),
                {{QStringLiteral("data"),
                  QString::fromLatin1(m_nodeTerminalReplay.toBase64())}});
        }
        return;
    }
    stopNodeTerminalSession();

    auto *pty = new AgentLoginPty(this);
    QString error;
    if (!pty->start(provider, cols, rows, &error)) {
        pty->deleteLater();
        sendNodeTerminalFrame(session, QStringLiteral("error"),
                              {{QStringLiteral("message"), error}});
        return;
    }
    m_nodeTerminalPty = pty;
    m_nodeTerminalSession = session;
    m_nodeTerminalReplay.clear();
    connect(pty, &AgentLoginPty::output, this,
            [this, session](const QByteArray &chunk) {
                if (session != m_nodeTerminalSession)
                    return;
                // Bounded repaint material for a reconnecting tab, held in
                // memory only: a sign-in screen is a few kilobytes, and this
                // is not a transcript of one.
                m_nodeTerminalReplay.append(chunk);
                if (m_nodeTerminalReplay.size() > kNodeTerminalReplayBytes) {
                    m_nodeTerminalReplay = m_nodeTerminalReplay.right(
                        kNodeTerminalReplayBytes);
                }
                sendNodeTerminalFrame(
                    session, QStringLiteral("output"),
                    {{QStringLiteral("data"),
                      QString::fromLatin1(chunk.toBase64())}});
            });
    connect(pty, &AgentLoginPty::finished, this,
            [this, session](int exitCode) {
                if (session != m_nodeTerminalSession)
                    return;
                sendNodeTerminalFrame(session, QStringLiteral("exit"),
                                      {{QStringLiteral("code"), exitCode}});
                stopNodeTerminalSession();
            });
    if (!m_nodeTerminalLifetime) {
        m_nodeTerminalLifetime = new QTimer(this);
        m_nodeTerminalLifetime->setSingleShot(true);
        connect(m_nodeTerminalLifetime, &QTimer::timeout, this,
                &MainWindow::stopNodeTerminalSession);
    }
    // A tab that vanished without closing must not leave a sign-in prompt
    // open on this machine indefinitely.
    m_nodeTerminalLifetime->start(kNodeTerminalMaxLifetimeMs);
    sendNodeTerminalFrame(session, QStringLiteral("ready"));
    logSystem(QStringLiteral(
                  "Started a website sign-in terminal for %1 on this node.")
                  .arg(provider));
}

void MainWindow::sendNodeTerminalFrame(const QString &session,
                                       const QString &action,
                                       const QJsonObject &fields)
{
    if (!m_nodeEventSocket || session.isEmpty())
        return;
    QJsonObject frame{
        {QStringLiteral("type"), QStringLiteral("terminal")},
        {QStringLiteral("session"), session},
        {QStringLiteral("node"), machineNodeName().trimmed().toLower()},
        {QStringLiteral("action"), action},
    };
    for (auto it = fields.begin(); it != fields.end(); ++it)
        frame.insert(it.key(), it.value());
    // No fallback: an unsent screen update is replaced by the next one, and a
    // reconnecting tab is repainted from the replay buffer.
    m_nodeEventSocket->sendTerminalFrame(frame);
}

void MainWindow::stopNodeTerminalSession()
{
    if (m_nodeTerminalLifetime)
        m_nodeTerminalLifetime->stop();
    m_nodeTerminalSession.clear();
    m_nodeTerminalReplay.clear();
    if (!m_nodeTerminalPty)
        return;
    AgentLoginPty *pty = m_nodeTerminalPty;
    m_nodeTerminalPty = nullptr;
    pty->disconnect(this);
    pty->stop();
    pty->deleteLater();
}

namespace {

struct AgentLoginDialogParts {
    QDialog *dialog = nullptr;
    QVBoxLayout *layout = nullptr;
    QLabel *autoOpenStatus = nullptr;
    TerminalWidget *terminal = nullptr;
    QHBoxLayout *buttons = nullptr;
    QPushButton *close = nullptr;
};

AgentLoginDialogParts buildAgentLoginDialog(QWidget *parent,
                                            const QString &title,
                                            const QString &noticeHtml)
{
    AgentLoginDialogParts parts;
    parts.dialog = new QDialog(parent);
    parts.dialog->setObjectName(QStringLiteral("hostAgentLoginDialog"));
    parts.dialog->setAttribute(Qt::WA_DeleteOnClose);
    parts.dialog->setWindowTitle(title);
    parts.dialog->resize(900, 560);
    parts.layout = new QVBoxLayout(parts.dialog);
    auto *notice = new QLabel(noticeHtml, parts.dialog);
    notice->setWordWrap(true);
    parts.layout->addWidget(notice);
    parts.autoOpenStatus = new QLabel(parts.dialog);
    parts.autoOpenStatus->setObjectName(
        QStringLiteral("hostAgentLoginAutoOpenStatus"));
    parts.autoOpenStatus->setWordWrap(true);
    parts.autoOpenStatus->hide();
    parts.layout->addWidget(parts.autoOpenStatus);
    parts.terminal = new TerminalWidget(parts.dialog);
    parts.terminal->setObjectName(QStringLiteral("hostAgentLoginTerminal"));
    parts.layout->addWidget(parts.terminal, 1);
    QLabel *autoOpenStatus = parts.autoOpenStatus;
    QObject::connect(
        parts.terminal, &TerminalWidget::signInUrlDetected, autoOpenStatus,
        [autoOpenStatus](const QUrl &url) {
            autoOpenStatus->setText(
                url.host() == QLatin1String("auth.openai.com")
                    ? QStringLiteral("Opened the Codex device sign-in page in "
                                     "your browser. Enter the one-time code "
                                     "shown below there.")
                    : QStringLiteral(
                          "Opened the Claude sign-in page in your browser."));
            autoOpenStatus->show();
        });
    parts.buttons = new QHBoxLayout;
    parts.buttons->addStretch(1);
    parts.close = new QPushButton(QStringLiteral("Close"), parts.dialog);
    parts.close->setObjectName(QStringLiteral("hostAgentLoginCloseButton"));
    QObject::connect(parts.close, &QPushButton::clicked, parts.dialog,
                     &QDialog::close);
    parts.buttons->addWidget(parts.close);
    parts.layout->addLayout(parts.buttons);
    return parts;
}

} // namespace

void MainWindow::openHostAgentLoginTerminal(const QString &node,
                                            const QString &ip,
                                            const QString &user,
                                            const QString &provider)
{
    if (node.isEmpty() || ip.isEmpty() || user.isEmpty())
        return;
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QString pass;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("name")).toString() == node &&
            host.value(QStringLiteral("ip")).toString() == ip &&
            host.value(QStringLiteral("user")).toString() == user) {
            pass = m_hostSessionPasswords.value(
                forkmesh::control::savedHostCredentialKey(node, ip, user));
            break;
        }
    }
    const QString identityFile = savedHostIdentityFile(node, ip, user);
    const QString sshPassword = pass;
    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostInteractiveSshCommand(
            ip, user, sshPassword,
            forkmesh::control::buildHostAgentLoginRemoteCommand(), &sshError,
            identityFile);
    if (ssh.program.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(sshError);
        return;
    }

    const QString cleanProvider = provider.trimmed().toLower();
    const QString loginCommand =
        cleanProvider == QLatin1String("claude-code")
            ? QStringLiteral("claude auth login")
            : cleanProvider == QLatin1String("codex")
                  ? QStringLiteral("codex login")
                  : QStringLiteral("claude (then /login) or codex login");
    const AgentLoginDialogParts parts = buildAgentLoginDialog(
        this, QString::fromUtf8("Sign in \xE2\x80\x94 %1").arg(node),
        QStringLiteral(
            "This is a live shell on <b>%1@%2</b>. Run <code>%3</code> and "
            "complete the provider-owned browser flow. ForkMesh never reads "
            "or stores what you type in this window. Drag to select text and "
            "it's copied automatically (or use Ctrl+Shift+C / right-click).")
            .arg(user.toHtmlEscaped(), ip.toHtmlEscaped(),
                 loginCommand.toHtmlEscaped()));
    QDialog *dialog = parts.dialog;
    TerminalWidget *terminal = parts.terminal;

    QStringList extraEnv;
    if (!sshPassword.isEmpty())
        extraEnv << QStringLiteral("SSHPASS=") + sshPassword;
    dialog->show();
    dialog->raise();
    terminal->runCommand(forkmesh::control::hostSshCommandLine(ssh),
                         QDir::homePath(), extraEnv);
    terminal->setFocus();
}

void MainWindow::configureHostActionsForSelection(int row)
{
    if (m_hostActionsProcess &&
        m_hostActionsProcess->state() != QProcess::NotRunning) {
        if (m_hostInstallStatus) {
            m_hostInstallStatus->setText(
                QStringLiteral("A mirror Actions configuration is already running."));
        }
        return;
    }
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;

    const auto cellText = [this, row](int column) {
        const QTableWidgetItem *item = m_hostsTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };
    const QString node = cellText(0);
    const QString host = cellText(1);
    const QString user = cellText(2);
    QString password = m_hostSessionPasswords.value(
        forkmesh::control::savedHostCredentialKey(node, host, user));

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Mirror Actions — %1").arg(node));
    dialog.setMinimumSize(640, 470);
    auto *layout = new QVBoxLayout(&dialog);
    auto *notice = new QLabel(QStringLiteral(
        "SSH access to <b>%1@%2</b> authorizes this one change. ForkMesh sends "
        "a bounded configuration document to the node helper on stdin. Variable "
        "values never enter command arguments, logs, the public mirror catalog, "
        "or this desktop's saved settings.")
                                  .arg(user.toHtmlEscaped(),
                                       host.toHtmlEscaped()));
    notice->setWordWrap(true);
    layout->addWidget(notice);

    auto *enabled =
        new QCheckBox(QStringLiteral("Enable Actions on this mirror node"));
    enabled->setObjectName(QStringLiteral("hostActionsEnabledCheck"));
    enabled->setChecked(false);
    layout->addWidget(enabled);
    auto *replace = new QCheckBox(
        QStringLiteral("Replace the node's Actions variables with this list"));
    replace->setObjectName(QStringLiteral("hostActionsReplaceVariablesCheck"));
    replace->setChecked(false);
    layout->addWidget(replace);

    auto *variables = new QTableWidget(0, 2);
    variables->setObjectName(QStringLiteral("hostActionsVariablesTable"));
    variables->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Value")});
    variables->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    variables->horizontalHeader()->setSectionResizeMode(1,
                                                         QHeaderView::Stretch);
    variables->verticalHeader()->setVisible(false);
    variables->setSelectionBehavior(QAbstractItemView::SelectRows);
    variables->setSelectionMode(QAbstractItemView::SingleSelection);
    variables->setEditTriggers(QAbstractItemView::NoEditTriggers);
    variables->setEnabled(false);
    layout->addWidget(variables, 1);

    auto *variableButtons = new QHBoxLayout;
    auto *addVariable = new QPushButton(QStringLiteral("Add variable"));
    addVariable->setObjectName(QStringLiteral("hostActionsAddVariableButton"));
    auto *removeVariable =
        new QPushButton(QStringLiteral("Remove selected"));
    removeVariable->setObjectName(
        QStringLiteral("hostActionsRemoveVariableButton"));
    addVariable->setEnabled(false);
    removeVariable->setEnabled(false);
    variableButtons->addWidget(addVariable);
    variableButtons->addWidget(removeVariable);
    variableButtons->addStretch();
    layout->addLayout(variableButtons);
    connect(replace, &QCheckBox::toggled, variables,
            &QWidget::setEnabled);
    connect(replace, &QCheckBox::toggled, addVariable,
            &QWidget::setEnabled);
    connect(replace, &QCheckBox::toggled, removeVariable,
            &QWidget::setEnabled);

    connect(addVariable, &QPushButton::clicked, &dialog,
            [this, variables] {
                bool ok = false;
                const QString name = QInputDialog::getText(
                    this, QStringLiteral("Actions variable"),
                    QStringLiteral("Variable name:"), QLineEdit::Normal,
                    QString(), &ok).trimmed();
                if (!ok || name.isEmpty())
                    return;
                const QString value = QInputDialog::getText(
                    this, QStringLiteral("Actions variable"),
                    QStringLiteral("Secret value:"), QLineEdit::Password,
                    QString(), &ok);
                if (!ok)
                    return;
                int rowForName = -1;
                for (int row = 0; row < variables->rowCount(); ++row) {
                    const QTableWidgetItem *item = variables->item(row, 0);
                    if (item && item->text() == name) {
                        rowForName = row;
                        break;
                    }
                }
                if (rowForName < 0) {
                    rowForName = variables->rowCount();
                    variables->insertRow(rowForName);
                    variables->setItem(rowForName, 0,
                                       new QTableWidgetItem(name));
                    variables->setItem(rowForName, 1,
                                       new QTableWidgetItem);
                }
                QTableWidgetItem *valueItem =
                    variables->item(rowForName, 1);
                valueItem->setText(QStringLiteral("••••••••"));
                valueItem->setData(Qt::UserRole, value);
                valueItem->setToolTip(
                    QStringLiteral("Value is hidden and will be discarded after sending."));
            });
    connect(removeVariable, &QPushButton::clicked, &dialog,
            [variables] {
                const int row = variables->currentRow();
                if (row < 0)
                    return;
                if (QTableWidgetItem *item = variables->item(row, 1)) {
                    QString secret = item->data(Qt::UserRole).toString();
                    secret.fill(QChar::Null);
                    item->setData(Qt::UserRole, QString());
                }
                variables->removeRow(row);
            });

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *apply = buttons->addButton(
        QStringLiteral("Send to mirror"), QDialogButtonBox::AcceptRole);
    apply->setObjectName(QStringLiteral("hostActionsApplyButton"));
    apply->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog,
            &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog,
            &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() != QDialog::Accepted) {
        for (int variableRow = 0; variableRow < variables->rowCount();
             ++variableRow) {
            if (QTableWidgetItem *item =
                    variables->item(variableRow, 1)) {
                QString secret = item->data(Qt::UserRole).toString();
                secret.fill(QChar::Null);
                item->setData(Qt::UserRole, QString());
            }
        }
        return;
    }

    forkmesh::control::MirrorActionsConfigurationRequest request;
    request.requestId =
        QUuid::createUuid().toString(QUuid::Id128).toLower();
    request.host = host;
    request.sshUser = user;
    request.nodeName = node;
    request.actionsEnabled = enabled->isChecked();
    request.replaceVariables = replace->isChecked();
    if (request.replaceVariables) {
        for (int variableRow = 0; variableRow < variables->rowCount();
             ++variableRow) {
            const QTableWidgetItem *nameItem =
                variables->item(variableRow, 0);
            QTableWidgetItem *valueItem =
                variables->item(variableRow, 1);
            if (!nameItem || !valueItem)
                continue;
            request.variables.insert(
                nameItem->text(),
                valueItem->data(Qt::UserRole).toString());
            QString secret = valueItem->data(Qt::UserRole).toString();
            secret.fill(QChar::Null);
            valueItem->setData(Qt::UserRole, QString());
            valueItem->setText(QStringLiteral("discarded"));
        }
    }
    runHostActionsConfiguration(std::move(request), password);
    password.fill(QChar::Null);
}

void MainWindow::runHostActionsConfiguration(
    forkmesh::control::MirrorActionsConfigurationRequest request,
    const QString &sshPassword)
{
    QString error;
    forkmesh::control::MirrorActionsSshCommand command =
        forkmesh::control::buildMirrorActionsSshCommand(
            request, sshPassword, &error,
            savedHostIdentityFile(request.nodeName, request.host,
                                  request.sshUser));
    if (command.program.isEmpty()) {
        for (QString &value : request.variables)
            value.fill(QChar::Null);
        command.standardInput.fill('\0');
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(error);
        return;
    }

    const QString requestId = request.requestId;
    const QString node = request.nodeName.trimmed();
    const QString host = request.host.trimmed();
    const bool enabled = request.actionsEnabled;
    const bool replaceVariables = request.replaceVariables;
    const int variableCount = request.variables.size();
    for (QString &value : request.variables) {
        value.fill(QChar::Null);
    }
    request.variables.clear();

    auto rawOutput = QSharedPointer<QByteArray>::create();
    auto outputOverflow = QSharedPointer<bool>::create(false);
    auto completed = QSharedPointer<bool>::create(false);
    auto *process = new QProcess(this);
    m_hostActionsProcess = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setProcessEnvironment(command.environment);
    if (m_hostInstallStatus) {
        m_hostInstallStatus->setText(
            QStringLiteral(
                "Sending Actions state to %1 over authenticated SSH…")
                .arg(node));
    }
    appendHostInstallLog(
        QStringLiteral(
            "\n[Actions] Sending %1 state and %2 variable name(s) to %3@%4; "
            "values are stdin-only and redacted.\n")
            .arg(enabled ? QStringLiteral("enabled")
                         : QStringLiteral("disabled"))
            .arg(replaceVariables ? variableCount : 0)
            .arg(request.sshUser.trimmed(), host));

    connect(process, &QProcess::readyReadStandardOutput, this,
            [process, rawOutput, outputOverflow] {
                QByteArray chunk = process->readAllStandardOutput();
                constexpr qsizetype kMaximumOutput = 64 * 1024;
                if (rawOutput->size() + chunk.size() <= kMaximumOutput) {
                    rawOutput->append(chunk);
                } else {
                    *outputOverflow = true;
                    const qsizetype remaining =
                        qMax<qsizetype>(0, kMaximumOutput - rawOutput->size());
                    rawOutput->append(chunk.first(remaining));
                }
                // Never render remote output from this secret-bearing request.
                // A hostile/broken helper could echo stdin verbatim or encode
                // it, which cannot be made safe by ordinary string redaction.
                chunk.fill('\0');
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, rawOutput, completed](
                QProcess::ProcessError processError) {
                if (processError == QProcess::FailedToStart) {
                    if (*completed)
                        return;
                    *completed = true;
                    appendHostInstallLog(
                        QStringLiteral(
                            "[Actions] Could not start SSH. Install OpenSSH"
                            " (and sshpass for password login).\n"));
                    if (m_hostInstallStatus) {
                        m_hostInstallStatus->setText(
                            QStringLiteral(
                                "Mirror Actions configuration failed: "
                                "could not start SSH."));
                    }
                    rawOutput->fill('\0');
                    rawOutput->clear();
                    QProcessEnvironment scrubbed =
                        QProcessEnvironment::systemEnvironment();
                    scrubbed.remove(QStringLiteral("SSHPASS"));
                    process->setProcessEnvironment(scrubbed);
                    if (m_hostActionsProcess == process)
                        m_hostActionsProcess = nullptr;
                    process->deleteLater();
                }
            });
    connect(process, &QProcess::finished, this,
            [this, process, rawOutput, outputOverflow, completed,
             requestId, node, enabled, replaceVariables, variableCount](
                int exitCode, QProcess::ExitStatus exitStatus) {
                if (*completed)
                    return;
                *completed = true;
                QByteArray finalChunk =
                    process->readAllStandardOutput();
                constexpr qsizetype kMaximumOutput = 64 * 1024;
                if (rawOutput->size() + finalChunk.size() <=
                    kMaximumOutput) {
                    rawOutput->append(finalChunk);
                } else {
                    *outputOverflow = true;
                    const qsizetype remaining =
                        qMax<qsizetype>(
                            0, kMaximumOutput - rawOutput->size());
                    rawOutput->append(finalChunk.first(remaining));
                }
                finalChunk.fill('\0');
                QString resultError;
                const QJsonObject result =
                    *outputOverflow
                        ? QJsonObject()
                        : forkmesh::control::
                              parseMirrorActionsConfigurationResult(
                                  *rawOutput, requestId, node, &resultError);
                const bool confirmed =
                    exitStatus == QProcess::NormalExit && exitCode == 0 &&
                    !result.isEmpty() &&
                    result.value(QStringLiteral("ok")).toBool() &&
                    result.value(QStringLiteral("actionsEnabled")).toBool() ==
                        enabled &&
                    result.value(QStringLiteral("variablesReplaced")).toBool() ==
                        replaceVariables &&
                    result.value(QStringLiteral("variableCount")).toInt() ==
                        (replaceVariables ? variableCount : 0);
                if (confirmed) {
                    const QString summary =
                        QStringLiteral(
                            "Mirror %1 confirmed Actions %2%3.")
                            .arg(
                                node,
                                enabled ? QStringLiteral("enabled")
                                        : QStringLiteral("disabled"),
                                replaceVariables
                                    ? QStringLiteral(
                                          " and replaced %1 variable(s)")
                                          .arg(variableCount)
                                    : QString());
                    appendHostInstallLog(
                        QStringLiteral("[Actions] %1\n").arg(summary));
                    if (m_hostInstallStatus)
                        m_hostInstallStatus->setText(summary);
                } else {
                    if (*outputOverflow) {
                        resultError = QStringLiteral(
                            "The remote helper returned more than 64 KiB.");
                    } else if (resultError.isEmpty()) {
                        resultError = QStringLiteral(
                            "The host did not confirm the requested state.");
                    }
                    appendHostInstallLog(
                        QStringLiteral(
                            "[Actions] Configuration was not confirmed "
                            "(exit %1): %2\n")
                            .arg(exitCode)
                            .arg(resultError));
                    if (m_hostInstallStatus) {
                        m_hostInstallStatus->setText(
                            QStringLiteral(
                                "Mirror Actions configuration failed: %1")
                                .arg(resultError));
                    }
                }
                rawOutput->fill('\0');
                rawOutput->clear();
                QProcessEnvironment scrubbed =
                    QProcessEnvironment::systemEnvironment();
                scrubbed.remove(QStringLiteral("SSHPASS"));
                process->setProcessEnvironment(scrubbed);
                if (m_hostActionsProcess == process)
                    m_hostActionsProcess = nullptr;
                process->deleteLater();
            });

    process->start(command.program, command.arguments);
    process->write(command.standardInput);
    process->closeWriteChannel();
    const QPointer<QProcess> guardedProcess(process);
    QTimer::singleShot(45 * 1000, this, [this, guardedProcess, completed] {
        if (!guardedProcess || *completed ||
            guardedProcess->state() == QProcess::NotRunning) {
            return;
        }
        appendHostInstallLog(
            QStringLiteral(
                "[Actions] SSH configuration timed out after 45 seconds.\n"));
        guardedProcess->kill();
    });
    command.standardInput.fill('\0');
    command.standardInput.clear();
    command.environment = QProcessEnvironment();
}

void MainWindow::loadHostIntoForm(int row, int /*column*/)
{
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;
    auto cellText = [this, row](int col) -> QString {
        QTableWidgetItem *item = m_hostsTable->item(row, col);
        return item ? item->text() : QString();
    };
    const QString name = cellText(0);
    if (m_hostNameEdit)
        m_hostNameEdit->setText(name);
    if (m_hostIpEdit)
        m_hostIpEdit->setText(cellText(1));
    if (m_hostUserEdit)
        m_hostUserEdit->setText(cellText(2));
    // Restore only a password entered or migrated during this process. Host
    // credentials are deliberately never reloaded from persistent settings.
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    if (m_hostPassEdit)
        m_hostPassEdit->clear();
    for (int i = 0; i < hosts.size(); ++i) {
        const QJsonObject h = hosts.at(i).toObject();
        if (h.value("name").toString() == name) {
            if (m_hostPassEdit) {
                m_hostPassEdit->setText(m_hostSessionPasswords.value(
                    forkmesh::control::savedHostCredentialKey(
                        h.value(QStringLiteral("name")).toString(),
                        h.value(QStringLiteral("ip")).toString(),
                        h.value(QStringLiteral("user")).toString())));
            }
            break;
        }
    }
    if (m_hostInstallStatus)
        m_hostInstallStatus->setText(QString::fromUtf8(
            "Loaded \"%1\". Click Install ForkMesh to run the installer.").arg(name));
}

void MainWindow::viewHostLogsForSelection(int row)
{
    if (m_hostLogProcess &&
        m_hostLogProcess->state() != QProcess::NotRunning) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("A host log stream is already running."));
        return;
    }
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;
    loadHostIntoForm(row, 0);
    const QString ip = m_hostIpEdit ? m_hostIpEdit->text().trimmed() : QString();
    const QString user = m_hostUserEdit ? m_hostUserEdit->text().trimmed() : QString();
    const QString node = m_hostNameEdit ? m_hostNameEdit->text().trimmed() : QString();
    if (ip.isEmpty() || user.isEmpty() || node.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("Load a host row first, then click Logs."));
        return;
    }
    const QString pass =
        m_hostPassEdit ? m_hostPassEdit->text() : QString();
    runHostLogSession(ip, user, pass, node);
}

void MainWindow::runHostLogSession(const QString &ip, const QString &user,
                                  const QString &pass, const QString &node)
{
    if (m_hostLogProcess &&
        m_hostLogProcess->state() != QProcess::NotRunning) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("A host log stream is already running."));
        return;
    }
    if (ip.isEmpty() || user.isEmpty() || node.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("Enter host IP, SSH username and node name."));
        return;
    }

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Host logs: %1").arg(node));
    dialog->setMinimumSize(860, 520);
    auto *layout = new QVBoxLayout(dialog);
    auto *status = new QLabel(
        QString::fromUtf8("Connecting to %1 as %2 \xE2\x80\xA6").arg(ip, user));
    status->setObjectName("mutedLabel");
    status->setWordWrap(true);
    layout->addWidget(status);

    auto *logView = new QPlainTextEdit;
    logView->setObjectName("actionLog");
    logView->setReadOnly(true);
    logView->setLineWrapMode(QPlainTextEdit::NoWrap);
    logView->setMinimumHeight(420);
    applyLogFont(logView);
    logView->setPlaceholderText(
        QString::fromUtf8("Remote node output starts streaming here\xE2\x80\xA6"));
    layout->addWidget(logView, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    if (auto *closeBtn = buttons->button(QDialogButtonBox::Close))
        closeBtn->setDefault(true);
    layout->addWidget(buttons);

    auto appendLog = [logView](const QString &text) {
        if (!logView || text.isEmpty())
            return;
        QTextCursor cursor(logView->document());
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);
        logView->moveCursor(QTextCursor::End);
        logView->ensureCursorVisible();
    };

    auto *proc = new QProcess(this);
    m_hostLogProcess = proc;
    auto stopLogStream = [this, node, ip, user, proc]() {
        if (!proc || proc->state() == QProcess::NotRunning) {
            if (m_hostLogProcess == proc)
                m_hostLogProcess = nullptr;
            return;
        }
        // code-health: allow(blocking-wait) dialog-close stop path; waits at most 250 ms for SIGTERM, then kills
        proc->terminate();
        if (!proc->waitForFinished(250))
            proc->kill();
        proc->deleteLater();
        if (m_hostLogProcess == proc)
            m_hostLogProcess = nullptr;
        if (m_hostInstallStatus) {
            m_hostInstallStatus->setText(
                QString::fromUtf8("Stopped log stream for %1@%2 (%3).")
                    .arg(user, ip, node));
        }
    };

    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::finished, this, [this, stopLogStream](int) {
        stopLogStream();
    });

    const auto shq = [](const QString &s) {
        QString out = s;
        out.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QStringLiteral("'") + out + QStringLiteral("'");
    };
    // A host that runs ForkMesh as a systemd service has no node.log at all,
    // so the journal is followed alongside the file rather than the stream
    // giving up when the file is missing.
    const QString remoteCmd =
        QStringLiteral("sh -lc ") +
        shq(QString::fromUtf8(forkmesh::hosts::kSingleHostLogScript));
    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(
            ip, user, pass, remoteCmd, &sshError,
            savedHostIdentityFile(node, ip, user));
    if (ssh.program.isEmpty()) {
        appendLog(QStringLiteral("[error] %1\n").arg(sshError));
        status->setText(sshError);
        proc->deleteLater();
        m_hostLogProcess = nullptr;
        dialog->deleteLater();
        return;
    }
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setProcessEnvironment(ssh.environment);

    appendLog(QStringLiteral("Attempting SSH log stream to %1@%2\n")
                  .arg(user, ip));
    status->setText(QString::fromUtf8("Connected — streaming logs from host..."));
    if (m_hostInstallStatus)
        m_hostInstallStatus->setText(QStringLiteral("Opening host logs..."));

    connect(proc, &QProcess::readyReadStandardOutput, this,
            [this, proc, appendLog] {
                appendLog(QString::fromUtf8(proc->readAllStandardOutput()));
            });
    connect(proc, &QProcess::errorOccurred, this,
            [this, proc, appendLog, status](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            appendLog(QString::fromUtf8(
                "\n[error] Could not start SSH. Install OpenSSH (and sshpass "
                "only when using password login) and try again.\n"));
            status->setText(
                QString::fromUtf8("Could not start SSH session to host."));
        }
    });
    connect(proc, &QProcess::finished, this,
            [this, ip, user, proc, status, node, appendLog](
                int code, QProcess::ExitStatus exitStatus) {
                if (m_hostLogProcess == proc)
                    m_hostLogProcess = nullptr;
                if (status)
                    status->setText(
                        QString::fromUtf8("Log stream ended for %1@%2.")
                            .arg(user, ip));
                if (code != 0 || exitStatus != QProcess::NormalExit) {
                    appendLog(QStringLiteral(
                        "\n[error] Stream ended with exit %1.\n").arg(code));
                } else {
                    appendLog(QString::fromUtf8("\n[info] Stream ended.\n"));
                }
                proc->deleteLater();
            });

    proc->start(ssh.program, ssh.arguments);
    dialog->exec();
}

namespace {

// One pane of the Hosts live view. The widgets are guarded because the SSH
// process feeding the pane is parented to the window and can still deliver
// output after a reconnect has replaced the pane.
struct HostLivePane {
    // One line of Output, kept so a chip can hide or restore its source
    // without the line having to arrive again.
    struct Entry {
        int source;
        QString text;
    };

    QString node;
    QPointer<QLabel> state;
    QPointer<QLabel> traffic;
    QPointer<QPlainTextEdit> output;
    QPointer<QPlainTextEdit> activity;
    QPointer<QProcess> process;
    QByteArray stdoutTail; // bytes after the last newline, per channel
    QByteArray stderrTail;
    QList<Entry> entries;   // every Output line, whatever is on screen
    QVector<qint64> counts = // lines seen per source, for the chip badges
        QVector<qint64>(forkmesh::hosts::kLogSourceCount, 0);
    QVector<bool> shown =
        QVector<bool>(forkmesh::hosts::kLogSourceCount, true);
    QVector<QPointer<forkmesh::ui::VerticalIconButton>> chips =
        QVector<QPointer<forkmesh::ui::VerticalIconButton>>(
            forkmesh::hosts::kLogSourceCount);
    QHash<QString, QString> connections; // "local peer" -> display text
    QHash<QString, QString> snapshot;    // collected between S and E
    bool collecting = false;
    bool chipsDirty = false; // a badge is behind its tally
    bool haveConnections = false;
    qint64 rxBytes = -1;
    qint64 txBytes = -1;
    double sampledUptime = 0.0;
    QString rateText;
    // Copies the pane's lines into the main log (forwardsToMainLog).
    std::function<void(int, const QString &)> toMainLog;
};

constexpr int kHostLiveMaxLines = 2000;
constexpr int kHostLiveMaxChangesPerTick = 40;
// Trimming the backlog in batches keeps a busy journal from memmoving the
// whole list once per line.
constexpr int kHostLiveLineHeadroom = 256;

// Every source shares one view, so each line wears its own tag. The width is
// fixed and the view is monospaced, which lines the messages up into a column
// however many sources are on.
QString hostLiveEntryText(int source, const QString &text)
{
    return QStringLiteral("%1 │ %2")
        .arg(QString::fromLatin1(forkmesh::hosts::kLogSources[source].id), -6)
        .arg(text);
}

// A chip greys out until its source has said something, so an empty tally
// reads as "nothing here" rather than as a filter someone left off.
//
// Called once per read rather than once per line: a badge that changes re-lays
// out the chip row, and a chatty journal delivers hundreds of lines in one
// chunk.
void hostLiveFlushChips(HostLivePane &pane)
{
    if (!pane.chipsDirty)
        return;
    pane.chipsDirty = false;
    for (int source = 0; source < forkmesh::hosts::kLogSourceCount; ++source) {
        forkmesh::ui::VerticalIconButton *chip = pane.chips.value(source).data();
        if (!chip)
            continue;
        const qint64 count = pane.counts.at(source);
        chip->setBadgeCount(count);
        chip->setAccentColor(QColor(
            count > 0
                ? QString::fromLatin1(forkmesh::hosts::kLogSources[source].accent)
                : QStringLiteral("#6e7681")));
    }
}

void hostLiveRenderOutput(HostLivePane &pane)
{
    if (!pane.output)
        return;
    QStringList lines;
    lines.reserve(pane.entries.size());
    for (const HostLivePane::Entry &entry : std::as_const(pane.entries)) {
        if (pane.shown.at(entry.source))
            lines.append(hostLiveEntryText(entry.source, entry.text));
    }
    pane.output->setPlainText(lines.join(QLatin1Char('\n')));
    pane.output->moveCursor(QTextCursor::End);
}

void hostLiveAppend(HostLivePane &pane, const QString &sourceId,
                    const QString &text)
{
    int source = forkmesh::hosts::logSourceIndex(sourceId);
    if (source < 0)
        source = forkmesh::hosts::kFallbackLogSource;
    pane.counts[source] += 1;
    pane.entries.append({source, text});
    if (pane.entries.size() > kHostLiveMaxLines + kHostLiveLineHeadroom)
        pane.entries.remove(0, pane.entries.size() - kHostLiveMaxLines);
    if (pane.output && pane.shown.at(source))
        pane.output->appendPlainText(hostLiveEntryText(source, text));
    pane.chipsDirty = true;
    if (pane.toMainLog && forkmesh::hosts::forwardsToMainLog(source))
        pane.toMainLog(source, text);
}

// A note from the desktop's side of the stream — nothing else is arriving
// behind it, so its badge lands right away.
void hostLiveNote(HostLivePane &pane, const QString &text)
{
    hostLiveAppend(pane, QStringLiteral("host"), text);
    hostLiveFlushChips(pane);
}

// "<tag> <rest>" from the remote script, falling back to the notes source when
// a line arrives without one.
void hostLiveAppendTagged(HostLivePane &pane, const QString &tagged)
{
    const int space = tagged.indexOf(QLatin1Char(' '));
    if (space <= 0) {
        hostLiveAppend(pane, QStringLiteral("host"), tagged);
        return;
    }
    hostLiveAppend(pane, tagged.left(space), tagged.mid(space + 1));
}

QString hostLiveByteRate(double bytesPerSecond)
{
    if (bytesPerSecond < 1024.0)
        return QStringLiteral("%1 B/s").arg(qRound(bytesPerSecond));
    if (bytesPerSecond < 1024.0 * 1024.0)
        return QStringLiteral("%1 KB/s").arg(bytesPerSecond / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB/s")
        .arg(bytesPerSecond / (1024.0 * 1024.0), 0, 'f', 1);
}

int hostLivePort(const QString &endpoint)
{
    const int colon = endpoint.lastIndexOf(QLatin1Char(':'));
    return colon < 0 ? -1 : endpoint.mid(colon + 1).toInt();
}

// Parses one `ss` socket line into a stable key and a readable direction.
// `ss state established` drops the State column; an ss that kept it (or
// printed its header because -H is unsupported) is handled too.
QString hostLiveConnection(const QString &line, QString *display)
{
    const QStringList fields =
        line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (fields.size() < 4 || fields.first() == QLatin1String("Recv-Q") ||
        fields.first() == QLatin1String("State"))
        return QString();
    const int base = fields.first().at(0).isDigit() ? 2 : 3;
    if (fields.size() < base + 2)
        return QString();
    const QString local = fields.at(base);
    const QString peer = fields.at(base + 1);
    static const QRegularExpression processName(QStringLiteral("\\(\"([^\"]+)\""));
    const QRegularExpressionMatch match =
        processName.match(fields.mid(base + 2).join(QLatin1Char(' ')));
    const QString process = match.hasMatch() ? match.captured(1) : QString();
    const int localPort = hostLivePort(local);
    const int peerPort = hostLivePort(peer);
    // The lower port is almost always the service end of the socket.
    const bool inbound = localPort >= 0 && peerPort >= 0 && localPort < peerPort;
    QString text = inbound
        ? QStringLiteral("← %1 on :%2").arg(peer).arg(localPort)
        : QStringLiteral("→ %1").arg(peer);
    if (!process.isEmpty())
        text += QStringLiteral("  (%1)").arg(process);
    *display = text;
    return local + QLatin1Char(' ') + peer;
}

void hostLiveUpdateTraffic(HostLivePane &pane)
{
    if (!pane.traffic)
        return;
    QStringList parts;
    if (!pane.rateText.isEmpty())
        parts.append(pane.rateText);
    if (pane.haveConnections)
        parts.append(QStringLiteral("%1 TCP").arg(pane.connections.size()));
    pane.traffic->setText(parts.join(QStringLiteral(" · ")));
}

void hostLiveHandleLine(HostLivePane &pane, const QString &line)
{
    const QString stamp = QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
    if (line.startsWith(QLatin1String("L ")) ||
        line.startsWith(QLatin1String("M "))) {
        hostLiveAppendTagged(pane, line.mid(2));
        return;
    }
    if (line.startsWith(QLatin1String("J "))) {
        QString display;
        const QString source =
            forkmesh::hosts::journalLogSource(line.mid(2), &display);
        hostLiveAppend(pane, source, display);
        return;
    }
    if (line.startsWith(QLatin1String("N "))) {
        const QStringList totals = line.mid(2).split(QLatin1Char(' '));
        if (totals.size() != 3)
            return;
        const double uptime = totals.at(0).toDouble();
        const qint64 rx = totals.at(1).toLongLong();
        const qint64 tx = totals.at(2).toLongLong();
        if (pane.rxBytes >= 0 && uptime > pane.sampledUptime &&
            rx >= pane.rxBytes && tx >= pane.txBytes) {
            const double seconds = uptime - pane.sampledUptime;
            pane.rateText = QStringLiteral("↓ %1  ↑ %2").arg(
                hostLiveByteRate((rx - pane.rxBytes) / seconds),
                hostLiveByteRate((tx - pane.txBytes) / seconds));
        }
        pane.rxBytes = rx;
        pane.txBytes = tx;
        pane.sampledUptime = uptime;
        hostLiveUpdateTraffic(pane);
        return;
    }
    if (line == QLatin1String("S")) {
        pane.collecting = true;
        pane.snapshot.clear();
        return;
    }
    if (line.startsWith(QLatin1String("C ")) && pane.collecting) {
        QString display;
        const QString key = hostLiveConnection(line.mid(2), &display);
        if (!key.isEmpty())
            pane.snapshot.insert(key, display);
        return;
    }
    if (line == QLatin1String("E") && pane.collecting) {
        pane.collecting = false;
        QStringList changes;
        if (!pane.haveConnections) {
            changes.append(QStringLiteral("%1  %2 TCP connections open")
                               .arg(stamp).arg(pane.snapshot.size()));
            QStringList open = pane.snapshot.values();
            open.sort();
            for (const QString &display : std::as_const(open))
                changes.append(QStringLiteral("          ") + display);
        } else {
            for (auto it = pane.snapshot.cbegin(); it != pane.snapshot.cend(); ++it) {
                if (!pane.connections.contains(it.key()))
                    changes.append(QStringLiteral("%1  + %2").arg(stamp, it.value()));
            }
            for (auto it = pane.connections.cbegin(); it != pane.connections.cend(); ++it) {
                if (!pane.snapshot.contains(it.key()))
                    changes.append(QStringLiteral("%1  − %2").arg(stamp, it.value()));
            }
        }
        if (pane.activity) {
            const int shown = qMin(changes.size(), kHostLiveMaxChangesPerTick);
            for (int i = 0; i < shown; ++i)
                pane.activity->appendPlainText(changes.at(i));
            if (changes.size() > shown) {
                pane.activity->appendPlainText(
                    QStringLiteral("%1  … and %2 more changes")
                        .arg(stamp).arg(changes.size() - shown));
            }
        }
        pane.connections = pane.snapshot;
        pane.snapshot.clear();
        pane.haveConnections = true;
        hostLiveUpdateTraffic(pane);
        return;
    }
    // Untagged output is the login shell itself (a banner, a profile error).
    if (!line.trimmed().isEmpty())
        hostLiveAppend(pane, QStringLiteral("host"), line);
}

void hostLiveConsume(HostLivePane &pane, QByteArray &tail, const QByteArray &chunk,
                     bool fromStderr)
{
    tail.append(chunk);
    int start = 0;
    for (int newline = tail.indexOf('\n'); newline >= 0;
         newline = tail.indexOf('\n', start)) {
        QString line = QString::fromUtf8(tail.constData() + start, newline - start);
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        start = newline + 1;
        if (fromStderr) {
            if (!line.trimmed().isEmpty()) {
                hostLiveAppend(pane, QStringLiteral("host"),
                               QStringLiteral("[ssh] ") + line);
            }
        } else {
            hostLiveHandleLine(pane, line);
        }
    }
    tail.remove(0, start);
    // A runaway line without a newline must not grow without bound.
    if (tail.size() > 64 * 1024) {
        if (!fromStderr)
            hostLiveHandleLine(pane, QString::fromUtf8(tail));
        tail.clear();
    }
    hostLiveFlushChips(pane);
}

QPlainTextEdit *hostLiveStreamView(const QString &placeholder)
{
    auto *view = new QPlainTextEdit;
    view->setObjectName(QStringLiteral("actionLog"));
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setMaximumBlockCount(kHostLiveMaxLines);
    view->setPlaceholderText(placeholder);
    applyLogFont(view);
    return view;
}

// Streams start when the live view is put on screen and stop when it leaves
// (another tab, another page). A minimised window keeps streaming: those
// show/hide events are spontaneous.
class HostLiveVisibilityFilter final : public QObject
{
public:
    HostLiveVisibilityFilter(QObject *parent, std::function<void(bool)> onChange)
        : QObject(parent), m_onChange(std::move(onChange))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!event->spontaneous() &&
            (event->type() == QEvent::Show || event->type() == QEvent::Hide))
            m_onChange(event->type() == QEvent::Show);
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void(bool)> m_onChange;
};

} // namespace

QWidget *MainWindow::buildHostsLiveView()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("hostsLiveView"));
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(6);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(6);
    m_hostsLiveSummary = new QLabel;
    m_hostsLiveSummary->setObjectName(QStringLiteral("mutedLabel"));
    header->addWidget(m_hostsLiveSummary);
    header->addWidget(makeInlineHelpButton(
        QStringLiteral("About the live view"), QStringLiteral(
            "Every saved host gets its own SSH connection, opened in parallel. "
            "The top of each pane streams every log the host keeps — its "
            "ForkMesh node log file and its systemd journal — interleaved into "
            "one view, with a tag per line saying which. The chips under "
            "\"Output\" count what each source has sent and hide it again when "
            "one is too loud to read past. Every host's logs also reach the "
            "main log, under the MIRROR badge and named by host and source, "
            "from a background feed that runs whether or not this view is "
            "open. The "
            "bottom samples the host every two seconds: established TCP "
            "connections as they open (+) and close (−), with the owning "
            "process where the SSH user may see it, and the host's total "
            "network throughput in the pane header. Nothing is written to the "
            "host, and the connections close when you leave this view.")));
    header->addStretch(1);
    auto *reconnect = new QPushButton(QStringLiteral("Reconnect all"));
    reconnect->setObjectName(QStringLiteral("hostsLiveReconnectButton"));
    reconnect->setCursor(Qt::PointingHandCursor);
    setOcticon(reconnect, QStringLiteral("sync"), 14);
    connect(reconnect, &QPushButton::clicked, this,
            &MainWindow::startHostsLiveView);
    header->addWidget(reconnect);
    col->addLayout(header);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_hostsLivePanes = new QWidget;
    m_hostsLivePanes->setObjectName(QStringLiteral("hostsLivePanes"));
    auto *row = new QHBoxLayout(m_hostsLivePanes);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    scroll->setWidget(m_hostsLivePanes);
    col->addWidget(scroll, 1);

    page->installEventFilter(new HostLiveVisibilityFilter(page, [this](bool shown) {
        if (!shown) {
            stopHostsLiveView();
            return;
        }
        bool streaming = false;
        for (const QPointer<QProcess> &process : std::as_const(m_hostsLiveProcesses))
            streaming = streaming || process;
        if (!streaming)
            QTimer::singleShot(0, this, &MainWindow::startHostsLiveView);
    }));
    return page;
}

void MainWindow::setHostsLiveViewActive(bool active)
{
    if (!m_hostsViewStack)
        return;
    if (m_hostsLiveViewButton) {
        const QSignalBlocker block(m_hostsLiveViewButton);
        m_hostsLiveViewButton->setChecked(active);
        m_hostsLiveViewButton->setText(active ? QStringLiteral("Host list")
                                              : QStringLiteral("Live view"));
        setOcticon(m_hostsLiveViewButton,
                   active ? QStringLiteral("arrow-left")
                          : QStringLiteral("broadcast"),
                   14);
    }
    // Showing the live page starts its streams and hiding it stops them.
    m_hostsViewStack->setCurrentIndex(active ? 1 : 0);
}

void MainWindow::stopHostsLiveView()
{
    const QList<QPointer<QProcess>> processes = std::exchange(m_hostsLiveProcesses, {});
    for (const QPointer<QProcess> &process : processes) {
        if (!process || process->state() == QProcess::NotRunning)
            continue;
        // ssh closes its channel on SIGTERM; the remote loop then dies on its
        // next write. Kill it outright if it lingers.
        process->terminate();
        QTimer::singleShot(3000, process, [process] {
            if (process && process->state() != QProcess::NotRunning)
                process->kill();
        });
    }
    if (m_hostsLiveSummary && !processes.isEmpty())
        m_hostsLiveSummary->setText(QStringLiteral("Live streams stopped."));
}

void MainWindow::startHostsLiveView()
{
    if (!m_hostsLivePanes)
        return;
    stopHostsLiveView();
    auto *row = qobject_cast<QHBoxLayout *>(m_hostsLivePanes->layout());
    if (!row)
        return;
    while (QLayoutItem *item = row->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }

    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    if (hosts.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral(
            "No saved hosts yet. Add one from the host list first."));
        empty->setObjectName(QStringLiteral("mutedLabel"));
        empty->setAlignment(Qt::AlignCenter);
        row->addWidget(empty, 1);
        if (m_hostsLiveSummary)
            m_hostsLiveSummary->clear();
        return;
    }
    if (m_hostsLiveSummary) {
        m_hostsLiveSummary->setText(
            QStringLiteral("Streaming %1 host%2 over parallel SSH connections")
                .arg(hosts.size())
                .arg(hosts.size() == 1 ? QString() : QStringLiteral("s")));
    }

    const auto shq = [](const QString &s) {
        QString out = s;
        out.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        return QStringLiteral("'") + out + QStringLiteral("'");
    };
    const QString remoteCmd = QStringLiteral("sh -lc ") +
                              shq(QString::fromUtf8(
                                  forkmesh::hosts::kLiveRemoteScript));

    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        const QString node = host.value(QStringLiteral("name")).toString();
        const QString ip = host.value(QStringLiteral("ip")).toString();
        const QString user = host.value(QStringLiteral("user")).toString();

        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("leaderboardCard"));
        card->setProperty("hostLiveNode", node);
        card->setFrameShape(QFrame::StyledPanel);
        card->setMinimumWidth(380);
        auto *cardCol = new QVBoxLayout(card);
        cardCol->setContentsMargins(10, 8, 10, 8);
        cardCol->setSpacing(4);

        auto *titleRow = new QHBoxLayout;
        titleRow->setContentsMargins(0, 0, 0, 0);
        titleRow->setSpacing(6);
        auto *name = new QLabel(node.isEmpty() ? ip : node);
        QFont nameFont = name->font();
        nameFont.setBold(true);
        name->setFont(nameFont);
        titleRow->addWidget(name);
        auto *address = new QLabel(QStringLiteral("%1@%2").arg(user, ip));
        address->setObjectName(QStringLiteral("mutedLabel"));
        titleRow->addWidget(address);
        titleRow->addStretch(1);
        auto *reconnect = new QPushButton(QStringLiteral("Reconnect"));
        reconnect->setProperty("buttonSize", "sm");
        reconnect->setCursor(Qt::PointingHandCursor);
        setOcticon(reconnect, QStringLiteral("sync"), 12);
        titleRow->addWidget(reconnect);
        cardCol->addLayout(titleRow);

        auto *statusRow = new QHBoxLayout;
        statusRow->setContentsMargins(0, 0, 0, 0);
        auto *state = new QLabel;
        state->setObjectName(QStringLiteral("mutedLabel"));
        statusRow->addWidget(state, 1);
        auto *traffic = new QLabel;
        traffic->setObjectName(QStringLiteral("mutedLabel"));
        traffic->setToolTip(QStringLiteral(
            "Throughput across the host's non-loopback interfaces and its "
            "established TCP connections, sampled every two seconds."));
        statusRow->addWidget(traffic);
        cardCol->addLayout(statusRow);

        auto *split = new QSplitter(Qt::Vertical);
        split->setChildrenCollapsible(false);
        const auto captioned = [](const QString &caption, QPlainTextEdit *view,
                                  QLayout *belowCaption = nullptr) {
            auto *box = new QWidget;
            auto *boxCol = new QVBoxLayout(box);
            boxCol->setContentsMargins(0, 4, 0, 0);
            boxCol->setSpacing(2);
            auto *label = new QLabel(caption);
            QFont captionFont = label->font();
            captionFont.setBold(true);
            label->setFont(captionFont);
            boxCol->addWidget(label);
            if (belowCaption)
                boxCol->addLayout(belowCaption);
            boxCol->addWidget(view, 1);
            return box;
        };
        auto *output = hostLiveStreamView(QStringLiteral(
            "Every log the host keeps streams here — the node log and the "
            "systemd journal, one line each…"));
        auto *activity = hostLiveStreamView(
            QStringLiteral("Connections opened and closed on the host show here…"));
        auto *sourceRow = new QHBoxLayout;
        sourceRow->setContentsMargins(0, 0, 0, 0);
        sourceRow->setSpacing(2);
        split->addWidget(
            captioned(QStringLiteral("Output"), output, sourceRow));
        split->addWidget(captioned(QStringLiteral("Network activity"), activity));
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 2);
        cardCol->addWidget(split, 1);
        row->addWidget(card, 1);

        auto pane = QSharedPointer<HostLivePane>::create();
        pane->node = node;
        pane->state = state;
        pane->traffic = traffic;
        pane->output = output;
        pane->activity = activity;
        pane->toMainLog = [this, node = node.isEmpty() ? ip : node](
                              int source, const QString &text) {
            forwardHostLogLine(node, source, text);
        };

        // One chip per source: the icon says which log, the badge says how
        // much of it has arrived, and unchecking it pulls those lines back out
        // of the shared view without stopping the feed.
        for (int source = 0; source < forkmesh::hosts::kLogSourceCount; ++source) {
            const forkmesh::hosts::LogSource &spec =
                forkmesh::hosts::kLogSources[source];
            auto *chip = new forkmesh::ui::VerticalIconButton(
                QString::fromLatin1(spec.label),
                forkmesh::ui::VerticalIconButton::Tab);
            chip->setObjectName(QStringLiteral("logFilterChip"));
            chip->setCheckable(true);
            chip->setChecked(true);
            chip->setOcticonName(QString::fromLatin1(spec.icon));
            chip->setAccentColor(QColor(QStringLiteral("#6e7681")));
            chip->setBadgeAboveIcon(true);
            chip->setToolTip(
                QString::fromLatin1(spec.tip) +
                QStringLiteral("\n\nThe badge counts the lines this source has "
                               "sent. Click to hide or show them; the feed "
                               "keeps running either way."));
            sourceRow->addWidget(chip);
            pane->chips[source] = chip;
            connect(chip, &QPushButton::clicked, this,
                    [pane, source](bool checked) {
                        pane->shown[source] = checked;
                        hostLiveRenderOutput(*pane);
                    });
        }
        sourceRow->addStretch(1);

        const QString pass = m_hostSessionPasswords.value(
            forkmesh::control::savedHostCredentialKey(node, ip, user));
        const QString identity = savedHostIdentityFile(node, ip, user);
        const auto launch = [this, pane, ip, user, pass, identity, remoteCmd] {
            if (pane->process && pane->process->state() != QProcess::NotRunning)
                return;
            pane->stdoutTail.clear();
            pane->stderrTail.clear();
            pane->collecting = false;
            pane->haveConnections = false;
            pane->connections.clear();
            pane->rxBytes = -1;
            pane->rateText.clear();
            if (pane->traffic)
                pane->traffic->clear();
            if (pane->activity)
                pane->activity->clear();
            QString sshError;
            const forkmesh::control::HostSshCommand ssh =
                forkmesh::control::buildHostSshCommand(ip, user, pass, remoteCmd,
                                                       &sshError, identity);
            if (ssh.program.isEmpty()) {
                if (pane->state)
                    pane->state->setText(QStringLiteral("Cannot connect"));
                hostLiveNote(*pane, QStringLiteral("[error] ") + sshError);
                return;
            }
            auto *proc = new QProcess(this);
            pane->process = proc;
            m_hostsLiveProcesses.append(proc);
            proc->setProcessEnvironment(ssh.environment);
            if (pane->state)
                pane->state->setText(QStringLiteral("Connecting…"));
            connect(proc, &QProcess::readyReadStandardOutput, this, [pane, proc] {
                if (pane->state && pane->state->text() != QLatin1String("Streaming"))
                    pane->state->setText(QStringLiteral("Streaming"));
                hostLiveConsume(*pane, pane->stdoutTail, proc->readAllStandardOutput(),
                                false);
            });
            connect(proc, &QProcess::readyReadStandardError, this, [pane, proc] {
                hostLiveConsume(*pane, pane->stderrTail, proc->readAllStandardError(),
                                true);
            });
            connect(proc, &QProcess::errorOccurred, this,
                    [pane](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                if (pane->state)
                    pane->state->setText(QStringLiteral("SSH could not start"));
                hostLiveNote(*pane, QStringLiteral(
                    "[error] Could not start SSH. Install OpenSSH (and sshpass "
                    "only when using password login) and try again."));
            });
            connect(proc, &QProcess::finished, this,
                    [this, pane, proc](int code, QProcess::ExitStatus exitStatus) {
                m_hostsLiveProcesses.removeAll(QPointer<QProcess>(proc));
                if (pane->state) {
                    pane->state->setText(
                        exitStatus == QProcess::NormalExit && code == 0
                            ? QStringLiteral("Disconnected")
                            : QStringLiteral("Disconnected (exit %1)").arg(code));
                }
                if (pane->traffic)
                    pane->traffic->clear();
                proc->deleteLater();
            });
            proc->start(ssh.program, ssh.arguments);
        };
        connect(reconnect, &QPushButton::clicked, this, [pane, launch] {
            if (pane->process && pane->process->state() != QProcess::NotRunning)
                return;
            hostLiveNote(*pane, QStringLiteral("[reconnecting]"));
            launch();
        });
        launch();
    }
}

// One saved host's main-log feed. Kept by key in m_hostLogFeeds; a feed that
// has been dropped from there (the host was removed or its login changed) is
// left to finish and is never relaunched.
//
// The feed rides QUIC when it can: the host's gateway serves its journal at
// /v1/node/logs as server-sent events, Caddy puts that on HTTP/3, and
// forkmesh::net::Http3Get (OpenSSL QUIC + nghttp3; QtNetwork has no HTTP/3)
// reads it in-process. A host that has not been redeployed with the endpoint,
// a blocked UDP 443 or a build without HTTP/3 drops the feed back to SSH,
// which retries QUIC every kHostLogFeedQuicRetryMs.
struct MainWindow::HostLogFeed {
    QString node; // name the main log shows, the address when unnamed
    QString signedNode; // the gateway's node name, which the request signs
    QString ip;
    QString user;
    QString pass;
    QString identity;
    QUrl origin; // https://<host>, empty when the host has no public origin
    QPointer<QProcess> process;              // the SSH feed, when on SSH
    QPointer<forkmesh::net::Http3Get> http3; // the QUIC reader
    quint64 generation = 0;  // bumps per connection; stale callbacks see it
    bool quic = false;       // the current connection is the QUIC reader
    bool gotData = false;    // it has delivered at least one line
    bool recycling = false;  // stopped on purpose to try QUIC again
    QByteArray stdoutTail;
    QByteArray stderrTail;
    QString lastError;       // why the QUIC reader failed, for the note
    qint64 lastReadMs = 0;
    qint64 startedMs = 0;
    qint64 sshUntilMs = 0;   // QUIC failed; stay on SSH until then
    int failures = 0; // consecutive short-lived connections, for the backoff

    bool running() const
    {
        return http3 ||
               (process && process->state() != QProcess::NotRunning);
    }

    // Hangs up without a callback; used once the feed is no longer wanted.
    void stop()
    {
        ++generation;
        if (process)
            process->kill();
        if (http3) {
            http3->abort();
            http3->deleteLater();
            http3 = nullptr;
        }
    }
};

namespace {

// A connection that went quiet for this long (the QUIC feed sends a keepalive
// every 10 s, the SSH feed every 30 s) is dead without its transport having noticed; it is dropped and
// redialled.
constexpr qint64 kHostLogFeedSilenceMs = 120 * 1000;
// Retry after 30 s, doubling per failed attempt, capped at ten minutes. A
// connection that stayed up for a few minutes resets the count.
constexpr int kHostLogFeedRetryBaseMs = 30 * 1000;
constexpr int kHostLogFeedRetryMaxMs = 10 * 60 * 1000;
constexpr qint64 kHostLogFeedHealthyMs = 3 * 60 * 1000;
// How long a host stays on the SSH fallback before QUIC is tried again.
constexpr int kHostLogFeedQuicRetryMs = 15 * 60 * 1000;

// The saved host's public HTTPS origin, the same way the fleet probe finds it:
// publicOrigin, then dnsHostname, then <name>.<the relay's zone>.
QUrl hostLogFeedOrigin(const QJsonObject &host, const QString &node,
                       const QUrl &catalog)
{
    QUrl origin(host.value(QStringLiteral("publicOrigin")).toString());
    if (origin.host().isEmpty()) {
        const QString dns =
            host.value(QStringLiteral("dnsHostname")).toString().trimmed();
        if (!dns.isEmpty())
            origin = QUrl(QStringLiteral("https://") + dns);
    }
    if (origin.host().isEmpty() && !node.isEmpty()) {
        const QStringList labels = catalog.host().trimmed().toLower().split(
            QLatin1Char('.'), Qt::SkipEmptyParts);
        if (labels.size() >= 3)
            origin = QUrl(QStringLiteral("https://%1.%2")
                              .arg(node, labels.mid(1).join(QLatin1Char('.'))));
    }
    if (!origin.isValid() || origin.scheme() != QLatin1String("https") ||
        origin.host().isEmpty() || !origin.userInfo().isEmpty())
        return QUrl();
    origin.setPath(QString());
    origin.setQuery(QString());
    origin.setFragment(QString());
    return origin;
}

} // namespace

void MainWindow::forwardHostLogLine(const QString &node, int source,
                                    const QString &text)
{
    // Every source, before the main-log filter: the owner's phone watching
    // this host's live log sees the journal the Live view shows.
    tapHostLogLineForPhones(node, source, text);
    if (!forkmesh::hosts::forwardsToMainLog(source) || text.trimmed().isEmpty())
        return;
    // Shared by the live view and the background feed, which both replay each
    // log's tail when they connect: whichever sees a line first forwards it.
    static forkmesh::hosts::ForwardedHostLines forwarded;
    if (!forwarded.firstSighting(node, QString::number(source) + text))
        return;
    logSystemFrom(forkmesh::hosts::mainLogLine(node, source, text),
                  forkmesh::logSourceRelativePath(FORKMESH_LOG_SOURCE_FILE),
                  FORKMESH_LOG_SOURCE_LINE, forkmesh::LogSeverity::Automatic,
                  QStringLiteral("MIRROR"));
}

void MainWindow::syncHostLogFeeds()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#else
    if (!m_hostLogFeedsArmed)
        return;
    if (!loggingEnabled()) {
        for (const QSharedPointer<HostLogFeed> &feed : std::as_const(m_hostLogFeeds))
            feed->stop();
        m_hostLogFeeds.clear();
        return;
    }
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    const QUrl catalog = catalogApiUrl();
    QSet<QString> wanted;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        const QString name = host.value(QStringLiteral("name")).toString();
        const QString ip = host.value(QStringLiteral("ip")).toString();
        const QString user = host.value(QStringLiteral("user")).toString();
        if (ip.isEmpty() || user.isEmpty())
            continue;
        const QString key =
            forkmesh::control::savedHostCredentialKey(name, ip, user);
        const QString pass = m_hostSessionPasswords.value(key);
        const QString identity = savedHostIdentityFile(name, ip, user);
        const QString signedNode = name.trimmed().toLower();
        const QUrl origin = hostLogFeedOrigin(host, signedNode, catalog);
        wanted.insert(key);
        const QSharedPointer<HostLogFeed> existing = m_hostLogFeeds.value(key);
        if (existing && existing->pass == pass &&
            existing->identity == identity && existing->origin == origin)
            continue;
        if (existing)
            existing->stop();
        auto feed = QSharedPointer<HostLogFeed>::create();
        feed->node = name.isEmpty() ? ip : name;
        feed->signedNode = signedNode;
        feed->ip = ip;
        feed->user = user;
        feed->pass = pass;
        feed->identity = identity;
        feed->origin = origin;
        m_hostLogFeeds.insert(key, feed);
        launchHostLogFeed(key);
    }
    for (auto it = m_hostLogFeeds.begin(); it != m_hostLogFeeds.end();) {
        if (wanted.contains(it.key())) {
            ++it;
            continue;
        }
        it.value()->stop();
        it = m_hostLogFeeds.erase(it);
    }
#endif
}

void MainWindow::launchHostLogFeed(const QString &key)
{
    const QSharedPointer<HostLogFeed> feed = m_hostLogFeeds.value(key);
    if (!feed || feed->running())
        return;
    const quint64 generation = ++feed->generation;
    feed->stdoutTail.clear();
    feed->stderrTail.clear();
    feed->lastError.clear();
    feed->gotData = false;
    feed->recycling = false;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString viewerKey = m_profileIdentity.publicKey();
    const bool quic = now >= feed->sshUntilMs && feed->origin.isValid() &&
                      !feed->signedNode.isEmpty() && !viewerKey.isEmpty() &&
                      forkmesh::net::http3Available();
    feed->quic = quic;
    feed->startedMs = feed->lastReadMs = now;

    // Splits a chunk into lines and forwards the log lines among them. The QUIC
    // reader's lines arrive as "data: <tagged line>" events; either feed's
    // keepalives only prove the connection is alive.
    const auto consume = [this, feed](QByteArray &tail, const QByteArray &chunk,
                                      bool fromStderr) {
        feed->lastReadMs = QDateTime::currentMSecsSinceEpoch();
        tail.append(chunk);
        int start = 0;
        for (int newline = tail.indexOf('\n'); newline >= 0;
             newline = tail.indexOf('\n', start)) {
            QString line = QString::fromUtf8(tail.constData() + start,
                                             newline - start);
            start = newline + 1;
            if (line.endsWith(QLatin1Char('\r')))
                line.chop(1);
            if (fromStderr) {
                if (!line.trimmed().isEmpty())
                    forwardHostLogLine(feed->node,
                                       forkmesh::hosts::kFallbackLogSource,
                                       QStringLiteral("[ssh] ") + line);
                continue;
            }
            if (feed->quic) {
                if (!line.startsWith(QLatin1String("data: ")))
                    continue;
                line.remove(0, 6);
            }
            feed->gotData = true;
            QString text;
            const int source = forkmesh::hosts::feedLogLine(line, &text);
            if (source >= 0)
                forwardHostLogLine(feed->node, source, text);
        }
        tail.remove(0, start);
        if (tail.size() > 64 * 1024)
            tail.clear();
    };

    // The connection is over, however it ended: fall back, recycle or redial.
    // Runs at most once per connection; a stale transport's late callback
    // finds the generation moved on and does nothing.
    const auto ended = [this, key, feed, generation](const QString &how) {
        if (feed->generation != generation)
            return;
        ++feed->generation;
        feed->process = nullptr;
        feed->http3 = nullptr;
        // Dropped or replaced by syncHostLogFeeds: this feed is over.
        if (m_hostLogFeeds.value(key) != feed)
            return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (feed->quic && !feed->gotData) {
            // QUIC never opened: go straight over SSH, and try QUIC again later.
            feed->sshUntilMs = now + kHostLogFeedQuicRetryMs;
            forwardHostLogLine(
                feed->node, forkmesh::hosts::kFallbackLogSource,
                QStringLiteral("[log feed] QUIC unavailable (%1); following "
                               "over SSH and retrying QUIC in %2 min")
                    .arg(how)
                    .arg(kHostLogFeedQuicRetryMs / 60000));
            launchHostLogFeed(key);
            return;
        }
        if (feed->recycling) {
            launchHostLogFeed(key);
            return;
        }
        feed->failures = now - feed->startedMs >= kHostLogFeedHealthyMs
                             ? 0
                             : feed->failures + 1;
        const int delay = int(qMin<qint64>(
            qint64(kHostLogFeedRetryBaseMs) << qMin(feed->failures, 5),
            kHostLogFeedRetryMaxMs));
        forwardHostLogLine(
            feed->node, forkmesh::hosts::kFallbackLogSource,
            QStringLiteral("[log feed] %1 connection closed (%2); "
                           "reconnecting in %3 s")
                .arg(feed->quic ? QStringLiteral("QUIC") : QStringLiteral("SSH"),
                     how)
                .arg(delay / 1000));
        QTimer::singleShot(delay, this, [this, key] { launchHostLogFeed(key); });
    };

    // Both transports get the same silence watchdog, owned by the transport so
    // it goes with it.
    const auto watch = [feed](QObject *transport, std::function<void()> drop) {
        auto *watchdog = new QTimer(transport);
        watchdog->setInterval(60 * 1000);
        QObject::connect(watchdog, &QTimer::timeout, transport,
                         [feed, drop = std::move(drop)] {
                             if (QDateTime::currentMSecsSinceEpoch() -
                                     feed->lastReadMs >
                                 kHostLogFeedSilenceMs)
                                 drop();
                         });
        watchdog->start();
    };

    if (quic) {
        const QString requestId =
            QUuid::createUuid().toString(QUuid::Id128).left(32);
        const QString signature = m_profileIdentity.signData(
            forkmesh::hosts::nodeLogsMessage(feed->signedNode, viewerKey,
                                             requestId, now)
                .toUtf8());
        QUrl url = feed->origin;
        url.setPath(QStringLiteral("/v1/node/logs"));
        auto *get = new forkmesh::net::Http3Get(this);
        feed->http3 = get;
        get->onHeaders = [feed](int status) {
            if (status != 200)
                feed->lastError = QStringLiteral("HTTP %1").arg(status);
        };
        get->onData = [feed, consume](const QByteArray &chunk) {
            consume(feed->stdoutTail, chunk, false);
        };
        get->onFinished = [feed, get, ended](const QString &error) {
            get->deleteLater();
            if (!error.isEmpty())
                feed->lastError = error;
            ended(feed->lastError.isEmpty() ? QStringLiteral("stream ended")
                                            : feed->lastError);
        };
        watch(get, [get, ended] {
            get->abort();
            get->deleteLater();
            ended(QStringLiteral("no data for two minutes"));
        });
        QString error;
        if (signature.isEmpty() ||
            !get->start(url,
                        {{"accept", "text/event-stream"},
                         {"x-forkmesh-log-viewer", viewerKey.toUtf8()},
                         {"x-forkmesh-request-id", requestId.toUtf8()},
                         {"x-forkmesh-issued-at", QByteArray::number(now)},
                         {"x-forkmesh-signature", signature.toUtf8()}},
                        &error)) {
            get->deleteLater();
            ended(signature.isEmpty() ? QStringLiteral("the request could not "
                                                       "be signed")
                                      : error);
        }
        return;
    }

    QString script = QString::fromUtf8(forkmesh::hosts::kMainLogFeedScript);
    script.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
    const QString remoteCmd =
        QStringLiteral("sh -lc '") + script + QStringLiteral("'");
    QString sshError;
    const forkmesh::control::HostSshCommand ssh =
        forkmesh::control::buildHostSshCommand(feed->ip, feed->user, feed->pass,
                                               remoteCmd, &sshError,
                                               feed->identity);
    if (ssh.program.isEmpty()) {
        forwardHostLogLine(feed->node, forkmesh::hosts::kFallbackLogSource,
                           QStringLiteral("[error] Cannot follow this host's "
                                          "logs: ") + sshError);
        return;
    }
    auto *proc = new QProcess(this);
    feed->process = proc;
    proc->setProcessEnvironment(ssh.environment);
    connect(proc, &QProcess::readyReadStandardOutput, this,
            [feed, proc, consume] {
                consume(feed->stdoutTail, proc->readAllStandardOutput(), false);
            });
    connect(proc, &QProcess::readyReadStandardError, this,
            [feed, proc, consume] {
                consume(feed->stderrTail, proc->readAllStandardError(), true);
            });
    watch(proc, [proc] { proc->kill(); });
    if (now < feed->sshUntilMs) {
        // On the fallback because QUIC failed: hang up when the retry is due
        // so the next connection tries QUIC first. The log tails it replays
        // are skipped as already seen.
        QTimer::singleShot(int(feed->sshUntilMs - now), proc, [feed, proc] {
            feed->recycling = true;
            proc->terminate();
        });
    }
    // A program that never starts emits no finished().
    connect(proc, &QProcess::errorOccurred, this,
            [proc, ended](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                proc->deleteLater();
                ended(QStringLiteral("SSH could not start"));
            });
    connect(proc, &QProcess::finished, this,
            [proc, ended](int code, QProcess::ExitStatus) {
                proc->deleteLater();
                ended(QStringLiteral("exit %1").arg(code));
            });
    proc->start(ssh.program, ssh.arguments);
}

void MainWindow::browseHostDiskUsageForSelection(int row)
{
    if (m_hostDiskProcess &&
        m_hostDiskProcess->state() != QProcess::NotRunning) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("A host size map is already loading."));
        return;
    }
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;
    const auto cellText = [this, row](int column) {
        const QTableWidgetItem *item = m_hostsTable->item(row, column);
        return item ? item->text().trimmed() : QString();
    };
    const QString node = cellText(0);
    const QString ip = cellText(1);
    const QString user = cellText(2);
    if (ip.isEmpty() || user.isEmpty() || node.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QStringLiteral(
                "Select a saved host row first, then click Size map."));
        return;
    }
    const QString pass = m_hostSessionPasswords.value(
        forkmesh::control::savedHostCredentialKey(node, ip, user));
    runHostDiskUsageBrowser(ip, user, pass, node);
}

namespace {

// The widgets of the host "Size map" dialog that runHostDiskUsageBrowser wires up.
struct HostDiskUsageDialog {
    QDialog *dialog = nullptr;
    QPushButton *upButton = nullptr;
    QLineEdit *pathEdit = nullptr;
    QPushButton *openButton = nullptr;
    QPushButton *refreshButton = nullptr;
    QLabel *totalLabel = nullptr;
    QTableWidget *table = nullptr;
    QLabel *status = nullptr;
    QLabel *mountStatus = nullptr;
    QVBoxLayout *mountCardsLayout = nullptr;
};

HostDiskUsageDialog buildHostDiskUsageDialog(QWidget *parent, const QString &node)
{
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("hostDiskUsageDialog"));
    dialog->setWindowTitle(QStringLiteral("Size map: %1").arg(node));
    dialog->setMinimumSize(1060, 620);
    auto *layout = new QVBoxLayout(dialog);

    auto *hint = new QLabel(QString::fromUtf8(
        "ForkMesh measures one directory level at a time with <b>du</b> over "
        "the same authenticated SSH channel the installer uses \xE2\x80\x94 "
        "nothing is written on the host. Enter any absolute folder, "
        "double-click a folder to drill in, or use a mount map on the right."));
    hint->setObjectName("mutedLabel");
    hint->setWordWrap(true);
    layout->addWidget(hint);

    auto *body = new QHBoxLayout;
    body->setSpacing(14);
    auto *mainPane = new QWidget(dialog);
    auto *mainLayout = new QVBoxLayout(mainPane);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto *nav = new QHBoxLayout;
    auto *upButton = new QPushButton(QStringLiteral("Up"));
    upButton->setObjectName(QStringLiteral("hostDiskUpButton"));
    upButton->setCursor(Qt::PointingHandCursor);
    setOcticon(upButton, "arrow-left", 12);
    nav->addWidget(upButton);
    auto *pathEdit = new QLineEdit(QStringLiteral("/"));
    pathEdit->setObjectName(QStringLiteral("hostDiskPathEdit"));
    pathEdit->setPlaceholderText(
        QStringLiteral("Absolute path on the host, e.g. /var/lib"));
    nav->addWidget(pathEdit, 1);
    auto *openButton = new QPushButton(QStringLiteral("Open"));
    openButton->setObjectName(QStringLiteral("hostDiskOpenButton"));
    openButton->setCursor(Qt::PointingHandCursor);
    openButton->setToolTip(
        QStringLiteral("Open the absolute folder entered to the left."));
    setOcticon(openButton, "file-directory", 12);
    nav->addWidget(openButton);
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    refreshButton->setObjectName(QStringLiteral("hostDiskRefreshButton"));
    refreshButton->setCursor(Qt::PointingHandCursor);
    setOcticon(refreshButton, "sync", 12);
    nav->addWidget(refreshButton);
    mainLayout->addLayout(nav);

    auto *totalLabel = new QLabel;
    totalLabel->setObjectName(QStringLiteral("hostDiskTotalLabel"));
    QFont totalFont = totalLabel->font();
    totalFont.setBold(true);
    totalLabel->setFont(totalFont);
    mainLayout->addWidget(totalLabel);

    auto *table = new QTableWidget(0, 4);
    table->setObjectName("issueTable");
    table->setHorizontalHeaderLabels(
        {QStringLiteral("Name"), QStringLiteral("Size"),
         QStringLiteral("Share of this folder"), QStringLiteral("Type")});
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setShowGrid(false);
    table->horizontalHeader()->setStretchLastSection(false);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(
        3, QHeaderView::ResizeToContents);
    mainLayout->addWidget(table, 1);

    auto *status = new QLabel(QString::fromUtf8(
        "Measuring the host root \xE2\x80\xA6"));
    status->setObjectName("mutedLabel");
    status->setWordWrap(true);
    mainLayout->addWidget(status);
    body->addWidget(mainPane, 1);

    auto *mountPane = new QWidget(dialog);
    mountPane->setObjectName(QStringLiteral("hostDiskMountPane"));
    mountPane->setMinimumWidth(235);
    mountPane->setMaximumWidth(285);
    auto *mountPaneLayout = new QVBoxLayout(mountPane);
    mountPaneLayout->setContentsMargins(10, 10, 10, 10);
    auto *mountHeading = new QLabel(QStringLiteral("Mount points"));
    QFont mountHeadingFont = mountHeading->font();
    mountHeadingFont.setBold(true);
    mountHeading->setFont(mountHeadingFont);
    mountPaneLayout->addWidget(mountHeading);
    auto *mountStatus = new QLabel(QStringLiteral("Loading mount maps..."));
    mountStatus->setObjectName(QStringLiteral("mutedLabel"));
    mountStatus->setWordWrap(true);
    mountPaneLayout->addWidget(mountStatus);
    auto *mountScroll = new QScrollArea(mountPane);
    mountScroll->setObjectName(QStringLiteral("hostDiskMountScroll"));
    mountScroll->setWidgetResizable(true);
    mountScroll->setFrameShape(QFrame::NoFrame);
    auto *mountCards = new QWidget(mountScroll);
    auto *mountCardsLayout = new QVBoxLayout(mountCards);
    mountCardsLayout->setContentsMargins(0, 0, 0, 0);
    mountCardsLayout->setSpacing(8);
    mountCardsLayout->addStretch();
    mountScroll->setWidget(mountCards);
    mountPaneLayout->addWidget(mountScroll, 1);
    body->addWidget(mountPane);
    layout->addLayout(body, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    if (auto *closeBtn = buttons->button(QDialogButtonBox::Close))
        closeBtn->setDefault(true);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog,
                     &QDialog::reject);
    layout->addWidget(buttons);

    HostDiskUsageDialog widgets;
    widgets.dialog = dialog;
    widgets.upButton = upButton;
    widgets.pathEdit = pathEdit;
    widgets.openButton = openButton;
    widgets.refreshButton = refreshButton;
    widgets.totalLabel = totalLabel;
    widgets.table = table;
    widgets.status = status;
    widgets.mountStatus = mountStatus;
    widgets.mountCardsLayout = mountCardsLayout;
    return widgets;
}

// Rebuild the mount-point mini maps from the mount half of one scan's output;
// clicking a card opens that mount through loadPath.
void fillHostDiskMountCards(
    QVBoxLayout *mountCardsLayout, QLabel *mountStatus, const QByteArray &output,
    const QString &path,
    const std::shared_ptr<std::function<void(const QString &)>> &loadPath)
{
    while (QLayoutItem *item = mountCardsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    const forkmesh::control::HostMountUsageList mounts =
        forkmesh::control::parseHostMountUsage(output);
    if (!mounts.error.isEmpty()) {
        mountStatus->setText(mounts.error);
    } else if (!mounts.complete || mounts.mounts.isEmpty()) {
        mountStatus->setText(QStringLiteral(
            "Mount details were not reported by this host."));
    } else {
        mountStatus->setText(QString::fromUtf8(
            "%1 filesystem%2 \xE2\x80\x94 click a mini map to "
            "open any mount.")
                                 .arg(mounts.mounts.size())
                                 .arg(mounts.mounts.size() == 1
                                          ? QString()
                                          : QStringLiteral("s")));
        QString activeMountPath;
        for (const forkmesh::control::HostMountUsage &mount :
             mounts.mounts) {
            const bool containsPath =
                path == mount.path ||
                (mount.path == QStringLiteral("/")
                     ? path.startsWith(QLatin1Char('/'))
                     : path.startsWith(mount.path +
                                       QLatin1Char('/')));
            if (containsPath &&
                mount.path.size() > activeMountPath.size()) {
                activeMountPath = mount.path;
            }
        }
        for (const forkmesh::control::HostMountUsage &mount :
             mounts.mounts) {
            const double usedShare =
                mount.totalBytes > 0
                    ? qBound(
                          0.0,
                          static_cast<double>(mount.usedBytes) /
                              static_cast<double>(
                                  mount.totalBytes),
                          1.0)
                    : 0.0;
            const int filled =
                qBound(0,
                       static_cast<int>(
                           std::lround(usedShare * 12.0)),
                       12);
            const QString miniMap =
                QString(filled, QChar(0x2588)) +
                QString(12 - filled, QChar(0x2591));
            auto *mountButton = new QPushButton(
                QStringLiteral("%1\n%2  %3%\n%4 of %5")
                    .arg(mount.path, miniMap)
                    .arg(usedShare * 100.0, 0, 'f', 0)
                    .arg(forkmesh::control::formatDiskSize(
                             mount.usedBytes),
                         forkmesh::control::formatDiskSize(
                             mount.totalBytes)));
            mountButton->setObjectName(
                QStringLiteral("hostDiskMountButton"));
            mountButton->setProperty("mountPath", mount.path);
            mountButton->setCursor(Qt::PointingHandCursor);
            mountButton->setCheckable(true);
            mountButton->setChecked(
                mount.path == activeMountPath);
            mountButton->setToolTip(
                QString::fromUtf8(
                    "Open %1 \xE2\x80\x94 %2 available")
                    .arg(mount.path,
                         forkmesh::control::formatDiskSize(
                             mount.availableBytes)));
            QObject::connect(mountButton, &QPushButton::clicked,
                    mountButton,
                    [loadPath, mount] {
                        (*loadPath)(mount.path);
                    });
            mountCardsLayout->addWidget(mountButton);
        }
    }
    mountCardsLayout->addStretch();
}

// Show one folder's entries, largest first, with their share of the folder.
void fillHostDiskUsageTable(QTableWidget *table, QLabel *status,
                            QLabel *totalLabel,
                            const forkmesh::control::HostDiskUsage &usage,
                            const QString &path)
{
    totalLabel->setText(
        QString::fromUtf8("%1 \xE2\x80\x94 %2 total, %3 entries")
            .arg(path,
                 forkmesh::control::formatDiskSize(
                     usage.totalBytes))
            .arg(usage.entries.size()));
    table->setRowCount(usage.entries.size());
    for (int i = 0; i < usage.entries.size(); ++i) {
        const forkmesh::control::HostDiskEntry &entry =
            usage.entries.at(i);
        auto *nameItem = new QTableWidgetItem(entry.name);
        nameItem->setIcon(themedOcticon(
            entry.directory ? QStringLiteral("file-directory")
                            : QStringLiteral("file"),
            Theme::remapColor(QColor("#8b949e")), 13));
        nameItem->setToolTip(entry.path);
        nameItem->setData(Qt::UserRole, entry.path);
        nameItem->setData(Qt::UserRole + 1, entry.directory);
        table->setItem(i, 0, nameItem);
        table->setItem(
            i, 1,
            new QTableWidgetItem(
                forkmesh::control::formatDiskSize(entry.bytes)));
        const double share =
            usage.totalBytes > 0
                ? static_cast<double>(entry.bytes) /
                      static_cast<double>(usage.totalBytes)
                : 0.0;
        const int filled = qBound(
            0, static_cast<int>(std::lround(share * 20.0)), 20);
        table->setItem(
            i, 2,
            new QTableWidgetItem(
                QStringLiteral("%1 %2%")
                    .arg(QString(filled, QChar(0x2588)) +
                         QString(20 - filled, QChar(0x2591)))
                    .arg(share * 100.0, 0, 'f', 1)));
        table->setItem(
            i, 3,
            new QTableWidgetItem(entry.directory
                                     ? QStringLiteral("Folder")
                                     : QStringLiteral("File")));
    }
    status->setText(
        usage.entries.isEmpty()
            ? QString::fromUtf8(
                  "%1 holds nothing this SSH user can read.")
                  .arg(path)
            : QString::fromUtf8(
                  "Largest first. Double-click a folder to "
                  "drill in, or Up to go back."));
}

} // namespace

void MainWindow::runHostDiskUsageBrowser(const QString &ip, const QString &user,
                                         const QString &pass,
                                         const QString &node)
{
    if (ip.isEmpty() || user.isEmpty() || node.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(
                QStringLiteral("Enter host IP, SSH username and node name."));
        return;
    }

    const HostDiskUsageDialog widgets = buildHostDiskUsageDialog(this, node);
    auto *dialog = widgets.dialog;
    auto *upButton = widgets.upButton;
    auto *pathEdit = widgets.pathEdit;
    auto *openButton = widgets.openButton;
    auto *refreshButton = widgets.refreshButton;
    auto *totalLabel = widgets.totalLabel;
    auto *table = widgets.table;
    auto *status = widgets.status;
    auto *mountStatus = widgets.mountStatus;
    auto *mountCardsLayout = widgets.mountCardsLayout;

    const QString identityFile = savedHostIdentityFile(node, ip, user);
    const QString credentialKey =
        forkmesh::control::savedHostCredentialKey(node, ip, user);
    auto sessionPass = std::make_shared<QString>(pass);

    auto currentPath = std::make_shared<QString>(QStringLiteral("/"));
    auto loadPath = std::make_shared<std::function<void(const QString &)>>();
    *loadPath = [this, dialog, table, pathEdit, status, totalLabel, upButton,
                 openButton, refreshButton, currentPath, mountCardsLayout,
                 mountStatus, loadPath, ip, user, sessionPass, identityFile,
                 credentialKey, node](const QString &requested) {
        if (m_hostDiskProcess &&
            m_hostDiskProcess->state() != QProcess::NotRunning) {
            status->setText(QStringLiteral(
                "Still measuring the previous folder on this host."));
            return;
        }
        const QString path =
            forkmesh::control::normalizeRemoteDiskPath(requested);
        QString commandError;
        const QString diskCommand =
            forkmesh::control::buildHostDiskUsageCommand(path, &commandError);
        if (diskCommand.isEmpty()) {
            status->setText(commandError);
            pathEdit->setText(*currentPath);
            return;
        }
        const QString remoteCommand =
            forkmesh::control::buildHostMountUsageCommand() +
            QLatin1Char('\n') + diskCommand;
        QString sshError;
        const forkmesh::control::HostSshCommand ssh =
            forkmesh::control::buildHostSshCommand(
                ip, user, *sessionPass, remoteCommand, &sshError,
                identityFile);
        if (ssh.program.isEmpty()) {
            status->setText(sshError);
            return;
        }

        *currentPath = path;
        pathEdit->setText(path);
        table->setRowCount(0);
        totalLabel->setText(QString());
        status->setText(
            QString::fromUtf8("Measuring %1 on %2@%3 \xE2\x80\xA6 a large "
                              "directory tree can take a minute.")
                .arg(path, user, ip));

        const QList<QWidget *> navWidgets{upButton, openButton, refreshButton,
                                          pathEdit};
        for (QWidget *w : navWidgets)
            w->setEnabled(false);

        auto *proc = new QProcess(dialog);
        m_hostDiskProcess = proc;
        auto output = std::make_shared<QByteArray>();
        proc->setProcessChannelMode(QProcess::MergedChannels);
        proc->setProcessEnvironment(ssh.environment);
        connect(proc, &QProcess::readyReadStandardOutput, dialog,
                [proc, output] { output->append(proc->readAllStandardOutput()); });
        connect(proc, &QProcess::finished, dialog,
                [this, dialog, proc, output, table, status, totalLabel,
                 navWidgets, path, ip, user, mountCardsLayout, mountStatus,
                 loadPath, sessionPass, credentialKey](
                    int code, QProcess::ExitStatus exitStatus) {
                    if (m_hostDiskProcess == proc)
                        m_hostDiskProcess = nullptr;
                    for (QWidget *w : navWidgets)
                        w->setEnabled(true);
                    fillHostDiskMountCards(mountCardsLayout, mountStatus, *output,
                                           path, loadPath);
                    const forkmesh::control::HostDiskUsage usage =
                        forkmesh::control::parseHostDiskUsage(*output, path);
                    proc->deleteLater();
                    if (!usage.error.isEmpty()) {
                        status->setText(usage.error);
                        return;
                    }
                    if (!usage.complete) {
                        const QString tail = forkmesh::control::redactProcessOutput(
                            QString::fromUtf8(output->right(2048)));
                        QString message =
                            QString::fromUtf8(
                                "Could not read the size map for %1 (exit %2).")
                                .arg(path)
                                .arg(exitStatus == QProcess::NormalExit ? code
                                                                        : 255);
                        const QString sshHint =
                            forkmesh::control::sshConnectionFailureHint(
                                exitStatus == QProcess::NormalExit ? code : 255,
                                tail, ip);
                        if (!sshHint.isEmpty())
                            message += QLatin1Char(' ') + sshHint;
                        // Without a password ssh runs BatchMode/publickey-only,
                        // so a password-login host can never finish this scan:
                        // ask for the one credential that would, then retry the
                        // same folder. The shared fleet key is offered to every
                        // host, so a rejected key is exactly the case that needs
                        // the prompt — only an already-entered password is a
                        // reason not to ask again.
                        const int sshExit =
                            exitStatus == QProcess::NormalExit ? code : 255;
                        if (sessionPass->isEmpty() &&
                            forkmesh::control::sshFailureNeedsPassword(sshExit,
                                                                       tail)) {
                            status->setText(
                                message +
                                QString::fromUtf8(
                                    " Asking for this host's SSH password "
                                    "\xE2\x80\xA6"));
                            QTimer::singleShot(
                                0, dialog,
                                [this, guard = QPointer<QDialog>(dialog),
                                 status, loadPath, sessionPass, credentialKey,
                                 path, ip, user] {
                                    promptHostDiskSshPassword(
                                        guard, status, loadPath, sessionPass,
                                        credentialKey, path, ip, user);
                                });
                            return;
                        }
                        status->setText(message);
                        return;
                    }

                    fillHostDiskUsageTable(table, status, totalLabel, usage,
                                           path);
                });
        connect(proc, &QProcess::errorOccurred, dialog,
                [this, proc, status, navWidgets](QProcess::ProcessError e) {
                    if (e != QProcess::FailedToStart)
                        return;
                    if (m_hostDiskProcess == proc)
                        m_hostDiskProcess = nullptr;
                    for (QWidget *w : navWidgets)
                        w->setEnabled(true);
                    status->setText(QString::fromUtf8(
                        "Could not start SSH. Install OpenSSH (and sshpass only "
                        "when using password login) and try again."));
                });
        proc->start(ssh.program, ssh.arguments);
    };

    connect(upButton, &QPushButton::clicked, dialog, [currentPath, loadPath] {
        (*loadPath)(*currentPath + QStringLiteral("/.."));
    });
    connect(refreshButton, &QPushButton::clicked, dialog,
            [currentPath, loadPath] { (*loadPath)(*currentPath); });
    connect(openButton, &QPushButton::clicked, dialog,
            [pathEdit, loadPath] { (*loadPath)(pathEdit->text()); });
    connect(pathEdit, &QLineEdit::returnPressed, dialog,
            [pathEdit, loadPath] { (*loadPath)(pathEdit->text()); });
    connect(table, &QTableWidget::cellDoubleClicked, dialog,
            [table, status, loadPath](int row, int) {
                const QTableWidgetItem *item = table->item(row, 0);
                if (!item)
                    return;
                const QString path = item->data(Qt::UserRole).toString();
                if (!item->data(Qt::UserRole + 1).toBool()) {
                    status->setText(
                        QStringLiteral("%1 is a file, not a folder.").arg(path));
                    return;
                }
                (*loadPath)(path);
            });

    connect(dialog, &QDialog::finished, this, [this](int) {
        if (!m_hostDiskProcess)
            return;
        if (m_hostDiskProcess->state() != QProcess::NotRunning) {
            // code-health: allow(blocking-wait) dialog-close stop path; waits at most 250 ms for SIGTERM, then kills
            m_hostDiskProcess->terminate();
            if (!m_hostDiskProcess->waitForFinished(250))
                m_hostDiskProcess->kill();
        }
        m_hostDiskProcess = nullptr;
    });

    if (m_hostInstallStatus) {
        m_hostInstallStatus->setText(
            QString::fromUtf8("Opening the size map for %1@%2 (%3)...")
                .arg(user, ip, node));
    }
    (*loadPath)(QStringLiteral("/"));
    dialog->exec();
}

// A password-login host rejected the key-only scan: ask for its SSH password
// (kept for this session only), then measure the same folder again.
void MainWindow::promptHostDiskSshPassword(
    QPointer<QDialog> guard, QLabel *status,
    const std::shared_ptr<std::function<void(const QString &)>> &loadPath,
    const std::shared_ptr<QString> &sessionPass, const QString &credentialKey,
    const QString &path, const QString &ip, const QString &user)
{
    QDialog *dialog = guard.data();
    if (!dialog)
        return;
    bool accepted = false;
    const QString entered =
        QInputDialog::getText(
            dialog,
            QStringLiteral(
                "SSH password needed"),
            QString::fromUtf8(
                "%1@%2 rejected the login "
                "ForkMesh tried. Enter that "
                "host's SSH password to "
                "measure its disk \xE2\x80\x94 "
                "it is kept in memory for this "
                "session only and is never "
                "written to settings.")
                .arg(user, ip),
            QLineEdit::Password, *sessionPass,
            &accepted);
    if (!guard)
        return;
    if (!accepted || entered.isEmpty()) {
        status->setText(QString::fromUtf8(
            "The size map needs an SSH "
            "password (or a working key) for "
            "%1@%2. Press Refresh to try "
            "again.")
                            .arg(user, ip));
        return;
    }
    *sessionPass = entered;
    m_hostSessionPasswords.insert(credentialKey,
                                  entered);
    (*loadPath)(path);
}

void MainWindow::addHostFromForm()
{
    const QString ip = m_hostIpEdit ? m_hostIpEdit->text().trimmed() : QString();
    const QString user = m_hostUserEdit ? m_hostUserEdit->text().trimmed() : QString();
    const QString pass = m_hostPassEdit ? m_hostPassEdit->text() : QString();
    const QString node = m_hostNameEdit ? m_hostNameEdit->text().trimmed() : QString();
    if (ip.isEmpty() || user.isEmpty() || node.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QString::fromUtf8(
                "Enter the host IP, SSH username and a node name to add a host."));
        return;
    }
    const QString identityFile =
        forkmesh::control::existingSharedHostIdentityFile();
    rememberHost(node, ip, user, pass, QStringLiteral("added"), identityFile);
    if (m_hostInstallStatus) {
        m_hostInstallStatus->setText(
            identityFile.isEmpty()
                ? QString::fromUtf8(
                      "Saved \"%1\". Click Install ForkMesh to provision it.")
                      .arg(node)
                : QString::fromUtf8(
                      "Saved \"%1\" against the shared ForkMesh SSH key. Click "
                      "Install ForkMesh to provision it.")
                      .arg(node));
    }
}

void MainWindow::rememberHost(const QString &name, const QString &ip,
                              const QString &user, const QString &pass, const QString &status,
                              const QString &identityFile,
                              const QJsonObject &metadata)
{
    QStringList fleetAddresses;
    for (const forkmesh::corelogs::CoreNode &node : forkmesh::corelogs::loadFleetNodes())
        fleetAddresses.append(node.address);
    if (forkmesh::fleet::isFleetInstanceHost(name, ip, fleetAddresses))
        return;
    QSettings settings;
    QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QJsonObject entry;
    entry.insert(QStringLiteral("name"), name);
    entry.insert(QStringLiteral("ip"), ip);
    entry.insert(QStringLiteral("user"), user);
    entry.insert(QStringLiteral("status"), status);
    // Dates the label so an abandoned "installing" can be told apart from an
    // install that is still running.
    entry.insert(QStringLiteral("statusChangedAt"),
                 double(QDateTime::currentMSecsSinceEpoch()));
    if (!identityFile.trimmed().isEmpty())
        entry.insert(QStringLiteral("identityFile"), identityFile.trimmed());
    bool replaced = false;
    for (int i = 0; i < hosts.size(); ++i) {
        if (hosts.at(i).toObject().value("name").toString() == name) {
            const QJsonObject old = hosts.at(i).toObject();
            if (old.value(QStringLiteral("status")).toString() == status &&
                old.contains(QStringLiteral("statusChangedAt"))) {
                entry.insert(QStringLiteral("statusChangedAt"),
                             old.value(QStringLiteral("statusChangedAt")));
            }
            entry = forkmesh::control::mergeSavedHostRecord(old, entry,
                                                            metadata);
            const QString oldCredentialKey =
                forkmesh::control::savedHostCredentialKey(
                    old.value(QStringLiteral("name")).toString(),
                    old.value(QStringLiteral("ip")).toString(),
                    old.value(QStringLiteral("user")).toString());
            const QString newCredentialKey =
                forkmesh::control::savedHostCredentialKey(name, ip, user);
            if (oldCredentialKey != newCredentialKey) {
                QString oldPassword =
                    m_hostSessionPasswords.take(oldCredentialKey);
                oldPassword.fill(QChar::Null);
            }
            hosts.replace(i, entry);
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        hosts.append(
            forkmesh::control::mergeSavedHostRecord({}, entry, metadata));
    }
    const QString credentialKey =
        forkmesh::control::savedHostCredentialKey(name, ip, user);
    if (pass.isEmpty()) {
        QString oldPassword = m_hostSessionPasswords.take(credentialKey);
        oldPassword.fill(QChar::Null);
    } else {
        QString oldPassword = m_hostSessionPasswords.take(credentialKey);
        oldPassword.fill(QChar::Null);
        m_hostSessionPasswords.insert(credentialKey, pass);
    }
    forkmesh::control::saveSavedHosts(settings, kHostsSetting, hosts);
    // Saving a host under this name means a machine answers to it again, so
    // drop any tombstone a predecessor of the same name left behind.
    clearDecommissionedMirrorNode(name);
    clearDeletedMeshNode(name);
    refreshHostsTable();
}

void MainWindow::promoteSavedHostsProvenHealthy(const QJsonArray &mirrors,
                                                const QString &evidence)
{
    if (mirrors.isEmpty())
        return;
    QSettings settings;
    QStringList promoted;
    const QJsonArray hosts =
        forkmesh::control::promoteSavedHostsProvenHealthy(
            forkmesh::control::loadSavedHosts(settings, kHostsSetting,
                                              &m_hostSessionPasswords),
            mirrors, QDateTime::currentMSecsSinceEpoch(), &promoted);
    // Every caller runs on a periodic sweep: rewrite the settings file only
    // when a label actually changes.
    if (promoted.isEmpty())
        return;
    forkmesh::control::saveSavedHosts(settings, kHostsSetting, hosts);
    for (const QString &node : std::as_const(promoted)) {
        logSystem(QStringLiteral(
                      "Hosts: %1 was saved as not installed, but %2 shows its "
                      "node online with integrity ok; marked it installed.")
                      .arg(node, evidence));
    }
    refreshHostsTable();
}

const QJsonObject &MainWindow::decommissionedMirrorNodes()
{
    if (!m_decommissionedNodesLoaded) {
        m_decommissionedNodesLoaded = true;
        m_decommissionedNodes =
            QJsonDocument::fromJson(QSettings()
                                        .value(kDecommissionedNodesSetting)
                                        .toString()
                                        .toUtf8())
                .object();
    }
    return m_decommissionedNodes;
}

void MainWindow::recordDecommissionedMirrorNode(const QString &node)
{
    const QJsonObject updated = forkmesh::control::recordDecommissionedNode(
        decommissionedMirrorNodes(), node,
        QDateTime::currentMSecsSinceEpoch());
    if (updated == m_decommissionedNodes)
        return;
    m_decommissionedNodes = updated;
    QSettings().setValue(
        kDecommissionedNodesSetting,
        QString::fromUtf8(
            QJsonDocument(m_decommissionedNodes).toJson(QJsonDocument::Compact)));
    // The panel is built from a cached /mirrors payload that still carries the
    // deleted node, so redraw rather than waiting for the next poll. Queued:
    // the rebuild pumps the event loop for its git reads, and the callers here
    // (the Hosts row action, the Vultr destroy reply) are mid-callback.
    if (m_mirrorNodesTable)
        QTimer::singleShot(0, this, [this] { loadMirrorNodesPanel(); });
}

void MainWindow::clearDecommissionedMirrorNode(const QString &node)
{
    const QJsonObject updated = forkmesh::control::forgetDecommissionedNode(
        decommissionedMirrorNodes(), node);
    if (updated == m_decommissionedNodes)
        return;
    m_decommissionedNodes = updated;
    QSettings().setValue(
        kDecommissionedNodesSetting,
        QString::fromUtf8(
            QJsonDocument(m_decommissionedNodes).toJson(QJsonDocument::Compact)));
}

bool MainWindow::mirrorNodeWasDecommissioned(const QString &node,
                                             qint64 lastSeenMs)
{
    return forkmesh::control::mirrorNodeIsDecommissioned(
        decommissionedMirrorNodes(), node, lastSeenMs,
        QDateTime::currentMSecsSinceEpoch());
}

const QJsonObject &MainWindow::deletedMeshNodes()
{
    if (!m_deletedMeshNodesLoaded) {
        m_deletedMeshNodesLoaded = true;
        m_deletedMeshNodes =
            QJsonDocument::fromJson(QSettings()
                                        .value(kDeletedMeshNodesSetting)
                                        .toString()
                                        .toUtf8())
                .object();
    }
    return m_deletedMeshNodes;
}

void MainWindow::recordDeletedMeshNode(const QString &node)
{
    const QJsonObject updated = forkmesh::control::recordDecommissionedNode(
        deletedMeshNodes(), node, QDateTime::currentMSecsSinceEpoch());
    if (updated == m_deletedMeshNodes)
        return;
    m_deletedMeshNodes = updated;
    QSettings().setValue(
        kDeletedMeshNodesSetting,
        QString::fromUtf8(
            QJsonDocument(m_deletedMeshNodes).toJson(QJsonDocument::Compact)));
}

void MainWindow::clearDeletedMeshNode(const QString &node)
{
    const QJsonObject updated = forkmesh::control::forgetDecommissionedNode(
        deletedMeshNodes(), node);
    if (updated == m_deletedMeshNodes)
        return;
    m_deletedMeshNodes = updated;
    QSettings().setValue(
        kDeletedMeshNodesSetting,
        QString::fromUtf8(
            QJsonDocument(m_deletedMeshNodes).toJson(QJsonDocument::Compact)));
}

bool MainWindow::meshNodeWasDeleted(const QString &node, qint64 lastSeenMs)
{
    return forkmesh::control::mirrorNodeIsDecommissioned(
        deletedMeshNodes(), node, lastSeenMs,
        QDateTime::currentMSecsSinceEpoch());
}

void MainWindow::forgetHostAtRow(int row)
{
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;
    QSettings settings;
    QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    if (row >= hosts.size())
        return;
    const QJsonObject removed = hosts.at(row).toObject();
    hosts.removeAt(row);
    const QString credentialKey = forkmesh::control::savedHostCredentialKey(
        removed.value(QStringLiteral("name")).toString(),
        removed.value(QStringLiteral("ip")).toString(),
        removed.value(QStringLiteral("user")).toString());
    QString oldPassword = m_hostSessionPasswords.take(credentialKey);
    oldPassword.fill(QChar::Null);
    forkmesh::control::saveSavedHosts(settings, kHostsSetting, hosts);
    refreshHostsTable();
    // The relay keeps this node's last signed catalog row for the length of its
    // freshness lease, so without a local tombstone the Mirror nodes panel goes
    // on listing the host — online, healthy, serving — for another ten minutes.
    recordDecommissionedMirrorNode(
        removed.value(QStringLiteral("name")).toString());
    if (m_hostInstallStatus) {
        m_hostInstallStatus->setText(QString::fromUtf8(
            "Removed \"%1\" from the saved hosts list.")
                .arg(removed.value(QStringLiteral("name")).toString()));
    }
}

void MainWindow::destroyVultrHostAtRow(int row)
{
    if (!m_hostsTable || row < 0 || row >= m_hostsTable->rowCount())
        return;
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    if (row >= hosts.size())
        return;
    const QJsonObject host = hosts.at(row).toObject();
    const QString name = host.value(QStringLiteral("name")).toString();
    const QString ip = host.value(QStringLiteral("ip")).toString();

    QString apiKey =
        m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString();
    if (apiKey.isEmpty()) {
        apiKey = forkmesh::control::vultrApiKeyFromVariables(
            ActionStore::variables());
    }
    if (apiKey.isEmpty()) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QStringLiteral(
                "Enter your Vultr API key above (or store it as a VULTR_API_KEY "
                "variable) to destroy a server."));
        return;
    }

    const auto reply = QMessageBox::question(
        this, QStringLiteral("Destroy server"),
        QString::fromUtf8(
            "This DELETES the Vultr server behind \"%1\" (%2). The instance and "
            "everything on it are gone permanently, billing stops, the host is "
            "removed from this list, and the git remotes that pushed to it are "
            "removed. This cannot be undone. Continue?")
            .arg(name, ip),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;
    destroyVultrHostNamed(name);
}

void MainWindow::destroyVultrHostNamed(
    const QString &name, std::function<void(bool ok, QString message)> onDone)
{
    const auto setStatus = [this, onDone](const QString &text, bool finished,
                                          bool ok) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(text);
        if (finished && onDone)
            onDone(ok, text);
    };
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QJsonObject host;
    for (const QJsonValue &value : hosts) {
        if (value.toObject().value(QStringLiteral("name")).toString() == name) {
            host = value.toObject();
            break;
        }
    }
    if (host.isEmpty()) {
        setStatus(QString::fromUtf8("No saved host is named \"%1\".").arg(name),
                  true, false);
        return;
    }
    const QString ip = host.value(QStringLiteral("ip")).toString();
    QString apiKey =
        m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString();
    if (apiKey.isEmpty()) {
        apiKey = forkmesh::control::vultrApiKeyFromVariables(
            ActionStore::variables());
    }
    if (apiKey.isEmpty()) {
        setStatus(QStringLiteral(
                      "Enter your Vultr API key above (or store it as a "
                      "VULTR_API_KEY variable) to destroy a server."),
                  true, false);
        return;
    }

    const QString recorded = forkmesh::control::savedHostVultrInstanceId(host);
    if (!recorded.isEmpty()) {
        sendVultrInstanceDestroy(apiKey, recorded, name, onDone);
        return;
    }
    setStatus(QString::fromUtf8("Looking up \"%1\" on Vultr\xE2\x80\xA6").arg(name),
              false, false);
    vultrApiCall(
        apiKey, QStringLiteral("/v2/instances?per_page=500"),
        QByteArrayLiteral("GET"), {},
        [this, apiKey, name, ip, setStatus, onDone](QJsonObject result,
                                                    QString error) {
            if (!error.isEmpty()) {
                setStatus(QString::fromUtf8("Could not destroy \"%1\": %2")
                              .arg(name, error),
                          true, false);
                return;
            }
            const QString instanceId =
                forkmesh::control::vultrInstanceIdForAddress(
                    result.value(QStringLiteral("instances")).toArray(), ip);
            if (instanceId.isEmpty()) {
                setStatus(QString::fromUtf8(
                    "No single Vultr instance matches \"%1\" (%2), so nothing "
                    "was destroyed. Delete it from the Vultr panel instead.")
                              .arg(name, ip),
                          true, false);
                return;
            }
            sendVultrInstanceDestroy(apiKey, instanceId, name, onDone);
        });
}

void MainWindow::sendVultrInstanceDestroy(
    const QString &apiKey, const QString &instanceId, const QString &name,
    std::function<void(bool ok, QString message)> onDone)
{
    const auto setStatus = [this, onDone](const QString &text, bool finished,
                                          bool ok) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(text);
        if (finished && onDone)
            onDone(ok, text);
    };
    const QString invalid =
        forkmesh::control::validateVultrDestroyRequest(apiKey, instanceId);
    if (!invalid.isEmpty()) {
        setStatus(invalid, true, false);
        return;
    }
    setStatus(QString::fromUtf8("Destroying \"%1\" on Vultr\xE2\x80\xA6").arg(name),
              false, false);
    appendHostInstallLog(
        QString::fromUtf8("Destroying Vultr instance %1 (\"%2\")\xE2\x80\xA6\n")
            .arg(instanceId, name));
    vultrApiCall(
        apiKey, QStringLiteral("/v2/instances/") + instanceId,
        QByteArrayLiteral("DELETE"), {},
        [this, name, instanceId, setStatus](QJsonObject, QString error) {
            if (!error.isEmpty()) {
                appendHostInstallLog(
                    QString::fromUtf8("Vultr destroy failed: %1\n").arg(error));
                setStatus(QString::fromUtf8("Could not destroy \"%1\": %2")
                              .arg(name, error),
                          true, false);
                return;
            }
            appendHostInstallLog(QString::fromUtf8(
                "Vultr instance %1 destroyed.\n").arg(instanceId));
            // Dropping it from the Hosts list is not enough: the unattended
            // SSH fan-out takes its targets from `git remote -v`, so a remote
            // left pointing at the destroyed server keeps costing a connect
            // timeout on every safety pass. Unwire it first — the saved host
            // is what maps this node to the address a remote may name it by.
            const int unwired = removeSshMirrorRemotesForNode(name);
            if (unwired > 0) {
                appendHostInstallLog(
                    QStringLiteral("Removed %1 git remote%2 that pointed at "
                                   "\"%3\".\n")
                        .arg(unwired)
                        .arg(unwired == 1 ? QString() : QStringLiteral("s"),
                             name));
            }
            QSettings settings;
            const QJsonArray hosts = forkmesh::control::loadSavedHosts(
                settings, kHostsSetting, &m_hostSessionPasswords);
            for (int i = 0; i < hosts.size(); ++i) {
                if (hosts.at(i).toObject().value(QStringLiteral("name"))
                        .toString() == name) {
                    forgetHostAtRow(i);
                    break;
                }
            }
            setStatus(QString::fromUtf8(
                          "Destroyed \"%1\" on Vultr and removed it from this list.")
                          .arg(name),
                      true, true);
        });
}

QString MainWindow::savedHostIdentityFile(const QString &name, const QString &ip,
                                          const QString &user) const
{
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, nullptr);
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        if (host.value(QStringLiteral("name")).toString() != name ||
            host.value(QStringLiteral("ip")).toString() != ip ||
            host.value(QStringLiteral("user")).toString() != user) {
            continue;
        }
        const QString identity =
            host.value(QStringLiteral("identityFile")).toString().trimmed();
        if (!identity.isEmpty())
            return identity;
        break;
    }
    return forkmesh::control::existingSharedHostIdentityFile();
}

namespace {

QNetworkRequest runnerApiRequest(const QUrl &url, const QString &token)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    request.setRawHeader("Authorization",
                         QByteArrayLiteral("Bearer ") + token.toUtf8());
    return request;
}

int replyHttpStatus(QNetworkReply *reply)
{
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

} // namespace

void MainWindow::refreshRunners(std::function<void()> then)
{
    if (then)
        m_runnersFetchWaiters.append(std::move(then));
    const auto finish = [this] {
        const QList<std::function<void()>> waiters =
            std::exchange(m_runnersFetchWaiters, {});
        for (const std::function<void()> &waiter : waiters)
            waiter();
    };
    if (m_runnersFetchInFlight)
        return;
    const QUrl url = forkmesh::runners::runnersListUrl(catalogApiUrl());
    const QString token = accountSessionTokenForUrl(url);
    if (!m_networkAccess || token.isEmpty()) {
        if (m_runnersStatus)
            m_runnersStatus->setText(QStringLiteral(
                "Sign in to your ForkMesh account on this desktop (Account "
                "menu) to see and sign in the agent runners."));
        finish();
        return;
    }
    m_runnersFetchInFlight = true;
    if (m_runnersStatus && !m_runnersLoaded)
        m_runnersStatus->setText(
            QString::fromUtf8("Loading runners\xE2\x80\xA6"));
    QNetworkReply *reply = m_networkAccess->get(runnerApiRequest(url, token));
    connect(reply, &QNetworkReply::finished, this, [this, reply, finish] {
        reply->deleteLater();
        m_runnersFetchInFlight = false;
        const int status = replyHttpStatus(reply);
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (status != 200 || !payload.value(QStringLiteral("ok")).toBool()) {
            if (m_runnersStatus) {
                const QString message =
                    payload.value(QStringLiteral("message")).toString();
                const QString detail =
                    !message.isEmpty() ? message
                    : status > 0       ? QStringLiteral("HTTP %1").arg(status)
                                       : reply->errorString();
                m_runnersStatus->setText(
                    status == 401
                        ? QStringLiteral("The account session was refused; "
                                         "sign in again to manage runners.")
                        : QStringLiteral("Could not load runners: %1")
                              .arg(detail));
            }
            finish();
            return;
        }
        m_runners = forkmesh::runners::parseRunners(payload);
        m_runnersLoaded = true;
        renderRunnersTable();
        if (QDateTime::currentMSecsSinceEpoch() <
            m_runnerAttentionEndpointMissingUntilMs)
            setRunnerAttention(
                forkmesh::runners::runnerAttentionFromRunners(m_runners));
        else
            refreshRunnerAttention(true);
        bool pending = false;
        for (const forkmesh::runners::RunnerInfo &runner :
             std::as_const(m_runners))
            for (const forkmesh::runners::RunnerAccountRequestInfo &request :
                 runner.accountRequests)
                pending = pending || request.state == QLatin1String("pending");
        if (pending && m_runnersPendingRefreshes > 0) {
            --m_runnersPendingRefreshes;
            QTimer::singleShot(2000, this, [this] { refreshRunners(); });
        }
        if (m_nodesTable && m_nodesTable->isVisible())
            refreshNodesTable();
        finish();
    });
}

void MainWindow::requestRunnerAccountChange(
    const QString &runner, const QString &provider,
    const forkmesh::runners::RunnerAccountInfo &account,
    const QString &action)
{
    const QString label = forkmesh::runners::runnerProviderLabel(provider);
    if (action == QLatin1String("remove") &&
        QMessageBox::question(
            this, QStringLiteral("Remove %1 account").arg(label),
            QStringLiteral("Remove %1 from %2? The runner deletes its saved "
                           "login; sign it in again to use it later.")
                .arg(account.displayName(), runner),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No) != QMessageBox::Yes)
        return;
    const QUrl url =
        forkmesh::runners::runnerAccountsUrl(catalogApiUrl(), runner);
    const QString token = accountSessionTokenForUrl(url);
    if (!m_networkAccess || token.isEmpty()) {
        const QString message = QStringLiteral(
            "Sign in to your ForkMesh account on this desktop to manage "
            "runner accounts.");
        if (m_runnersStatus)
            m_runnersStatus->setText(message);
        if (m_runnersStatus && m_runnersStatus->isVisible())
            fileMessage(message, true);
        else
            flashMessage(message, true);
        return;
    }
    QNetworkReply *reply = m_networkAccess->post(
        runnerApiRequest(url, token),
        forkmesh::runners::runnerAccountRequestBody(provider, account.id,
                                                    action));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, action, account, runner] {
                const int status = replyHttpStatus(reply);
                const QJsonObject payload =
                    QJsonDocument::fromJson(reply->readAll()).object();
                const QString message =
                    status == 202
                        ? (action == QLatin1String("remove")
                               ? QStringLiteral("Removing %1 from %2; the "
                                                "runner applies it on its "
                                                "next check-in.")
                               : QStringLiteral("Switching %2 to %1; the "
                                                "runner applies it on its "
                                                "next check-in."))
                              .arg(account.displayName(), runner)
                        : QStringLiteral("Could not change the runner "
                                         "account: %1")
                              .arg(payload.value(QStringLiteral("message"))
                                       .toString(
                                           status > 0
                                               ? QStringLiteral("HTTP %1")
                                                     .arg(status)
                                               : reply->errorString()));
                if (m_runnersStatus)
                    m_runnersStatus->setText(message);
                if (m_runnersStatus && m_runnersStatus->isVisible())
                    fileMessage(message, status != 202);
                else
                    flashMessage(message, status != 202);
                m_runnersPendingRefreshes = 30;
                refreshRunners();
            });
}

void MainWindow::fillAgentRunnerAccountsMenu(QMenu *menu,
                                             const QString &provider)
{
    const QUrl url = forkmesh::runners::runnersListUrl(catalogApiUrl());
    const bool signedIn =
        m_networkAccess && !accountSessionTokenForUrl(url).isEmpty();
    const QString emptyText =
        !signedIn ? QStringLiteral("Sign in to your ForkMesh account to "
                                   "manage runner accounts")
        : !m_runnersLoaded
            ? QString::fromUtf8("Loading runners\xE2\x80\xA6")
            : QStringLiteral("No runner has %1 installed")
                  .arg(forkmesh::runners::runnerProviderLabel(provider));
    forkmesh::runners::fillRunnerAccountsMenu(
        menu, m_runners, provider, QDateTime::currentMSecsSinceEpoch(),
        emptyText,
        [this](const QString &runner, const QString &runnerProvider) {
            openRunnerAgentLoginTerminal(runner, runnerProvider);
        },
        [this](const QString &runner, const QString &runnerProvider,
               const forkmesh::runners::RunnerAccountInfo &account,
               const QString &action) {
            requestRunnerAccountChange(runner, runnerProvider, account,
                                       action);
        });
}

void MainWindow::refreshAgentRunnerAccountMenus()
{
    for (const auto &[menu, provider] :
         {std::pair<QMenu *, QString>{m_claudeAccountUsageMenu.data(),
                                      kClaudeCodeProvider},
          std::pair<QMenu *, QString>{m_codexAccountUsageMenu.data(),
                                      kCodexProvider}}) {
        if (!menu)
            continue;
        if (auto *runnerMenu = menu->findChild<QMenu *>(
                QStringLiteral("agentRunnerAccountsMenu"),
                Qt::FindDirectChildrenOnly))
            fillAgentRunnerAccountsMenu(runnerMenu, provider);
    }
}

void MainWindow::renderRunnersTable()
{
    refreshAgentRunnerAccountMenus();
    if (!m_runnersTable)
        return;
    forkmesh::runners::fillRunnersTable(
        m_runnersTable, m_runners, QDateTime::currentMSecsSinceEpoch(),
        [this](const QString &runner, const QString &provider) {
            openRunnerAgentLoginTerminal(runner, provider);
        },
        [this](const QString &runner, const QString &provider,
               const forkmesh::runners::RunnerAccountInfo &account,
               const QString &action) {
            requestRunnerAccountChange(runner, provider, account, action);
        });
    if (!m_runnersStatus)
        return;
    int needSignIn = 0;
    for (const forkmesh::runners::RunnerInfo &runner : std::as_const(m_runners))
        for (const forkmesh::runners::RunnerProviderState &state :
             runner.providers)
            if (state.reported && state.binaryFound && !state.signedIn())
                ++needSignIn;
    if (m_runners.isEmpty())
        m_runnersStatus->setText(QStringLiteral(
            "No runners are visible to this account. Pool runners are listed "
            "for app admins; a personal runner for the account that "
            "registered it."));
    else if (needSignIn > 0)
        m_runnersStatus->setText(
            QStringLiteral("%1 runner CLI sign-in(s) needed before queued "
                           "cloud agents can run there. Click Sign in on the "
                           "runner's row.")
                .arg(needSignIn));
    else
        m_runnersStatus->setText(
            QStringLiteral("%1 runner(s); every installed agent CLI is "
                           "signed in.")
                .arg(m_runners.size()));
}

void MainWindow::refreshRunnerAttention(bool force)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool useEndpoint = now >= m_runnerAttentionEndpointMissingUntilMs;
    const QUrl base = catalogApiUrl();
    const QUrl url = useEndpoint
                         ? forkmesh::runners::runnerAttentionUrl(base)
                         : forkmesh::runners::runnersListUrl(base);
    const QString token = accountSessionTokenForUrl(url);
    if (!m_networkAccess || token.isEmpty()) {
        setRunnerAttention({});
        return;
    }
    if (m_runnerAttentionInFlight)
        return;
    const qint64 floorMs = force ? 5000 : m_runnerAttentionPollMs;
    if (m_runnerAttentionFetchedAtMs > 0 &&
        now - m_runnerAttentionFetchedAtMs < floorMs)
        return;
    m_runnerAttentionInFlight = true;
    m_runnerAttentionFetchedAtMs = now;
    QNetworkReply *reply = m_networkAccess->get(runnerApiRequest(url, token));
    connect(reply, &QNetworkReply::finished, this, [this, reply, useEndpoint] {
        reply->deleteLater();
        m_runnerAttentionInFlight = false;
        const int status = replyHttpStatus(reply);
        if (useEndpoint && (status == 404 || status == 405)) {
            m_runnerAttentionEndpointMissingUntilMs =
                QDateTime::currentMSecsSinceEpoch() + 30 * 60 * 1000;
            m_runnerAttentionFetchedAtMs = 0;
            refreshRunnerAttention(true);
            return;
        }
        if (status == 401 || status == 403) {
            setRunnerAttention({});
            return;
        }
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        if (status != 200 || !payload.value(QStringLiteral("ok")).toBool())
            return;
        if (useEndpoint) {
            m_runnerAttentionPollMs =
                forkmesh::runners::runnerAttentionPollMs(payload);
            setRunnerAttention(
                forkmesh::runners::parseRunnerAttention(payload));
            return;
        }
        m_runners = forkmesh::runners::parseRunners(payload);
        m_runnersLoaded = true;
        renderRunnersTable();
        setRunnerAttention(
            forkmesh::runners::runnerAttentionFromRunners(m_runners));
    });
}

void MainWindow::setRunnerAttention(
    const QList<forkmesh::runners::RunnerAttentionItem> &items)
{
    QStringList before;
    for (const forkmesh::runners::RunnerAttentionItem &item :
         std::as_const(m_runnerAttention))
        before.append(item.runner + QLatin1Char('/') + item.provider +
                      QLatin1Char('/') + item.state);
    m_runnerAttention = forkmesh::runners::sortedRunnerAttention(items);
    for (const forkmesh::runners::RunnerAttentionItem &item :
         std::as_const(m_runnerAttention)) {
        const QString key = item.runner + QLatin1Char('/') + item.provider +
                            QLatin1Char('/') + item.state;
        if (!before.contains(key))
            logSystem(forkmesh::runners::runnerAttentionDetail(item));
    }
    if (!before.isEmpty() && m_runnerAttention.isEmpty())
        logSystem(QStringLiteral("Every runner login you manage is signed in "
                                 "again."));
    if (!m_runnerAttentionButton)
        return;
    const forkmesh::runners::RunnerAttentionBadge badge =
        forkmesh::runners::runnerAttentionBadge(m_runnerAttention);
    m_runnerAttentionButton->setText(badge.text);
    m_runnerAttentionButton->setToolTip(badge.toolTip);
    m_runnerAttentionButton->setAccessibleName(badge.text);
    const QString severity =
        badge.expired ? QStringLiteral("expired") : QStringLiteral("expiring");
    if (m_runnerAttentionButton->property("severity").toString() != severity) {
        m_runnerAttentionButton->setProperty("severity", severity);
        m_runnerAttentionButton->style()->unpolish(m_runnerAttentionButton);
        m_runnerAttentionButton->style()->polish(m_runnerAttentionButton);
    }
    m_runnerAttentionButton->setVisible(badge.visible);
}

void MainWindow::openRunnerAttention()
{
    if (m_runnerAttention.isEmpty()) {
        showNetworkTab(kNetworkHostsTab);
        return;
    }
    if (m_runnerAttention.size() == 1) {
        const forkmesh::runners::RunnerAttentionItem item =
            m_runnerAttention.first();
        if (item.canConnect)
            openRunnerAgentLoginTerminal(item.runner, item.provider);
        else
            QDesktopServices::openUrl(forkmesh::runners::runnerConnectPageUrl(
                catalogApiUrl(), item));
        return;
    }
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("runnerAttentionMenu"));
    for (const forkmesh::runners::RunnerAttentionItem &item :
         std::as_const(m_runnerAttention)) {
        QAction *action = menu.addAction(
            QString::fromUtf8("%1 \xE2\x80\x94 %2")
                .arg(forkmesh::runners::runnerAttentionTitle(item),
                     forkmesh::runners::runnerAttentionActionText(item)));
        action->setToolTip(forkmesh::runners::runnerAttentionDetail(item));
        connect(action, &QAction::triggered, this, [this, item] {
            if (item.canConnect)
                openRunnerAgentLoginTerminal(item.runner, item.provider);
            else
                QDesktopServices::openUrl(
                    forkmesh::runners::runnerConnectPageUrl(catalogApiUrl(),
                                                            item));
        });
    }
    menu.addSeparator();
    connect(menu.addAction(QStringLiteral("Open the Runners list")),
            &QAction::triggered, this,
            [this] { showNetworkTab(kNetworkHostsTab); });
    const QWidget *anchor = m_runnerAttentionButton;
    menu.exec(anchor ? anchor->mapToGlobal(QPoint(0, anchor->height()))
                     : QCursor::pos());
}

bool MainWindow::openRunnerSignInLink(const QUrl &url)
{
    QString runner;
    QString provider;
    if (!forkmesh::runners::runnerSignInLink(url, &runner, &provider))
        return false;
    if (!runner.isEmpty()) {
        openRunnerAgentLoginTerminal(runner, provider);
        return true;
    }
    forkmesh::agents::CloudAgentSession cloud;
    cloud.provider = provider;
    withRunnerForCloudAgent(cloud, [this, provider](const QString &picked) {
        openRunnerAgentLoginTerminal(picked, provider);
    });
    return true;
}

void MainWindow::withRunnerForCloudAgent(
    const forkmesh::agents::CloudAgentSession &cloud,
    std::function<void(const QString &)> then)
{
    const auto resolve = [this, cloud, then] {
        const QString runner = forkmesh::runners::runnerForTarget(
            m_runners, cloud.targetNode, cloud.provider);
        if (runner.isEmpty()) {
            showNetworkTab(kNetworkHostsTab);
            const QString message =
                m_runners.isEmpty()
                    ? QStringLiteral("No runner is visible to this account. "
                                     "Check the Runners list on the Hosts "
                                     "page.")
                    : QStringLiteral("Pick the runner to sign in from the "
                                     "Runners list on the Hosts page.");
            if (m_runnersStatus && m_runnersStatus->isVisible()) {
                m_runnersStatus->setText(message);
                fileMessage(message, m_runners.isEmpty());
            } else {
                flashMessage(message, m_runners.isEmpty());
            }
            return;
        }
        then(runner);
    };
    if (m_runnersLoaded)
        resolve();
    else
        refreshRunners(resolve);
}

void MainWindow::openRunnerAgentLoginTerminal(const QString &runnerName,
                                              const QString &providerName,
                                              std::function<void()> onSignedIn)
{
    using forkmesh::runners::RunnerLoginSession;
    const QString runner = runnerName.trimmed().toLower();
    const QString provider = providerName.trimmed().toLower();
    if (runner.isEmpty() ||
        !forkmesh::runners::runnerProviders().contains(provider))
        return;
    const QString key = runner + QLatin1Char('/') + provider;
    if (QDialog *existing = m_runnerLoginDialogs.value(key)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    const QUrl base = catalogApiUrl();
    if (!m_networkAccess ||
        accountSessionTokenForUrl(
            forkmesh::runners::runnerLoginStartUrl(base, runner))
            .isEmpty()) {
        flashMessage(QStringLiteral("Sign in to your ForkMesh account on this "
                                    "desktop first; runner sign-ins use the "
                                    "account session."),
                     true);
        return;
    }
    const QString label = forkmesh::runners::runnerProviderLabel(provider);
    const QString command = provider == QLatin1String("codex")
                                ? QStringLiteral("codex login --device-auth")
                                : QStringLiteral("claude auth login");
    const AgentLoginDialogParts parts = buildAgentLoginDialog(
        this,
        QString::fromUtf8("Sign in %1 \xE2\x80\x94 %2").arg(label, runner),
        QStringLiteral(
            "The runner <b>%1</b> runs <code>%2</code> as its job user and "
            "relays the terminal here through the app. Finish the provider's "
            "browser flow; for Claude, paste the code it shows into the box "
            "below and press Enter. The credential is written on the runner "
            "and never passes through ForkMesh.")
            .arg(runner.toHtmlEscaped(), command.toHtmlEscaped()));
    QDialog *dialog = parts.dialog;
    dialog->setProperty("runner", runner);
    dialog->setProperty("provider", provider);
    m_runnerLoginDialogs.insert(key, dialog);
    TerminalWidget *terminal = parts.terminal;
    terminal->setRemoteInput(true);

    auto *status = new QLabel(dialog);
    status->setObjectName(QStringLiteral("runnerLoginStatus"));
    status->setWordWrap(true);
    status->setText(QString::fromUtf8("Starting the sign-in\xE2\x80\xA6"));
    parts.layout->insertWidget(1, status);

    auto *codeRow = new QHBoxLayout;
    auto *code = new QLineEdit(dialog);
    code->setObjectName(QStringLiteral("runnerLoginCodeInput"));
    code->setPlaceholderText(QStringLiteral(
        "Paste the code the browser shows, then press Enter"));
    code->setMaxLength(forkmesh::runners::kRunnerLoginMaxInputBytes - 1);
    auto *send = new QPushButton(QStringLiteral("Send"), dialog);
    send->setObjectName(QStringLiteral("runnerLoginSendButton"));
    codeRow->addWidget(code, 1);
    codeRow->addWidget(send);
    parts.layout->insertLayout(parts.layout->count() - 1, codeRow);

    auto *cancel = new QPushButton(QStringLiteral("Cancel sign-in"), dialog);
    cancel->setObjectName(QStringLiteral("runnerLoginCancelButton"));
    parts.buttons->insertWidget(1, cancel);

    auto session = std::make_shared<RunnerLoginSession>(runner, provider);
    auto pollInFlight = std::make_shared<bool>(false);
    auto signedInCalled = std::make_shared<bool>(false);
    auto *timer = new QTimer(dialog);
    timer->setInterval(forkmesh::runners::kRunnerLoginPollMs);
    QPointer<QDialog> guard(dialog);

    const auto request = [this](const QUrl &url) {
        return runnerApiRequest(url, accountSessionTokenForUrl(url));
    };
    const auto showState = [session, status, code, send, cancel] {
        QString text =
            forkmesh::runners::runnerLoginStateText(session->state());
        if (!session->error().isEmpty())
            text = session->error();
        else if (!session->message().isEmpty())
            text += QStringLiteral(": ") + session->message();
        status->setText(text);
        const bool open = session->open();
        code->setEnabled(open);
        send->setEnabled(open);
        cancel->setEnabled(open);
    };
    const auto ended = [this, session, timer, signedInCalled, onSignedIn] {
        timer->stop();
        refreshRunners();
        for (const int delayMs : {6000, 20000})
            QTimer::singleShot(delayMs, this, [this] {
                m_runnerAttentionFetchedAtMs = 0;
                refreshRunnerAttention(true);
            });
        if (session->state() == QLatin1String("succeeded") &&
            !*signedInCalled) {
            *signedInCalled = true;
            if (onSignedIn)
                QTimer::singleShot(0, this, onSignedIn);
        }
    };
    auto poll = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakPoll = poll;
    *poll = [this, base, session, pollInFlight, terminal, guard, request,
             showState, ended, weakPoll] {
        if (!guard || !session->open() || *pollInFlight)
            return;
        *pollInFlight = true;
        QNetworkReply *reply = m_networkAccess->get(request(
            forkmesh::runners::runnerLoginOutputUrl(
                base, session->runner(), session->id(), session->afterSeq())));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        connect(reply, &QNetworkReply::finished, guard.data(),
                [reply, session, pollInFlight, terminal, showState, ended,
                 weakPoll] {
                    *pollInFlight = false;
                    const int http = replyHttpStatus(reply);
                    const QJsonObject body =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    if (http == 404 || http == 403 || http == 401) {
                        session->markFailed(
                            http == 404
                                ? QStringLiteral("The sign-in session is gone.")
                                : QStringLiteral("The app refused to show "
                                                 "this sign-in (HTTP %1).")
                                      .arg(http));
                        showState();
                        ended();
                        return;
                    }
                    if (http != 200)
                        return;
                    terminal->feed(session->applyOutputReply(body));
                    showState();
                    if (!session->open()) {
                        ended();
                        return;
                    }
                    if (session->more())
                        if (const auto next = weakPoll.lock())
                            QTimer::singleShot(0, terminal, *next);
                });
    };
    connect(timer, &QTimer::timeout, dialog, [poll] { (*poll)(); });

    const auto sendInput = [this, base, session, status, request,
                            guard](const QString &data) {
        if (!session->open() || data.isEmpty())
            return;
        if (!forkmesh::runners::runnerLoginInputValid(data)) {
            status->setText(QStringLiteral(
                "Only printable text (up to 4096 bytes) can be sent to the "
                "runner; arrow keys and other control keys are not relayed."));
            return;
        }
        QNetworkReply *reply = m_networkAccess->post(
            request(forkmesh::runners::runnerLoginInputUrl(
                base, session->runner(), session->id())),
            forkmesh::runners::runnerLoginInputBody(data));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        connect(reply, &QNetworkReply::finished, guard.data(),
                [reply, status] {
                    if (replyHttpStatus(reply) == 200)
                        return;
                    const QJsonObject body =
                        QJsonDocument::fromJson(reply->readAll()).object();
                    QString message =
                        body.value(QStringLiteral("message")).toString();
                    if (message.isEmpty())
                        message = body.value(QStringLiteral("error")).toString();
                    status->setText(
                        QStringLiteral("The runner did not take that input: %1")
                            .arg(message));
                });
    };
    const auto submitCode = [code, sendInput] {
        const QString data =
            forkmesh::runners::runnerLoginCodeSubmission(code->text());
        code->clear();
        sendInput(data);
    };
    connect(code, &QLineEdit::returnPressed, dialog, submitCode);
    connect(send, &QPushButton::clicked, dialog, submitCode);
    connect(terminal, &TerminalWidget::inputTyped, dialog,
            [code, sendInput](const QByteArray &bytes) {
                QStringList submitted;
                code->setText(forkmesh::runners::applyRunnerLoginKeystrokes(
                    code->text(), bytes, &submitted));
                for (const QString &line : std::as_const(submitted))
                    sendInput(line);
            });

    const auto postCancel = [this, base, session,
                             request]() -> QNetworkReply * {
        if (!session->needsCancelOnClose() || !m_networkAccess)
            return nullptr;
        QNetworkReply *reply = m_networkAccess->post(
            request(forkmesh::runners::runnerLoginCancelUrl(
                base, session->runner(), session->id())),
            QByteArrayLiteral("{}"));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        return reply;
    };
    connect(cancel, &QPushButton::clicked, dialog,
            [postCancel, guard, session, showState, ended] {
                QNetworkReply *reply = postCancel();
                if (!reply)
                    return;
                connect(reply, &QNetworkReply::finished, guard.data(),
                        [reply, session, showState, ended] {
                            session->applyCancelReply(
                                QJsonDocument::fromJson(reply->readAll())
                                    .object());
                            showState();
                            ended();
                        });
            });
    connect(dialog, &QObject::destroyed, this, [this, key, postCancel] {
        m_runnerLoginDialogs.remove(key);
        postCancel();
        refreshRunners();
    });

    QString openId;
    for (const forkmesh::runners::RunnerInfo &info : std::as_const(m_runners))
        if (info.name == runner)
            openId = info.openLoginId(provider);

    dialog->show();
    dialog->raise();
    code->setFocus();
    if (!openId.isEmpty()) {
        session->attach(openId);
        showState();
        timer->start();
        (*poll)();
        return;
    }
    QNetworkReply *reply = m_networkAccess->post(
        request(forkmesh::runners::runnerLoginStartUrl(base, runner)),
        forkmesh::runners::runnerLoginStartBody(provider));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    connect(reply, &QNetworkReply::finished, this,
            [reply, session, showState, timer, poll, guard, postCancel] {
                session->applyStartReply(
                    replyHttpStatus(reply),
                    QJsonDocument::fromJson(reply->readAll()).object());
                if (!guard) {
                    postCancel();
                    return;
                }
                showState();
                if (!session->open())
                    return;
                timer->start();
                (*poll)();
            });
}
