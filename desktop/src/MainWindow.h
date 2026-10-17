#ifndef FORKMESH_MAIN_WINDOW_H
#define FORKMESH_MAIN_WINDOW_H

#include "BackoffNetworkAccessManager.h" // FirewallVerdict, in a signature below
#include "ChatBackend.h"
#include "ChatListPreview.h"
#include "ChannelGroups.h"
#include "RailFolders.h"
#include "RailGroups.h"
#include "AgentFolders.h"
#include "AgentCliTasks.h"
#include "RailShortcuts.h"
#include "MessageRow.h" // ThreadReplySummary is returned by value below
#include "ClientErrorReports.h"
#include "SystemStats.h" // DescendantProcess, in a signature below
// forkmesh::code — the symbol heuristics behind the Files editor's trace panel.
#include "CodeSymbols.h"
#include "PingFixes.h"
#include "PingSyncState.h"
// FORKMESH_LOG_SOURCE_FILE/LINE: logSystem()'s default arguments, which expand
// at each call site so every entry records where it was logged from.
#include "CoreNodeLogs.h"
#include "PlannedRestarts.h"
#include "IpCountry.h"
#include "IpWhois.h"
#include "NodeRole.h"
#include "NodeDirectoryRows.h"
#include "D1QuerySource.h"
#include "LogSource.h"
// BackgroundTicket / BackgroundRoutine: the footer's background-activity
// bookkeeping below is keyed on what a ticket says about itself.
#include "BackgroundActivity.h"
#include "LogTimelineChart.h"  // LogTimelineBucket is returned by value below
#include "NetworkLogArchive.h" // netlog::Digest is held by value below
#include "CrashLogScan.h"      // crashlog::Record is returned by value below
#include "DiscussionInboxBackoff.h"
#include "NetworkBackoff.h"
#include "DiscussionStore.h"
#include "ForkMeshIdentity.h"
#include "GenieVitals.h"
#include "IssueStore.h"
#include "WebBranchOps.h"
#include "IssueModelMention.h"
#include "ProjectStore.h"
#include "PullStore.h"
#include "PullReviewModel.h"
#include "MergeQueue.h"
#include "CoveStore.h"
#include "ActionStore.h"
#include "ActionFile.h"
#include "AccountCapability.h"
#include "AccountSessionCache.h"
#include "AgentStore.h"
#include "TaskWorkItems.h"
#include "ListViewMode.h"
#include "AgentCloudSessions.h"
#include "AgentCloudStream.h"
#include "RunnerLogin.h"
#include "AppViewMode.h"
#include "StatsDashboard.h"
#include "CodeHealth.h" // codehealth::Report is held by value below
#include "RepoStatsStore.h" // RepoRatchetReading is held by value below
#include "AgentRunEfficiencyLog.h"
#include "AgentToolEfficiencyLedger.h"
#include "AgentGuardrailLedger.h"
#include "AgentToolImprovementLog.h"
#include "AgentRunner.h"
#include "AgentNetworkGate.h" // the gate server is held by value below
#include "AgentRunRules.h"
#include "AgentIntake.h"
#include "AgentWindows.h"
#include "ClaudeTranscriptTools.h"
#include "TeammateMood.h"
#include "TeammateStore.h"
#include "TeammateWorkspace.h"
#include "SavedSearchStore.h"
#include "SecurityAlarmState.h"
#include "ClaudeSessionScan.h"
#include "MachineAgentScan.h"
#include "ClaudePeerMessaging.h"
#include "TerminalHandoff.h"
#include "ControlNode.h" // CompleteSystemDataEntry is held by value on the export page
#include "DirectorySizeScan.h"
#include "RepoSecurity.h"
#include "RepositoryCalendar.h" // RepositoryCalendarScan is held by value below
#include "MirrorCrypto.h"
#include "MirrorGatewayHealth.h"
#include "GuiPump.h"
#include "NativePackageExtension.h"
#include "RelayRepositoryScope.h"
#include "DesktopPackageExtension.h"
// forkmesh::ledger — the Ledger package's record model, held by value on
// the Ledger page below.
#include "LedgerFeature.h"
#include "LocalControlStream.h"
#include "LocalControlDiagnostics.h"
#include "LocalControlHosts.h" // host-control tokens and log streams held by value
#include "LocalControlPromptImages.h"
#include "LocalControlInsights.h"
#include "LocalControlRepos.h"
#include "LocalControlTerminals.h"
#include "LocalControlUpdate.h"
#include "DesktopCommands.h"
// forkmesh::timers — the Timers package's timer, countdown and alarm model,
// held by the top-bar clock below.
#include "TimersFeature.h"

namespace forkmesh::ui { class LogActivityLights; }
namespace forkmesh::ui { struct DiffHealthReport; }
namespace forkmesh::ui { class SecretDiffBanner; }
namespace forkmesh::ui { struct DiffFileMenuTarget; }
namespace forkmesh::ui { struct MirrorReleaseBlob; }
namespace forkmesh::ui { struct GitCaptureResult; }
namespace MarkdownRender { struct RepoLink; }
namespace forkmesh::ui { class ChannelGroupTree; }
namespace forkmesh::ui { class OnboardingTour; }
namespace forkmesh::ui { class WindowBackdrop; }
namespace forkmesh::ui { class WindowFade; }
namespace forkmesh::ui { class QtRuntimeDiagnostics; }
namespace forkmesh::ui::kit { class InlineNotice; }
namespace forkmesh { struct SystemPressureSnapshot; }
namespace forkmesh::agentresources {
struct GovernorSnapshot;
struct SliceCounters;
}
namespace forkmesh::ui { struct NextAccess; }
namespace forkmesh::ui { enum class ActionPingEvent; }
namespace forkmesh::pingforward { struct Ping; struct Source; }
namespace forkmesh::poweralert { struct Reading; }
namespace forkmesh::localcontrol::agentops { struct DeleteTrace; }
namespace forkmesh::capture { class CaptureFeature; }
namespace forkmesh::fleet { class AddCoreNodeController; class RemoveCoreNodeController; }
namespace forkmesh::deploylogs { class DeployLogFollower; }
namespace forkmesh::sonify { class AgentSonifier; enum class Motion; }
namespace forkmesh::announce {
class Commentator;
class Speaker;
class VoiceInstaller;
}
namespace forkmesh::memory { struct ProcessMemorySnapshot; }
namespace forkmesh::releasedraft { struct Cut; }
namespace forkmesh::market {
class PackageRegistry;
struct PackageManifest;
}

struct MarketPackageScore {
    double ratingAverage = 0.0;
    qint64 ratingCount = 0;
    qint64 people = 0;
    qint64 opens = 0;
    qint64 views = 0;

    bool operator==(const MarketPackageScore &other) const
    {
        return ratingAverage == other.ratingAverage &&
               ratingCount == other.ratingCount && people == other.people &&
               opens == other.opens && views == other.views;
    }
};

struct AgentScannerState {
    qint64 lastActivityMs = 0; // wall-clock of the last raw-output chunk
    double intensity = 0.0;    // 0..1 live-output rate the effect reacts to
};

// Where a running agent's fireball is in its orbit, carried forward frame by
// frame.  Deriving the angle from tick-count x speed instead made every change
// of speed teleport the head - at tick 900 a session that speeds up jumps
// thousands of degrees at once - so the position lives here and only its
// *rate* follows the live activity meter.
struct AgentSpinState {
    qreal phaseDeg = 0.0; // where the head is right now, 0..360
    double meter = 0.0;   // eased 0..1 activity the orbit speed follows
    bool seeded = false;  // per-session starting offset already applied
};

struct AgentDiffStat {
    int files = -1;
    int ahead = -1;
    int behind = -1;
    bool conflicted = false;
    QString base;
    QString worktree;
    int dirty = -1;
    int added = -1;
    int removed = -1;
};

inline bool sameAgentDiffStat(const AgentDiffStat &a, const AgentDiffStat &b)
{
    return a.files == b.files && a.ahead == b.ahead && a.behind == b.behind &&
           a.conflicted == b.conflicted && a.base == b.base &&
           a.worktree == b.worktree && a.dirty == b.dirty &&
           a.added == b.added && a.removed == b.removed;
}

// Whether a reload of the Agents list is allowed to go back to Git. Walking
// every session's worktree for its diff, ahead/behind and merged state costs
// seconds on a fleet of any size, and merely opening the page is no reason to
// pay it: FromCache repaints the values the last scan left behind. An explicit
// refresh — "Update all", or an action that changed a branch — asks for Rescan.
enum class AgentReloadScan { FromCache, Rescan };

struct MirrorSelfSnapshot {
    QString key;
    MirrorAdvert advert;
    QString servedCommit; // tip of the branch we actually serve
    int pendingPush = 0;  // local commits not yet in the served mirror
    qint64 gatheredMs = 0;
};

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHash>
#include <QSharedPointer>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMainWindow>
#include <QMap>
#include <QMetaType>
#include <QPair>
#include <QPixmap>
#include <QPointer>
#include <QRect>
#include <QRegularExpression>
#include <QQueue>
#include <QSet>
#include <QTextBlockUserData>
#include <QTextCursor>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <atomic>
#include <functional>
#include <limits>
#include <map>
#include <memory>

class FooterLogLineData : public QTextBlockUserData {
public:
    explicit FooterLogLineData(QString line) : rawLine(std::move(line)) {}
    QString rawLine;
};

class MessageRow;
class MarkdownEditor;
class PullBadgeWidget;
namespace forkmesh::ui {
class ActivityRailButton;
class RailEventHook;
class RailFolderWidget;
class RailGroupSeparator;
class AgentDotMatrix;
class BusySpinner;
class NodeDotMatrix;
class RelaySpeedDot;
class ActionRunStrip;
class BackgroundTaskChip;
class ElidingStatusLabel;
class VerticalIconButton;
}
using forkmesh::ui::ActionRunStrip;
using forkmesh::ui::ActivityRailButton;
using forkmesh::ui::RailEventHook;
using forkmesh::ui::RailFolderWidget;
using forkmesh::ui::RailGroupSeparator;
using forkmesh::ui::VerticalIconButton;
using forkmesh::ui::AgentDotMatrix;
using forkmesh::ui::BusySpinner;
using forkmesh::ui::ElidingStatusLabel;
using forkmesh::ui::NodeDotMatrix;
using forkmesh::ui::RelaySpeedDot;
class TerminalWidget;
class ClaudeIdeBridge;
class ClaudeStreamSession;
class CodexAppServerSession;
class AgentCapabilityBroker;
class AgentPermissionController;
class StallWatchdog;
class ClaudeTranscriptView;
class AgentLoginPty;
class NodeEventSocket;
class WorldSpeechBridge;
class OfficeChannelMirror;
class LocalMeshNode;
class LocalControlServer;
class DiscordClient;
class PrivateMirrorMaterialization;
class ActionRunner;
class LogTimelineChart;
class SkillUsageChart;
class AgentToolEfficiencyBoard;
class AgentModelTable;
class GenieSpiderWidget;
class GenieCoachPanel;
class GenieTipBubble;
class GenieDesktopStage;
struct AgentModelGitHistory;
class AgentToolUsageTable;
class AgentRunTable;
class AgentGuardrailBoard;
class SettingsSectionsView;
class PingSettingsPanel;
class QTabBar;
namespace forkmesh {
class DatabaseManagerWidget;
class DesktopProcessView;
class FleetVisualizer;
class QueryInspector;
namespace desktopprocs { struct Process; }
}
class QButtonGroup;
class QGridLayout;
class QFileSystemWatcher;
class QSplitter;
class QTextEdit;
class QCheckBox;
class QComboBox;
class QCompleter;
class QAbstractItemView;
class QDateEdit;
class QDateTimeEdit;
class QDialog;
class QStringListModel;
class QGraphicsOpacityEffect;
class QFrame;
class QLabel;
class QMouseEvent;
class QContextMenuEvent;
class QAbstractButton;
class QAction;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMessageBox;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QPlainTextEdit;
class QImage;
class ComponentHealthBoard;
class HomeBoard;
class HomePageDots;
struct HomeGroup;
class BinarySizeMap;
class MemoryUsageChart;
class QProgressBar;
class QPropertyAnimation;
class QPushButton;
class QToolButton;
class QScrollArea;
class QSpinBox;
class QStackedWidget;
class QSystemTrayIcon;
class QTableWidget;
class QTableWidgetItem;
class QTabWidget;
class QTextBrowser;
class QTextEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;
class QProcess;
class QTemporaryDir;
class QVBoxLayout;
class QBoxLayout;
class HeaderWrapLayout;
class QCheckBox;
class QHBoxLayout;
class PublicMirrorMaterialization;
namespace forkmesh::control {
struct MirrorActionsConfigurationRequest;
struct AgentCliCredentials;
struct CloudflareBootstrapCommand;
struct CompleteSystemDataEntry;
}
namespace forkmesh::ui {
class DiffFileNavigator;  // file-list <-> diff-view sync
struct DiffFileEntry;     // one changed file parsed out of a patch
struct BranchDiffRender;
}
namespace ContributorPayoutReceipt {
struct Pending;           // a signed reward payout still awaiting reconciliation
}

// One commit of the pull request currently on screen, in the form the
// Conversation feed needs. renderPullCommits() already walks base..head (or the
// signed mbox) for the Commits tab, so it records what it found and
// renderPullThread() reuses it: the conversation must not repeat that walk,
// which pumps the GUI event loop.
struct PullActivityCommit {
    QString sha;
    QString subject;
    QString author;
    QString when;         // absolute, already formatted for display
    qint64 committedSecs = 0;
    QString agentTrailer; // ForkMesh-Agent trailer, when the commit has one
};

struct ServerConfig {
    // Stable local identity for this configured relay. The URL may be edited;
    // account and UI preferences remain attached to this id.
    QString id;
    QString name;
    QString url;
    QString room;
    // The most recently used account is instance-scoped. A username may exist
    // independently on any number of relays.
    QString lastAccount;
};

struct RepoInfo {
    QString about;
    QString website;
    QString language; // optional primary-language override
    QStringList topics;
    int forks = 0;
    int stars = 0;
    int mirrors = 1;
    QString defaultBranch;
    QHash<QString, QString> contributorAvatars; // name -> image path/URL
};

struct RepositoryRecord {
    QString owner;
    QString name;
    QString description;
    QString cloneUrl;
    // Normalized HTTP(S) origin of the relay this local repository belongs to.
    // Empty on legacy records; those fall back to the default relay when the
    // repository switcher is scoped.
    QString relayUrl;
    QString localPath;
    QString solanaAddress;
    QString mirrorPath;
    // "owner/name" of the repository this one was forked from, when it was
    // created by Fork. The managed mirror service includes it in signed catalog
    // state so the upstream page can list forks; it is remembered locally because
    // bare mirror borrows the upstream mirror's object store (git alternates)
    // instead of duplicating its history.
    QString forkedFrom;
    QString privateReplicaId;
    // Legacy handle from releases that age-encrypted public repositories.
    // New public mirrors use the durable plaintext bare mirrorPath; this is
    // retained only until a successful sync removes the old archive.
    QString publicArchiveId;
    bool publishToNetwork = false;
    // Private repo: absent from public discovery. Authorized owners and
    // collaborators fetch only its opaque encrypted replica over HTTPS, then
    // decrypt into short-lived owner-only local storage with their own key.
    bool isPrivate = false;
    bool actionsEnabled = true;
    bool actionsAutoApprove = true;
    // Optional repository-relative filesystem allowlist inherited by every AI
    // agent process and subprocess launched for this repository.
    bool agentGuardrailsEnabled = false;
    QStringList agentAllowedDirectories;
    bool requirePeerApproval = true;
    bool mergeQueueEnabled = false;
    bool mergeQueuePaused = false;
    QStringList mergeQueue;
    bool externallyManagedActions = false;
    QString externalActionsSource;
    QString externalActionsRef;
    QStringList disabledWorkflows;
    QStringList workflowNodes;
    // Block pushes when the diff introduces a high-confidence secret (API key,
    // private key, etc.). Enabled by default; the user can bypass per-push or
    // turn it off entirely here.
    bool secretScanningEnabled = true;
    bool previewOnly = false;
    qint64 hostedSinceMs = 0;
    qint64 lastSyncMs = 0;
    // Private repo: hex SHA-256 of the sealed source's ref listing at the last
    // successful private sync. The periodic safety pass compares against it so
    // an unchanged private repository is not announced to the relay again;
    // every request on that path carries the owner/name in its URL.
    QString privateSyncRefsHash;
    // A source/local-mirror association that contradicted owner/name is never
    // fetched again until the evidence agrees.  Keep the exact reason so
    // periodic/event retries coalesce into one operator alert and a restart
    // cannot silently resume a quarantined record.
    bool mirrorSyncQuarantined = false;
    QString mirrorSyncQuarantineReason;
};

struct NotificationLink {
    QString kind;
    QString owner;   // repo owner
    QString name;    // repo name
    int number = -1; // issue / PR / discussion number
    QString ref;     // commit hash ("commit") or chat channel ("chat")

    bool isValid() const { return !kind.isEmpty(); }
};
Q_DECLARE_METATYPE(NotificationLink)

struct ComposerModelChoice {
    QString provider;  // "claude-code", "codex", "cloudflare-ai", "local-llama", …
    QString model;     // empty for the provider-only API agents
    QString label;     // bare model name shown in the row, e.g. "Opus 5"
    QString agentName; // which CLI/API runs it, for the tooltip
    QString tooltip;   // fixed tooltip; ranked rows build theirs from counts
    QIcon icon;        // built-in art or a package-provided avatar
    bool ranked = false;
};

struct GlobalSearchHit {
    QString kind;       // see kSearchCategories in MainWindowSearch.cpp
    int number = 0;     // issue/PR/discussion/project/agent/run id, or repo index
    QString path;       // file path (absolute for worktree code), branch, worktree branch, commit/tag ref
    int line = 0;       // code hit: 1-based line number
    int repoIndex = -1; // repository the hit belongs to (-1 = the open one)
    QString primary;    // main row text
    QString detail;     // dimmer context (where it matched)
    QString status;
};

// A repository whose branches, worktrees and tags search reads. The git dir is
// resolved on the worker (localPath, else mirrorPath) so no disk probe runs on
// the GUI thread.
struct GlobalSearchGitRepo {
    int index = -1;    // m_repositories index
    QString label;     // owner/name
    QString localPath;
    QString mirrorPath;
};

namespace forkmesh::restarttiming {
struct RestartRun;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT
    friend struct forkmesh::ui::NextAccess;
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    static constexpr int kBranchesDeleteColumn = 0;
    static constexpr int kBranchesLinksColumn = 1;
    static constexpr int kBranchesNameColumn = 2;
    static constexpr int kBranchesStatusColumn = 3;
    static constexpr int kBranchesBehindColumn = 4;
    static constexpr int kBranchesAheadColumn = 5;
    static constexpr int kBranchesCommitColumn = 6;
    static constexpr int kBranchesMessageColumn = 7;
    static constexpr int kBranchesAuthorColumn = 8;
    static constexpr int kBranchesChangesColumn = 9;
    static constexpr int kBranchesCreatedColumn = 10;
    static constexpr int kBranchesUpdatedColumn = 11;
    static constexpr int kBranchesWorktreeColumn = 12;
    static constexpr int kBranchesColumnCount = 13;
    struct BranchRefHistory {
        qint64 createdAt = 0;
        qint64 movedAt = 0;
        QString createdFrom;
        int ownUpdates = 0;
        int moves = 0;
    };

    // Safe target for the website's secret-free setup link. Only bounded
    // public deployment topology may be prefilled; credentials are rejected
    // by the URL parser and entered in the focused session-only local field.
    void openCloudflareSetupFromSystemLink(const QString &target = {});
    // Secret-free website handoff. The desktop resolves the public repository
    // through the mesh and asks the user where its local fork should live.
    void openRepositoryForkFromSystemLink(const QString &target);
    void openFolderFromSystem(const QString &folder);
    // Secret-free website handoff for provider-owned CLI authentication. The
    // URL contains only a bounded node name and provider id; ForkMesh resolves
    // that node against local/saved hosts and opens the existing local or
    // TOFU-pinned SSH terminal. Credentials never pass through the URL/relay.
    void openAgentLoginFromSystemLink(const QString &node,
                                      const QString &provider);
    // The same provider sign-in, streamed to the account owner's browser
    // instead of a window here: the website has no way to reach this machine
    // otherwise, and the `forkmesh://` handoff above is a dead end on any
    // computer without this app installed. One session at a time, gated on the
    // same "let the website run agents on this node" consent, and the only
    // command it can run is the provider's own login (AgentLoginPty).
    void onNodeTerminalFrame(const QJsonObject &frame);
    void sendNodeTerminalFrame(const QString &session, const QString &action,
                               const QJsonObject &fields = {});
    void stopNodeTerminalSession();
    // The owner's phone away from home: its local control channel carried
    // end to end sealed over the relay socket (LocalControl.h; node_control.py
    // on the relay). Frames for another machine are ignored.
    void onNodeControlFrame(const QJsonObject &frame);
    void onNodeControlRelayRestored();

    void logCapturedMessage(QtMsgType type, const QString &text,
                            const QString &sourceFile = QString(),
                            int sourceLine = 0);

    static void applyTheme(
        forkmesh::market::PackageRegistry *registry = nullptr);
    void refreshThemedIcons();

    static QString autolinkReferences(
        const QString &markdown,
        const std::function<QString(const QString &kind, int number)>
            &statusGlyph = {});
    // autolinkReferences with each issue/PR link led by its status octicon,
    // resolved against the loaded issue and pull lists.
    QString autolinkReferencesWithStatus(const QString &markdown) const;

    static const Issue *looperPickNext(
        const QList<Issue> &issues,
        const std::function<bool(int)> &hasLocalSession);

#ifdef FORKMESH_WINDOW_TESTS
    using TestIssueHistoryDeleteRunner =
        std::function<bool(int number, QString *error)>;
    void testStartOffThreadTeardownProbe(std::atomic_bool *started,
                                         std::atomic_bool *allowFinish,
                                         std::atomic_bool *finished,
                                         std::atomic_bool *applied,
                                         std::atomic_bool *applyCaptureDestroyed,
                                         std::atomic_bool *applyCaptureDestroyedOffThread)
    {
        struct ThreadBoundApplyCapture {
            std::atomic_bool *destroyed = nullptr;
            std::atomic_bool *destroyedOffThread = nullptr;

            ~ThreadBoundApplyCapture()
            {
                destroyed->store(true, std::memory_order_release);
                if (QThread::currentThread() !=
                    QCoreApplication::instance()->thread())
                    destroyedOffThread->store(true, std::memory_order_release);
            }
        };
        auto applyCapture = std::make_shared<ThreadBoundApplyCapture>();
        applyCapture->destroyed = applyCaptureDestroyed;
        applyCapture->destroyedOffThread = applyCaptureDestroyedOffThread;
        runOffThread<bool>(
            [started, allowFinish, finished] {
                started->store(true, std::memory_order_release);
                while (!allowFinish->load(std::memory_order_acquire))
                    QThread::msleep(1);
                finished->store(true, std::memory_order_release);
                return true;
            },
            [applied, applyCapture = std::move(applyCapture)](bool) {
                applied->store(true, std::memory_order_release);
            });
    }
    void testSetBeforeOffThreadDrainHook(std::function<void()> hook)
    {
        m_testBeforeOffThreadDrainHook = std::move(hook);
    }
    void testCreateTransientBordersForTeardown()
    {
        flashErrorBorder();
        flashCelebrationBorder();
        startRestartCautionFlash();
    }
    bool testTransientBordersDestroyed() const
    {
        return !m_errorBorderOverlay && !m_errorBorderTimer &&
               !m_celebrationBorderOverlay && !m_celebrationBorderTimer &&
               !m_restartCautionBorderOverlay &&
               !m_restartCautionBorderTimer;
    }
    void testStartStallWatchdogForTeardown();
    bool testStallWatchdogRunning() const;
    // Stands in for the dialogs a session leaves open — a QDialog child of
    // this window, shaped like the ones the crash reports named, whose
    // destroyed() handler is bound to `this`. `deliveredAfterBody` records a
    // delivery made after the destructor body finished, which is past the
    // point where every MainWindow member has been destroyed; `probeDestroyed`
    // is bound to qApp instead and so still reports the dialog going away.
    void testInstallTeardownDeliveryProbe(std::atomic_bool *bodyFinished,
                                          std::atomic_bool *deliveredAfterBody,
                                          std::atomic_bool *probeDestroyed);
    void testShowFirstRunWelcome() { showFirstRunWelcome(); }
    void testStartFeatureTour() { startFeatureTour(); }
    bool testSetPackageInstalled(const QString &id, bool installed)
    {
        return setPackageInstalled(id, installed, false);
    }
    forkmesh::ui::OnboardingTour *testOnboardingTour() const
    {
        return m_onboardingTour;
    }
    void testShowTipWalletSetupDialog() { linkTipWallet(); }
    void testShowTipPaymentDialog(const QString &recipientName,
                                  const QString &recipientAddress,
                                  quint64 fixedLamports,
                                  const QString &rewardId)
    {
        sendTipToMember(recipientName, recipientAddress, fixedLamports,
                        rewardId);
    }
    // Runs `body` with this window standing as a signed-in, desktop-capable
    // account — the state authenticateSilently() reaches against a live relay
    // — then puts every account field back. A check can thus read the
    // credential a payout notice carries without a relay or a session token;
    // the signing key is the real one this test profile generated at startup.
    void testWithOwnerSigningAccount(const QString &accountName,
                                     const std::function<void()> &body);
    QString testProfilePublicKey() const
    {
        return m_profileIdentity.publicKey();
    }
    // The URL prepareContributorNoticeRequest() would POST a payout notice to,
    // or an empty one when this window holds no credential for it.
    QUrl testContributorNoticeRequestUrl(const QString &recipientName,
                                         quint64 lamports,
                                         const QString &chainSignature,
                                         const QString &rewardId);
    // Renders the real Calendar page against a supplied activity set. The page
    // reads Git from every configured repository, which a test cannot stand up
    // cheaply at the size where its rendering cost actually shows.
    void testRenderCalendarActivity(const QJsonArray &activity,
                                    const QDate &anchor, const QString &mode)
    {
        ensureSectionBuilt(kCalendarSectionIndex);
        m_repositoryCalendarEvents = activity;
        m_repositoryCalendarScans.clear();
        m_repositoryCalendarQueue.clear();
        // Claim the current view so a later navigation does not re-read Git
        // over the supplied set.
        m_repositoryCalendarRequest = QStringLiteral("window-tests");
        m_calendarAnchor = anchor;
        m_calendarMode = mode;
        m_calendarAgendaLimit = 0;
        renderCalendar();
    }
    void testExtendCalendarAgenda(int by)
    {
        m_calendarAgendaLimit += by;
        renderCalendar();
    }
    QTableWidget *testCalendarMonthTable() const { return m_calendarMonthTable; }
    QTableWidget *testCalendarWeekTable() const { return m_calendarWeekTable; }
    QListWidget *testCalendarAgendaList() const { return m_calendarAgendaList; }
    QLabel *testCalendarStatus() const { return m_calendarStatus; }
    QString testMostRecentUnreadConversation(
        const QString &currentConversation, const QStringList &channels,
        const QSet<QString> &unread,
        const QHash<QString, qint64> &lastMessageMs)
    {
        const QString savedCurrent = m_currentConversation;
        const QStringList savedChannels = m_channels;
        const QStringList savedOpenDms = m_openDms;
        const QSet<QString> savedUnread = m_unread;
        const auto savedHistory = m_history;

        m_currentConversation = currentConversation;
        m_channels = channels;
        m_openDms.clear();
        m_unread = unread;
        m_history.clear();
        for (auto it = lastMessageMs.constBegin(); it != lastMessageMs.constEnd();
             ++it) {
            ChatMessage message;
            message.timestampMs = it.value();
            m_history[it.key()].append(message);
        }
        const QString result = mostRecentUnreadConversation();

        m_currentConversation = savedCurrent;
        m_channels = savedChannels;
        m_openDms = savedOpenDms;
        m_unread = savedUnread;
        m_history = savedHistory;
        return result;
    }
    // Restore an unread state over a transcript and read back what survives
    // reconciliation, the way a relaunch does. `fromOthers` / `fromSelf` say how
    // many messages each conversation still holds after the retention sweep.
    QHash<QString, int> testReconcileUnreadCounts(
        const QHash<QString, int> &unreadCounts,
        const QHash<QString, int> &fromOthers,
        const QHash<QString, int> &fromSelf = QHash<QString, int>(),
        QSet<QString> *marked = nullptr)
    {
        const QSet<QString> savedUnread = m_unread;
        const QHash<QString, int> savedCounts = m_unreadCounts;
        const auto savedHistory = m_history;

        m_history.clear();
        const auto seed = [this](const QHash<QString, int> &counts, bool self) {
            for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
                QList<ChatMessage> &messages = m_history[it.key()];
                for (int i = 0; i < it.value(); ++i) {
                    ChatMessage message;
                    message.conversation = it.key();
                    message.timestampMs = 1000 + i;
                    message.self = self;
                    messages.append(message);
                }
            }
        };
        seed(fromOthers, false);
        seed(fromSelf, true);
        m_unreadCounts = unreadCounts;
        m_unread.clear();
        for (auto it = unreadCounts.constBegin(); it != unreadCounts.constEnd();
             ++it)
            m_unread.insert(it.key());

        reconcileUnreadWithHistory();

        const QHash<QString, int> result = m_unreadCounts;
        if (marked)
            *marked = m_unread;
        m_unread = savedUnread;
        m_unreadCounts = savedCounts;
        m_history = savedHistory;
        return result;
    }
    void testSetRoster(const QList<MemberInfo> &members) { setRoster(members); }
    // Stands in for the relay reachability probe: the offscreen suite has no
    // route off the machine, so the only way to reach automatic offline mode
    // is to hand the verdicts over directly.
    void testNoteRelayReachability(bool reachable)
    {
        noteRelayReachability(reachable);
    }
    bool testOfflineModeIsAutomatic() const { return m_offlineModeAuto; }
    bool testOfflineModeActive() const { return m_offlineMode; }
    void testSetOfflineMode(bool offline, bool automatic = false)
    {
        setOfflineMode(offline, automatic);
    }
    // Issue #712: the startup restore rule, exercised without a second window.
    void testRestorePersistedOfflineMode() { restorePersistedOfflineMode(); }
    bool testOfflineModeAnnouncementPending() const
    {
        return m_offlineModeAnnouncementPending;
    }
    void testAnnounceRestoredOfflineMode() { announceRestoredOfflineMode(); }
    void testResetRosterForAlerts()
    {
        m_homeRoster.clear();
        m_removedPeerIds.clear();
        m_peerLastSeenMs.clear();
    }
    void testAgePeerSightings(qint64 ageMs)
    {
        for (auto it = m_peerLastSeenMs.begin(); it != m_peerLastSeenMs.end(); ++it)
            *it -= ageMs;
    }
    QList<MemberInfo> testHomeRoster() const { return m_homeRoster; }
    void testSetHomeRosterAndReloadMirrorPanel(const QList<MemberInfo> &members)
    {
        m_homeRoster = members;
        loadMirrorNodesPanel();
        testSettleMirrorNodesPanel();
    }
    void testSettleMirrorNodesPanel();
    void testSetNodeAlertGraceUntilMs(qint64 value) { m_nodeAlertGraceUntilMs = value; }
    // GET /api/accounts/<name> answers with the whole account record and is
    // read once per account per session; these drive the cache its readers
    // share and count what actually left the app.
    void testCacheAccountRecord(const QString &account,
                                const QJsonObject &record)
    {
        cacheAccountRecord(account, record);
    }
    QJsonObject testCachedAccountRecord(const QString &account) const
    {
        return cachedAccountRecord(account);
    }
    QString testSettingsAccountName() const { return settingsAccountName(); }
    QDialog *testShowEditAccountProfileDialog()
    {
        showEditAccountProfileDialog();
        return m_accountProfileDialog.data();
    }
    QJsonObject testAccountProfileEditBody() const
    {
        return accountProfileEditBody();
    }
    void testInvalidateAccountRecord(const QString &account = QString())
    {
        invalidateAccountRecord(account);
    }
    int testAccountRecordCacheSize() const { return m_accountRecords.size(); }
    int testAccountRecordGets(const QString &account) const;
    // What a top-bar repaint does for the signed-in user: the switcher rebuilds
    // on most navigations and asks for that user's payout address.
    void testRepaintTopBarForUser(const QString &account)
    {
        m_nodeOwnerUser = account;
        refreshWebUserSolanaAddress();
    }
    QString testWebUserSolanaAddress() const { return m_webSolanaAddress; }
    QStringList testNetworkLog() const { return m_networkLog; }
    int testStallCount() const { return m_stallCount; }
    bool testDeleteNetworkLogLine(const QString &line)
    {
        return deleteNetworkLogLine(line);
    }
    QString testNetworkLogPath() const { return networkLogPath(); }
    // Exactly what closeEvent() does to the log. A test can call it without
    // tearing the window down, and it fails loudly if a rewrite ever creeps
    // back onto the shutdown path.
    void testPersistNetworkLogOnShutdown() { persistNetworkLogOnShutdown(); }
    void testResetNetworkLog();
    void testLogSystem(const QString &text) { logSystem(text); }
    bool testPlannedRestartOpen(const QString &node) const
    {
        return m_plannedRestarts.isOpen(node);
    }
    void testExpirePlannedRestarts(qint64 nowMs) { closeExpiredPlannedRestarts(nowMs); }
    // adhoc #36: drive a ticket the whole way — open, retire, flush — so the
    // outcome line's source link can be checked without waiting out the
    // footer's 90 ms spin timer.
    void testNoteBackgroundActivity(quint64 id, const QString &kind,
                                    const QString &detail, bool backgrounded,
                                    bool started,
                                    const forkmesh::LogSource &source)
    {
        forkmesh::BackgroundTicket ticket;
        ticket.id = id;
        ticket.kind = kind;
        ticket.detail = detail;
        ticket.source = source;
        noteBackgroundActivity(ticket, backgrounded, started);
    }
    // adhoc #186: the same for a ticket that also names its routine, checkout,
    // queue wait and outcome.
    void testNoteBackgroundTicket(const forkmesh::BackgroundTicket &ticket,
                                  bool backgrounded, bool started)
    {
        noteBackgroundActivity(ticket, backgrounded, started);
    }
    void testFlushBackgroundOutcomes() { flushBackgroundOutcomes(true); }
    void testLoadNetworkLog(const QStringList &lines)
    {
        m_networkLog = lines;
        clearLogStyleCache();
        m_networkLogViewStale = true;
    }
    void testRecordUiStall(qint64 peakMs, const QString &blockingCall,
                           const QString &backtrace,
                           const QString &stallId = QString(),
                           const QString &likelyOrigin = QString(),
                           qint64 startedAtMs = -1)
    {
        onUiStall(peakMs, stallId, likelyOrigin, blockingCall, backtrace,
                  startedAtMs);
    }
    QString testStallFixPrompt() const { return stallFixPrompt(); }
    QString testQtDiagnosticsReport() const;
    bool testQtDiagnosticsDetailed() const;
    void testSampleQtDiagnostics();
    void testShowQtDiagnosticsDialog() { showQtDiagnosticsDialog(); }
    void testFlashMessage(const QString &text, bool error = false,
                          const QString &clickHref = QString(),
                          const QString &kind = QString())
    {
        flashMessage(text, error, clickHref, kind);
    }
    QString testLatestNoticeText() const
    {
        return m_recentNotices.isEmpty() ? QString() : m_recentNotices.constFirst();
    }
    QStringList testNoticeTexts() const { return m_recentNotices; }
    QStringList testPageNoticeTexts() const;
    bool testPageNoticeVisible() const;
    QString testPageNoticeOpenText() const;
    void testClickPageNoticeOpen();
    void testClickPageNoticeClear();
    QString testCurrentNoticePage() const { return currentNoticePage(); }
    void testClearPageNotices()
    {
        clearPageNotices();
        m_recentNotices.clear();
    }
    bool testStartQuickAddCloudAgent(int repoIndex, const QString &prompt,
                                     const QString &provider)
    {
        return startQuickAddCloudAgent(repoIndex, prompt, provider, QString(),
                                       false);
    }
    void testAddNotification(const QString &title, const QString &body,
                             bool warning = false)
    {
        addNotification(title, body, warning);
    }
    void testAddLinkedNotification(const QString &title, const QString &body,
                                   const NotificationLink &link)
    {
        addNotification(title, body, false, link);
    }
    void testPostNotification(const QString &title, const QString &body,
                              const QString &activateHref)
    {
        postNotification(title, body, false, QString(), activateHref);
    }
    QString testDesktopNotificationHref() const
    {
        return m_desktopNotificationHref;
    }
    void testResetDesktopNotificationHref()
    {
        m_desktopNotificationHref.clear();
    }
    void testSetRestartCautionFlash(bool active)
    {
        if (active)
            startRestartCautionFlash();
        else
            stopRestartCautionFlash();
    }
    void testAnnounceOrgTaskOpened(int sessionId)
    {
        announceOrgTaskOpened(sessionId);
    }
    void testFlyPromptSparkleToAgents() { flyPromptSparkleToAgents(); }
    void testFloatPromptIntoTranscript(const QString &prompt)
    {
        floatPromptIntoTranscript(prompt, {});
    }
    void testFloatPromptOntoAgentDetail(const QString &prompt);
    QPushButton *testAgentsNavButton() const { return m_agentsNavButton; }
    void testCloudLogMonitorLine(const QString &line)
    {
        handleCloudLogMonitorLine(line);
    }
    void testCloudLogMonitorChunk(const QByteArray &chunk)
    {
        consumeCloudLogMonitorBytes(chunk);
    }
    int testCloudLogMonitorErrors() const { return m_cloudLogMonitorErrors; }
    int testCloudLogMonitorEvents() const { return m_cloudLogMonitorEvents; }
    int testCloudLogMonitorPendingRecords() const
    {
        return m_cloudLogMonitorPendingRecords.size();
    }
    void testDrainCloudLogMonitorRecords() { drainCloudLogMonitorRecords(); }
    QString testLogUsageHtml() const;
    void testSetLogUsageReport(const QJsonObject &report)
    {
        m_logUsageReport = report;
        refreshLogUsageView();
    }
    QString testLogD1UsageHtml() const;
    void testSetLogD1UsageReport(const QJsonObject &report)
    {
        m_logD1UsageReport = report;
        refreshLogD1UsageView();
    }
    QString testLogWorkerFootprintHtml() const;
    void testSetLogWorkerFootprintReport(const QJsonObject &report)
    {
        m_logWorkerFootprintReport = report;
        refreshLogWorkerFootprintView();
    }
    QString testLogCronHtml() const;
    void testSetLogCronReport(const QJsonObject &report)
    {
        m_logCronReport = report;
        refreshLogCronView();
    }
    void testResetCloudLogMonitor()
    {
        setCloudLogMonitorEnabled(false);
        cancelCloudLogMonitorRestart();
        m_cloudLogMonitorErrors = 0;
        m_cloudLogMonitorEvents = 0;
        m_cloudLogMonitorRestarts = 0;
        m_cloudLogMonitorIdleReason.clear();
    }
    int testCloudLogMonitorTailEnded(int exitCode, qint64 uptimeMs)
    {
        handleCloudLogMonitorEnded(exitCode, uptimeMs);
        const int pending = m_cloudLogMonitorRestartTimer &&
                                    m_cloudLogMonitorRestartTimer->isActive()
                                ? m_cloudLogMonitorRestartTimer->remainingTime()
                                : 0;
        cancelCloudLogMonitorRestart();
        return pending;
    }
    bool testCloudLogMonitorRunning() const { return cloudLogMonitorRunning(); }
    void testCoreLogFeedLine(const QString &node, const QByteArray &line)
    {
        handleCoreLogFeedLine(node, line);
    }
    const forkmesh::corelogs::ActivityTally &testCoreLogTally() const
    {
        return m_coreLogTally;
    }
    QPushButton *testLogFilterChip(const QString &category) const;
    int testLogFilterChipCount(const QString &category) const
    {
        return logFilterChipCount(category);
    }
    QStringList testChatReactors(const QString &messageId,
                                 const QString &reaction) const
    {
        return m_reactions.value(messageId).value(reaction);
    }
    void testShowLoadStatus(const QString &what) { showLoadStatus(what); }
    void testDismissLoadStatus() { dismissLoadStatus(); }
    bool testLoadStatusShowing() const { return m_loadStatusShowing; }
    QString testLoadStatusText() const;
    void testMarkLocalPingsSeen() { markLocalPingsSeen(); }
    int testPendingDismissOnlyPings() const
    {
        int pending = 0;
        for (const AppNotification &item : m_notifications) {
            if (item.dismissOnly && !item.acknowledged)
                ++pending;
        }
        return pending;
    }
    void testNotifyUsageLimitReady(const QString &providerKey,
                                   const QString &windowKey,
                                   const QString &providerName,
                                   const QString &windowName, qint64 resetMs)
    {
        notifyUsageLimitReady(providerKey, windowKey, providerName, windowName,
                              resetMs);
    }
    void testApplyClaudeRateLimitEvent(const QJsonObject &rateLimitInfo)
    {
        applyClaudeRateLimitEvent(rateLimitInfo);
    }
    // A countdown to the reset is running for this window.
    bool testUsageLimitReminderArmed(const QString &providerKey,
                                     const QString &windowKey) const
    {
        QTimer *timer = m_usageLimitReminderTimers.value(
            providerKey + QLatin1Char('/') + windowKey);
        return timer && timer->isActive();
    }
    void testScheduleUsageLimitReminder(const QString &providerKey,
                                        const QString &windowKey,
                                        const QString &providerName,
                                        const QString &windowName,
                                        qint64 resetMs)
    {
        scheduleUsageLimitReminder(providerKey, windowKey, providerName,
                                   windowName, resetMs);
    }
    void testHoldAgentForUsageLimit(int sessionId)
    {
        holdAgentForUsageLimit(sessionId);
    }
    bool testUsageLimitAgentHoldArmed(int sessionId) const
    {
        QTimer *timer = m_usageLimitAgentHoldTimers.value(sessionId);
        return timer && timer->isActive();
    }
    void testRestoreUsageLimitAgentHolds() { restoreUsageLimitAgentHolds(); }
    bool testErrorBorderVisible() const
    {
        return m_errorBorderOverlay && m_errorBorderOverlay->isVisible();
    }
    void testNotifyAgentDone(int sessionId, const QString &closingSummary)
    {
        if (closingSummary.isEmpty())
            m_lastAssistantText.remove(sessionId);
        else
            m_lastAssistantText[sessionId] = closingSummary;
        notifyAgentDone(sessionId);
    }
    // notifyIfInactive only posts when the window has lost focus, which a test
    // cannot pin down, so the two halves are exercised directly instead.
    QString testAgentDoneNotificationBody(int sessionId, const QString &summary)
    {
        return agentDoneNotificationBody(sessionId, summary);
    }
    void testActivateDesktopNotification(const QString &href)
    {
        activateDesktopNotification(href);
    }
    int testSelectedAgentSessionId() const { return m_selectedAgentSessionId; }
    QString testDeliverChatMessage(const QString &conversation,
                                   const QString &senderId,
                                   const QString &senderName,
                                   const QString &text)
    {
        static int seq = 0;
        ChatMessage message;
        message.id = QStringLiteral("test-chat-%1").arg(++seq);
        message.conversation = conversation;
        message.senderId = senderId;
        message.senderName = senderName;
        message.text = text;
        message.timestampMs = QDateTime::currentMSecsSinceEpoch();
        onMessage(message);
        return message.id;
    }
    bool testHasChatBackend() const { return m_backend != nullptr; }
    // Deliver a message with a chosen timestamp (and optionally as a thread
    // reply), so a test can reproduce a replayed backlog arriving out of order.
    QString testDeliverChatMessageAt(const QString &conversation,
                                     const QString &senderId,
                                     const QString &senderName,
                                     const QString &text, qint64 timestampMs,
                                     const QString &threadRootId = QString(),
                                     bool self = false,
                                     const QString &fileName = QString())
    {
        static int seq = 0;
        ChatMessage message;
        message.id = QStringLiteral("test-chat-at-%1").arg(++seq);
        message.conversation = conversation;
        message.senderId = senderId;
        message.senderName = senderName;
        message.text = text;
        message.timestampMs = timestampMs;
        message.threadRootId = threadRootId;
        message.self = self;
        message.fileName = fileName;
        onMessage(message);
        return message.id;
    }
    QString testCurrentConversation() const { return m_currentConversation; }
    QString testChatUnreadBannerText() const;
    bool testChatUnreadJumpShown() const;
    QString testChatAllUnreadBadge() const;
    QStringList testChatDmRows() const;
    QString testChatListPreview(const QString &conversation) const;
    qint64 testChatListPreviewTime(const QString &conversation) const;
    QPixmap testChatListAvatar(const QString &conversation) const;
    int testChatListRowHeight(const QString &conversation) const;
    void testEditChatMessage(const QString &conversation,
                             const QString &messageId, const QString &text)
    {
        onMessageEdited(conversation, messageId, text);
    }
    void testDeleteChatMessage(const QString &conversation,
                               const QString &messageId)
    {
        onMessageDeleted(conversation, messageId);
    }
    QString testChatTopicText() const;
    void testOpenNextUnreadConversation() { openNextUnreadConversation(); }
    bool testChatChannelIsPrivate(const QString &conversation) const
    {
        return m_privateChannels.contains(conversation);
    }
    void testEnsureTeamRoom() { ensureTeamRoom(); }
    void testJoinPrivateChannel(const QString &channel)
    {
        onPrivateChannelJoined(channel);
    }
    void testFileChatPing(const QString &conversation, const QString &title,
                          const QString &body)
    {
        AppNotification item;
        item.title = title;
        item.body = body;
        item.kind = QStringLiteral("chat");
        item.link.kind = QStringLiteral("chat");
        item.link.ref = conversation;
        item.quiet = true;
        recordNotification(item);
    }
    void testPurgePrivateChatPings() { purgePrivateChatPings(); }
    void testAttachChatBackend(ChatBackend *backend) { attachBackend(backend); }
    void testTriggerTeamChatMention(const QString &text,
                                    const QString &threadRootId)
    {
        triggerTeammatesForEvent(QStringLiteral("mentions"), text, -1,
                                 QStringLiteral("#team"), threadRootId);
    }
    void testBindTeammateChatThread(int sessionId,
                                    const QString &threadRootId)
    {
        bindTeammateChatThread(sessionId, QStringLiteral("#team"),
                               threadRootId);
    }
    QStringList testChatThreadTexts(const QString &conversation,
                                    const QString &rootMessageId) const
    {
        QStringList texts;
        for (const ChatMessage &message : m_history.value(conversation)) {
            if (message.threadRootId == rootMessageId)
                texts.append(message.text);
        }
        return texts;
    }
    QStringList testChatThreadSenderIds(const QString &conversation,
                                        const QString &rootMessageId) const
    {
        QStringList senderIds;
        for (const ChatMessage &message : m_history.value(conversation)) {
            if (message.threadRootId == rootMessageId)
                senderIds.append(message.senderId);
        }
        return senderIds;
    }
    QStringList testChatMentionCandidates();
    void testSwitchConversation(const QString &conversation)
    {
        switchConversation(conversation);
    }
    void testOpenChatThread(const QString &rootMessageId)
    {
        openChatThread(rootMessageId);
    }
    bool testChatThreadPaneVisible() const
    {
        return m_chatThreadPane && m_chatThreadPane->isVisible();
    }
    QString testChatThreadCountText() const;
    void testCloseChatThread() { closeChatThread(); }
    void testForgetConversation(const QString &conversation)
    {
        m_history.remove(conversation);
        m_unread.remove(conversation);
        m_unreadCounts.remove(conversation);
    }
    int testTranscriptRowCount() const { return int(m_visibleRows.size()); }
    QString testFirstTranscriptMessageId() const
    {
        for (const ChatMessage &message : m_history.value(m_currentConversation))
            if (message.threadRootId.isEmpty())
                return message.id;
        return QString();
    }
    bool testTranscriptRebuildPending() const
    {
        return m_transcriptRebuildPending;
    }
    // Read back the transcript as it is actually laid out, top to bottom, so a
    // test sees the rendered order rather than a restatement of the model.
    QStringList testTranscriptRowOrder() const;
    MessageRow *testCreateChatMessageRow(const QString &senderId,
                                         const QString &senderName)
    {
        ChatMessage message;
        message.id = QStringLiteral("test-chat-profile");
        message.conversation = QStringLiteral("#general");
        message.senderId = senderId;
        message.senderName = senderName;
        message.text = QStringLiteral("profile fixture");
        message.timestampMs = QDateTime::currentMSecsSinceEpoch();
        return createMessageRow(message);
    }
    bool testCelebrationBorderVisible() const
    {
        return m_celebrationBorderOverlay &&
               m_celebrationBorderOverlay->isVisible();
    }
    void testResetLoggedErrorAlerts()
    {
        m_lastLoggedErrorText.clear();
        m_lastLoggedErrorAtMs = 0;
        m_loggedErrorGroups.clear();
        m_plannedRestarts.clear();
        if (m_errorBorderTimer)
            m_errorBorderTimer->stop();
        if (m_errorBorderOverlay)
            m_errorBorderOverlay->hide();
    }
    bool testDraftStallPromptInComposer();
    QString testQuickAddText() const;
    QString testDiagnosticsReport() { return diagnosticsReport(false); }
    QPushButton *testDebugCopyDiagnosticsButton() const
    {
        return m_debugCopyDiagnosticsButton;
    }
    void testShowSettingsSection() { showSection(1); }
    void testShowChatSection() { showSection(2); }
    // The proof and the ordered body fields one organization request signs
    // with, so a test can pin the desktop table against the relay's without
    // reaching through a network round trip.
    static QString testOrganizationAdminProof(const QByteArray &method,
                                              const QString &path,
                                              QStringList *fields = nullptr)
    {
        return organizationAdminProof(method, path, fields);
    }
    void testSetOrganizations(const QJsonArray &organizations)
    {
        m_organizations = organizations;
    }
    void testShowNewRepositoryDialog() { createNewRepository(); }
    void testShowHomeSection() { showSection(0); }
    void testShowLogSection() { showSection(4); }
    void testShowMarketSection() { showSection(kMarketSectionIndex); }
    bool testPackageInstalled(const QString &id) const;
    QString testPackageName(const QString &id) const;
    bool testPackageSurfaceShown(const QString &packageId) const
    {
        return packageSurfaceShown(packageId);
    }
    bool testRailButtonShown(const QString &key) const;
    bool testRepoTabShown(const QString &key) const;
    // Stands in for the save/open file pickers of the package data export and
    // import, so a test can round-trip a package's data without a dialog.
    void testSetPackageDataFilePath(const QString &path)
    {
        m_packageDataTestFilePath = path;
    }
    // The rail's right-click menu for a package, without the menu popping up.
    QStringList testPackageRailMenuActions(const QString &entrypoint);
    void testTriggerPackageRailAction(const QString &entrypoint,
                                      const QString &text);
    // Inject the counts /api/market/installs would have returned, so the
    // shelf's popularity line is testable without a relay.
    void testSetPackageInstallCounts(
        const QHash<QString, QPair<qint64, qint64>> &counts);
    void testSetPackageMarketScores(
        const QHash<QString, MarketPackageScore> &scores);
    int testMarketPreviewDecodes() const { return m_marketPreviewDecodes; }
    bool testCapturePackageActionVisible() const;
    void testStartCapturePackage();
    bool testCapturePackageRuntimeActive() const;
    bool testLedgerPackageActionVisible() const;
    bool testLedgerPackageRuntimeActive() const;
    QUrl testLedgerPackageUrl() const;
    bool testDiscordPackageActionVisible() const;
    bool testDiscordPackageRuntimeActive() const;
    bool testNativePackageActionVisible(const QString &id) const;
    bool testNativePackageRuntimeActive(const QString &id) const;
    QStringList testNativePackageIds() const;
    void testShowNativePackage(const QString &id);
    QWidget *testNativePackageWindow(const QString &id) const;
    QPushButton *testPackageActionsButton() const
    {
        return m_packageActionsButton;
    }
    QStringList testPackageActionsMenu(const QString &id);
    // Sidebar rows as rendered: "[Group]" headers, "Group > #channel" members,
    // bare "#channel" for the top level.
    QStringList testChatSidebarRows() const;
    bool testCreateChannelGroup(const QString &name);
    void testMoveChannelToGroup(const QString &channel, const QString &group)
    {
        moveChannelToGroup(channel, group);
    }
    void testRemoveChannelGroup(const QString &group) { removeChannelGroup(group); }
    void testSetChannels(const QStringList &channels) { setChannels(channels); }
    void testSetOfficeConversations(const QStringList &conversations)
    {
        m_officeConversations = conversations;
        setChannels(m_channels);
    }
    QString testChatConversationLabel(const QString &conversation) const
    {
        return chatConversationLabel(conversation);
    }
    QString testWelcomeChannel() const { return welcomeChannelForIdentity(); }
    void testShowLedgerSection() { showSection(kLedgerSectionIndex); }
    // Feed the page exactly what the two relay endpoints would have returned,
    // so the row and payment-button rules are exercised without a network.
    void testLoadLedgerFixture(const QByteArray &ledgerBody,
                               const QByteArray &estimatesBody);
    int testLedgerRowCount() const;
    int testLedgerWalletCardCount() const;
    QString testLedgerWalletCardText(int index) const;
    bool testLedgerWalletCardRefreshable(int index) const;
    void testAddLedgerEntry(const QString &direction, const QString &category,
                            const QString &description,
                            const QString &counterparty,
                            const QString &currency, const QString &amount,
                            bool outstanding);
    bool testLedgerRowHasLocalButtons(int row) const;
    void testSettleFirstLocalLedgerRecord();
    void testRemoveFirstLocalLedgerRecord();
    QString testLedgerCellText(int row, int column) const;
    QString testLedgerCellToolTip(int row, int column) const;
    // Where clicking this cell would go, which for the Transaction column is
    // the transfer's own page on a public Solana explorer.
    QUrl testLedgerCellLinkUrl(int row, int column) const;
    // What a click on this cell actually opened, and what a double-click on
    // it opened: an offscreen build records the destination instead of handing
    // it to a browser. An invalid URL back means nothing was opened.
    QUrl testClickLedgerCell(int row, int column);
    QUrl testDoubleClickLedgerCell(int row, int column);
    // Whether hovering this cell offers the hand cursor a link deserves.
    bool testHoverLedgerCellIsLink(int row, int column);
    bool testLedgerRowHasHeldPayoutBadge(int row) const;
    bool testLedgerRowHasPayButton(int row) const;
    bool testLedgerRowHasPaymentSpinner(int row) const;
    bool testClickLedgerPayButton(int row);
    // Put one payable row at the point reached immediately after the wallet
    // password is submitted, then finish it without a live transfer. This lets
    // the offscreen suite pin the row feedback and the deferred balance refresh.
    void testStartLedgerPaymentProcessing(const QString &recordId,
                                          const QString &walletAddress,
                                          bool balanceReadInFlight);
    void testFinishLedgerPaymentProcessing();
    // Put a payment approved behind the one in flight on a row, or drop every
    // such payment again, without ever reaching the wallet or the network.
    void testQueueLedgerPayment(const QString &recordId);
    void testClearTipTransferQueue();
    bool testLedgerWalletRefreshQueued(const QString &address) const;
    void testApplyLedgerWalletBalance(const QString &address, qint64 lamports,
                                      qint64 checkedAt);
    qint64 testLedgerWalletBalance(const QString &address) const;
    bool testLedgerRowHasRemindButton(int row) const;
    // Apply an owner-scoped /api/rewards/estimates body's reminder times the
    // way a signed-in load would, then read back the Remind button they shape.
    void testApplyLedgerReminderTimes(const QByteArray &ownerEstimatesBody);
    QString testLedgerRemindButtonText(int row) const;
    QString testLedgerRemindButtonToolTip(int row) const;
    bool testLedgerRemindButtonEnabled(int row) const;
    QStringList testLedgerRecipientCandidates() const
    {
        return ledgerRecipientCandidates();
    }
    void testSortLedgerBy(int column, bool ascending);
    // Exercise the same pending-and-paid reward renderer the relay response
    // reaches, without making the window test depend on a live relay.
    void testLoadContributorRewardsFixture(const QJsonArray &rewards)
    {
        ensureSectionBuilt(kControlNodeSectionIndex);
        renderContributorRewards(rewards);
    }
    QString testContributorRewardsStatusText() const;
    void testSetLogOverlayExpanded(bool expanded)
    {
        setLogOverlayExpanded(expanded);
    }
    bool testApplyFooterWebsiteStatusPayload(const QJsonObject &payload)
    {
        return applyFooterWebsiteStatusPayload(payload);
    }
    void testApplyFooterWebsiteStatusFailure(int httpStatus)
    {
        applyFooterWebsiteStatusFailure(httpStatus);
    }
    QUrl testWebsiteStatusTargetUrl(const QString &statusId) const
    {
        return websiteStatusTargetUrl(statusId);
    }
    QString testApplyDesktopWebsiteProbe(const QString &id, int httpStatus,
                                         const QByteArray &body,
                                         const QString &transportError = QString(),
                                         bool reachableByOtherPrograms = false);
    void testResetLocalFirewallAlert() { m_localFirewallAlertMs = 0; }
    QString testDesktopWebsiteRowStatus(const QString &id) const;
    QStringList testWebsiteRecheckProbes(const QString &statusId) const;
    void testClearDesktopProbeHistory() { m_desktopProbeHistory.clear(); }
    int testNotificationsMentioning(const QString &needle) const
    {
        int count = 0;
        for (const AppNotification &item : m_notifications) {
            if (item.title.contains(needle) || item.body.contains(needle))
                ++count;
        }
        return count;
    }
    int testNotificationsTitled(const QString &needle) const
    {
        int count = 0;
        for (const AppNotification &item : m_notifications) {
            if (item.title.contains(needle))
                ++count;
        }
        return count;
    }
    int testNotificationsWith(const QString &needle) const
    {
        int count = 0;
        for (const AppNotification &item : m_notifications)
            if (item.title.contains(needle, Qt::CaseInsensitive) ||
                item.body.contains(needle, Qt::CaseInsensitive))
                ++count;
        return count;
    }
    QString testNotificationTextTitled(const QString &needle) const
    {
        for (const AppNotification &item : m_notifications) {
            if (item.title.contains(needle))
                return item.title + QLatin1Char('\n') + item.body;
        }
        return QString();
    }
    QString testPingStatusFor(const QString &needle);
    void testNotifyActionEvent(forkmesh::ui::ActionPingEvent event,
                               const QString &title, bool warning = false)
    {
        notifyActionEvent(event, title, QString(), warning);
    }
    // Simulate the acknowledgement of a pause heartbeat without starting a
    // socket. True means the newer local deadline correctly remains pending.
    bool testAcknowledgePingPauseDelivery(qint64 deliveredPause,
                                         qint64 currentPause);
    // Open the Pings page and rebuild its grid, the way showNotifications()
    // does when the rail button is pressed.
    void testShowPingsSection();
    // Click the settings button in the row whose title contains the needle;
    // true when Settings > Pings opened with that ping's kind highlighted.
    bool testClickPingSettings(const QString &titleNeedle,
                               QString *highlightedKind = nullptr);
    // Apply a heartbeat reply's ping settings (or not, while a push is
    // pending) without a socket.
    void testApplyPingSettingsReply(const QJsonObject &reply);
    // How far past the grid's own viewport the columns reach. Zero means every
    // column is on screen; the bug this guards is a Detail column frozen at the
    // width of a whole agent summary.
    int testPingsTableOverflowPx() const;
    // Click one row's Send-to-prompt button and hand back what the footer
    // composer holds afterwards.
    QString testClickPingPrompt(const QString &titleNeedle);
    bool testPingCellOpens(const QString &titleNeedle, int column) const;
    QString testOpenPingDetail(const QString &titleNeedle);
    bool testOpenPingLink(const QString &titleNeedle)
    {
        for (const AppNotification &item : m_notifications) {
            if (item.title.contains(titleNeedle) && item.link.isValid()) {
                openNotificationLink(item.link);
                return true;
            }
        }
        return false;
    }
    bool testPingDetailVisible() const;
    void testSetFooterUpdateLine(const QString &line)
    {
        setFooterUpdateLine(line);
        flushFooterLogLines();
    }
    void testFlushFooterLogLines() { flushFooterLogLines(); }
    void testShowHostsSection() { showSection(7); }
    QWidget *testPageFindRoot() const { return pageFindRoot(); }
    void testSetDirectoryUserNodes(const QString &user,
                                   const QStringList &nodes);
    void testShowNodesSection();
    QStringList testNodeDirectoryNames() const;
    void testAddMirrorNodeRow(const QString &node, const QString &commit);
    QStringList testNodeDirectoryRowTexts() const;
    void testSortNodeDirectory(int column, Qt::SortOrder order);
    void testRefreshNodeDirectory() { refreshNodesTable(); }
    void testReloadMirrorNodesPanel() { loadMirrorNodesPanel(); }
    // Stand in for one /api/repo/<owner>/<repo>/mirrors record, the way
    // fetchNodesCatalogInfo() files them, so the mirror columns can be
    // exercised without a relay.
    void testSetNodesCatalogRecord(const QString &node,
                                   const QJsonObject &record)
    {
        m_nodesCatalogInfo.insert(node.trimmed().toLower(), record);
    }
    QString testNodeDirectoryCell(const QString &node,
                                  const QString &columnCaption) const;
    QStringList testNodeDirectoryHeadersWithoutIcons() const;
    void testRecordDeletedMeshNode(const QString &node)
    {
        recordDeletedMeshNode(node);
    }
    void testReloadDeletedMeshNodes()
    {
        m_deletedMeshNodes = {};
        m_deletedMeshNodesLoaded = false;
    }
    void testClearDeletedMeshNode(const QString &node)
    {
        clearDeletedMeshNode(node);
    }
    void testRecordDecommissionedHost(const QString &node)
    {
        recordDecommissionedMirrorNode(node);
    }
    void testClearDecommissionedHost(const QString &node)
    {
        clearDecommissionedMirrorNode(node);
    }
    void testSetAdmin(bool admin)
    {
        m_isAdmin = admin;
        updateAdminCrownBadge();
        applyDebugBarStartupPreference();
    }
    bool testUsersNavButtonVisible() const;
    void testShowUsersSection() { showSection(kUsersSectionIndex); }
    void testApplyUsersDirectory(const QJsonArray &users)
    {
        mergeChatUserDirectory(users);
    }
    void testApplyUsersQuarantine(const QJsonArray &users)
    {
        m_usersQuarantinePayload = users;
        renderUsersQuarantinePage(users);
    }
    void testApplyUsersSignupReferralCode(const QJsonObject &state)
    {
        renderUsersSignupReferralCode(state);
    }
    QString testUsersSignupReferralCode() const;
    QString testUsersSignupReferralCodeStatus() const;
    bool testUsersSignupReferralCodeCopyEnabled() const;
    bool testUsersDirectoryHasAdminPayload() const
    {
        return m_usersDirectoryAdminPayload;
    }
    void testShowNotesSection() { showSection(kNotesSectionIndex); }
    bool testFilesNavButtonVisible() const;
    bool testFilesRepoTreeShowing() const;
    bool testFilesCoveTreeShowing() const;
    int testCodeFilesPageCount() const;
    void testShowFilesSection() { showSection(kFilesSectionIndex); }
    void testShowStatsSection() { showSection(kStatsSectionIndex); }
    StatsDashboard *testStatsDashboard() const { return m_statsDashboard; }
    void testShowMemorySection() { showSection(kMemorySectionIndex); }
    void testShowGovernorSection() { showSection(kGovernorSectionIndex); }
    void testShowProjectSettings(const QString &slug) { showProjectSettings(slug); }
    QString testProjectSettingsSlug() const { return m_projectSettingsSlug; }
    bool testProjectSettingsShowing() const;
    QTreeWidget *testMemoryTree() const { return m_memoryTree; }
    QTreeWidget *testMemoryAppTree() const { return m_memoryAppTree; }
    bool testMemorySampling() const { return m_memoryTimer && m_memoryTimer->isActive(); }
    QString testMemoryReport() const { return m_memoryLastReport; }
    void testShowHomeGridSection() { showSection(kHomeGridSectionIndex); }
    HomeBoard *testHomeGridBoard() const { return m_homeGridBoard; }
    QPushButton *testHomeGridNavButton() const { return m_homeGridNavButton; }
    QVector<HomeGroup> testHomeGridGroups() const;
    void testRefreshHomeGridPage() { refreshHomeGridPage(); }
    bool testActivateHomeGridTile(const QString &key) { return activateHomeGridTile(key); }
    bool testHomeGridTicking() const { return m_homeGridTimer && m_homeGridTimer->isActive(); }
    QString testHomeGridHeadline() const;
    bool testHomeGridSearchShown() const;
    QLineEdit *testHomeGridFilter() const { return m_homeGridFilter; }
    HomePageDots *testHomeGridDots() const { return m_homeGridDots; }
    bool testStatsNavButtonVisible() const;
    // Every rail destination must actually be IN the rail. A button that was
    // registered with m_navGroup but never added to the rail layout has no
    // parent and simply never renders — and isHidden() returns false for it,
    // so a visibility check does not catch that. Returns the labels of any
    // such buttons; empty means every destination is reachable.
    QStringList testNavButtonsMissingFromRail() const;
    QStringList testRailOrderKeys() const;
    void testApplyRailOrder() { applyRailOrder(); }
    void testRecordRailClick(const QString &key) { recordRailClick(key); }
    QString testCreateRailFolder(const QString &name);
    void testMoveRailItem(const QString &key, const QString &folderId)
    {
        moveRailItemToFolder(key, folderId);
    }
    void testDeleteRailFolder(const QString &folderId)
    {
        deleteRailFolder(folderId);
    }
    QStringList testRailFolderMemberKeys(const QString &folderId) const;
    void testSetRailSortMode(const QString &mode)
    {
        setRailSortMode(RailGroups::parseSortMode(mode));
    }
    void testSetRailGrouped(bool grouped) { setRailGrouped(grouped); }
    QString testCreateRailGroup(const QString &name);
    void testAssignRailGroup(const QString &key, const QString &groupId)
    {
        assignRailItemToGroup(key, groupId);
    }
    void testDeleteRailGroup(const QString &groupId) { deleteRailGroup(groupId); }
    QStringList testRailSortMenuActions();
    QPushButton *testRailSortButton() const;
    bool testRailFolderExpanded(const QString &folderId) const;
    bool testRailFolderShowsActive(const QString &folderId) const;
    void testExpandRailFolder(const QString &folderId, bool expanded);
    bool testClickRailFolderMember(const QString &folderId, const QString &key);
    QWidget *testRailContent() const;
    QLayout *testRailLayout() const;
    bool testCanPopOutRailItem(const QString &key) const;
    void testPopOutRailItem(const QString &key);
    QWidget *testPoppedOutWindow(const QString &key) const;
    void testPopOutLogPage() { popOutLogPage(); }
    QStringList testRepoTabRowKeys() const;
    QStringList testRepoCodeBarKeys() const;
    void testBuildRepoCodeTab() { ensureRepoDetailTabBuilt(0); }
    QPushButton *testRepoBranchesButton() const { return m_branchesButton; }
    void testMoveButtonToPanel(const QString &key, const QString &panel)
    {
        moveButtonToPanel(key, panel);
    }
    void testResetButtonPlacement() { resetButtonPlacement(); }
    bool testIsDraggablePageButton(QAbstractButton *button)
    {
        return isDraggablePageButton(button);
    }
    void testClickPageButton(QAbstractButton *button);
    QString testPageButtonPath(QWidget *button) const { return pageButtonPath(button); }
    QAbstractButton *testPageButtonAtPath(const QString &path) const
    {
        return pageButtonAtPath(path);
    }
    QString testDropPageButtonOnRail(QAbstractButton *button, int y)
    {
        return dropPageButtonOnRail(button, y);
    }
    void testActivateRailShortcut(const QString &id) { activateRailShortcut(id); }
    void testRemoveRailShortcut(const QString &id) { removeRailShortcut(id); }
    QWidget *testRailShortcutTile(const QString &id) const;
    void testBuildRepoDetailSection() { ensureRepoDetailSectionBuilt(); }
    QStringList testButtonPlacementMenuActions(const QString &key);
    QPushButton *testRepoPullsTab() const { return m_repoPullsTab; }
    int testRepoDetailTabIndex() const;
    int testSectionIndex() const;
    StatsTileData testStatsTileData(const QString &id) { return statsTileData(id); }
    void testShowFilesCoveTree()
    {
        showSection(kFilesSectionIndex);
        showFilesCoveTree();
    }
    void testClickFilesNavButton() { openFilesSectionAtRepoRoot(); }
    void testSetFileExplorerShowHidden(bool on);
    void testSetFileExplorerRoot(const QString &path)
    {
        setFileExplorerRoot(path);
    }
    QString testFileExplorerRoot() const { return m_fileExplorerRoot; }
    QStringList testFileExplorerNames() const;
    QStringList testExpandFileExplorerEntry(const QString &name);
    QStringList testExpandedFileExplorerPaths() const;
    void testPreviewFileExplorerFile(const QString &path)
    {
        previewFileExplorerFile(path);
    }
    QString testFileExplorerPreview() const;
    void testRevealFileInExplorer(const QString &path, int line)
    {
        revealFileInExplorer(path, line);
    }
    int testFileExplorerPreviewLine() const;
    // Files editor (adhoc #1607): the breadcrumb naming the open tab's file,
    // the find bar's state, and the symbol trace panel's rows.
    QString testFilesBreadcrumb() const;
    QString testFilesCursorLabel() const;
    void testOpenFilesFindBar(const QString &term);
    QString testFilesFindCount() const;
    void testStepFilesFind(int delta);
    int testFilesFindLine() const;
    void testTraceFilesSymbol(const QString &symbol, int mode)
    {
        traceFilesSymbol(symbol, mode);
    }
    QString testFilesSymbolTitle() const;
    QStringList testFilesSymbolRows() const;
    bool testFilesSymbolPanelVisible() const;
    // adhoc #2093: the repository tree lists the whole checkout — tracked files
    // and the untracked and ignored ones beside them — so these read back every
    // path in it, open one of its collapsed working-tree directories the way a
    // click does, and say whether the open editor tab can be typed into.
    QStringList testRepoFileTreePaths() const;
    bool testRepoFileTreeLoading() const { return m_repoFileTreeLoading; }
    QStringList testExpandRepoFileTreeEntry(const QString &path);
    bool testFilesEditorEditable() const;
    // adhoc #1634: single-click a top-level explorer row by name, as the mouse
    // does, and read back the editor tabs that are open beside the tree.
    void testClickFileExplorerEntry(const QString &name);
    QStringList testOpenFileTabTitles() const;
    // adhoc #1634: the Size column toggle, off until someone asks for it.
    void testSetFilesShowSize(bool on);
    bool testFilesSizeColumnVisible() const;
    QStringList testUsersColumns() const;
    QString testUsersCellText(int row, const QString &header) const;
    QStringList testUsersTabLabels() const;
    void testShowUsersTab(int index);
    QStringList testUsersQuarantineColumns() const;
    QString testUsersQuarantineCellText(int row,
                                        const QString &header) const;
    void testSelectUsersRow(int row);
    void testSelectUsersQuarantineRow(int row);
    QStringList testSelectedUsersActionTargets() const
    {
        return selectedUsersActionTargets();
    }
    bool testUsersTipButtonEnabled() const;
    QString testUsersTipButtonToolTip() const;
    bool testUsersQuarantineButtonEnabled() const;
    QString testUsersQuarantineButtonText() const;
    QString testUsersQuarantineButtonToolTip() const;
    bool testUsersUnquarantineButtonEnabled() const;
    QString testUsersUnquarantineButtonToolTip() const;
    bool testUsersDeleteButtonEnabled() const;
    QString testUsersDeleteButtonText() const;
    QString testUsersDeleteButtonToolTip() const;
    QStringList testSortUsersBy(const QString &header, Qt::SortOrder order);
    QStringList testChatMemberNames(const QString &conversation);
    QStringList testTeamRoomMembersWith(const QList<MemberInfo> &roster,
                                        const QList<ChatMessage> &messages,
                                        qint64 *elapsedMs);
    void testRenderNetworkRepos(const QJsonArray &repos);
    QStringList testNetworkRepoNames() const;
    QString testNetworkRepoActionText(int row) const;
    QString testNetworkRepoMirrorHeader() const;
    QStringList testNetworkRepoColumns() const;
    QString testNetworkRepoCellText(int row, const QString &header) const;
    bool testNetworkRepoHasCommitSparkline(int row) const;
    QString testNetworkRepoCommitActivitySummary(int row) const;
    bool testNetworkRepoHasMergeChart(int row) const;
    QString testNetworkRepoMergeActivitySummary(int row) const;
    QStringList testLocalRepoNames() const;
    QString testLocalRepoCellText(int row, const QString &header) const;
    QString testLocalReposStatusText() const;
    QStringList testNetworkTabLabels() const;
    void testRenderNetworkWebRequests(const QJsonObject &payload);
    QStringList testNetworkWebRequestsGroups() const;
    QString testNetworkWebRequestsCellText(int row, const QString &header) const;
    QString testNetworkWebRequestsStatusText() const;
    int testNetworkWebRequestsChartHeight() const;
    QJsonObject testWebRequestsSummary(int windowMinutes, qint64 nowMs) const;
    // adhoc #142: the Firewall Rules grid counts the live traffic each rule is
    // deciding and links every row to the log filtered to it.
    QStringList testFirewallRuleColumns() const;
    int testFirewallRuleRow(const QString &storedRule) const;
    QString testFirewallRuleCellText(int row, const QString &header) const;
    QString testFirewallStatusText() const;
    QStringList testFirewallRules(bool deny) const;
    void testSetFirewallRules(const QStringList &allow, const QStringList &deny);
    // Puts a connection to url through the firewall's rules the way a request
    // or WebSocket does, crediting the deciding rule, then updates the counts.
    void testRecordFirewallMatch(const QUrl &url);
    void testRecordFirewallRequest(const QString &method, const QUrl &url,
                                   const QString &rule, bool allowed)
    {
        recordFirewallRequest(method, url, rule, allowed);
    }
    QString testFirewallRuleDraft() const;
    QString testCurrentNetworkTabText() const;
    // Click the row's "open the log filtered to this rule" cell; reports the
    // stored rule it acted on.
    QString testClickFirewallRuleLogCell(int row);
    int testReposNavBadgeCount() const;
    void testRebuildNetworkLogView() { rebuildNetworkLogView(); }
    QStringList testLogFilterChipLabels() const;
    void testSetLogFilter(const QString &category)
    {
        m_logFilters.clear();
        if (!category.isEmpty())
            m_logFilters.insert(category);
        rebuildLogFilterButtons();
        rebuildNetworkLogView();
        refreshLogTimelineChart();
        updateLogAggregationAvailability();
    }
    bool testLogCategoryEnabled(const QString &badge) const
    {
        return logCategoryEnabled(badge);
    }
    void testSetLogCategoryEnabled(const QString &badge, bool enabled)
    {
        setLogCategoryEnabled(badge, enabled);
    }
    bool testLoggingEnabled() const { return loggingEnabled(); }
    void testSetLoggingEnabled(bool enabled) { setLoggingEnabled(enabled); }
    int testDeleteLogCategory(const QString &badge)
    {
        return deleteLogCategory(badge);
    }
    void testSetLogTextFilter(const QString &text) { setLogTextFilter(text); }
    QString testLogTextFilter() const { return m_logTextFilter; }
    QString testLogSearchEditText() const;
    // Click the Web Requests row's "open the log filtered to this route group"
    // button, or the route-group label itself. Both report the group they acted
    // on so the test can name it.
    QString testClickWebRequestsLogButton(int row);
    QString testClickWebRequestsGroupLabel(int row);
    QString testWebRequestsChartGroupAt(int row) const;
    QString testClickWebRequestsChartLabel(int row);
    QString testLogBadgeFor(const QString &storedLine) const
    {
        return logBadgeFor(storedLine);
    }
    void testOpenFullLogForCategory(const QString &category)
    {
        openFullLogForCategory(category);
    }
    QTextBrowser *testNetworkLogView() const
    {
        // Tests read the view right after logging; land the queued batch first.
        const_cast<MainWindow *>(this)->flushNetworkLogLiveLines();
        return m_settingsLog;
    }
    bool testNetworkLogFollowing() const { return m_logFollowTail; }
    void testFlushNetworkLogLiveLines() { flushNetworkLogLiveLines(); }
    QTextEdit *testFooterLogView() const
    {
        const_cast<MainWindow *>(this)->flushFooterLogLines();
        return m_footerUpdateLog;
    }
    QWidget *testLogTimelineChart() const;
    int testLogTimelineVisibleCount() const;
    void testSetLogTimelineHours(int hours);
    QString testLogTimelineSummary() const;
    QStringList testLogViewTabLabels() const;
    bool testLogAggregationTabEnabled() const;
    QString testLogFrequencyMessageAt(int row) const;
    int testLogFrequencyCountAt(int row) const;
    bool testClickLogFrequencyPromptButton(int row);
    bool testFaviconCached(const QString &host) const
    {
        return m_faviconCache.contains(host);
    }
    void testScrollNetworkLogToTop() { onNetworkLogScrolled(0); }
    QStringList testQuickUpdatePullArguments(const QString &clientDir) const;
    QString testWorkingClientDir() const;
    QString testRunningClientDir() const;
    QString testRunningClientExecutable() const;
    QStringList testBuildAndPreviewSteps(const QString &gitDir,
                                         const QString &previewDir,
                                         const QString &clientDir,
                                         const QString &buildDir,
                                         const QString &commit,
                                         bool haveWorktree) const;
    void testSetSetupInputs(const QString &name, const QString &solana);
    void testSetAccountFlowResult(bool result, bool desktopCapable = true)
    {
        m_testUseAccountFlowResult = true;
        m_testAccountFlowResult = result;
        m_testAccountFlowDesktopCapable = desktopCapable;
        m_testEnsureNodeAccountCalls = 0;
    }
    void testEnableSessionStartBypass(bool value)
    {
        m_testBypassServerStart = value;
        if (value) {
            m_pendingSilentAuth = false;
            m_pendingRestoreRepoIndex = -1;
            m_deferredStartupRun = true;
            m_startupAuthResolved = true;
        }
    }
    void testMarkDeferredStartupRunning()
    {
        m_deferredStartupRun = true;
        m_startupAuthResolved = false;
    }
    void testRunDeferredStartupNow() { runDeferredStartup(); }
    void testStartSession() { startSession(); }
    void testEnablePaidMirroring() { enablePaidMirroring(); }
    int testAccountFlowCalls() const { return m_testEnsureNodeAccountCalls; }
    int testStackIndex() const;
    bool testSignInButtonVisible() const;
    void testRefreshSignInButton() { updateSignInButton(); }
    QString testUserName() const { return m_userName; }
    QString testAccountName() const { return m_accountName; }
    bool testChatIdentityIsGuest() const { return chatIdentityIsGuest(); }
    QString testChatDisplayName() const { return chatDisplayName(); }
    QString testMachineNodeName() const { return machineNodeName(); }
    QString testSavedSolanaAddress() const;
    bool testAccountAuthenticated() const { return m_accountAuthenticated; }
    QString testAccountTier() const { return m_accountTier; }
    QString testRecentUserInstance(const QString &accountName) const;
    int testRecentUserIdentityCount() const;
    int testRecentUserIdentityCount(const QString &accountName) const;
    void testRememberUserIdentity(const QString &accountName,
                                  const QString &instanceUrl);
    int testConfiguredServerCount() const { return m_servers.size(); }
    QString testConfiguredServerId(int index) const
    {
        return index >= 0 && index < m_servers.size()
                   ? m_servers.at(index).id
                   : QString();
    }
    QString testConfiguredServerName(int index) const
    {
        return index >= 0 && index < m_servers.size()
                   ? m_servers.at(index).name
                   : QString();
    }
    QString testActiveServerInstanceUrl() const
    {
        return activeServerInstanceUrl();
    }
    bool testActiveServerMatches(const QString &id,
                                 const QString &url) const
    {
        return activeServerMatches(id, url);
    }
    QString testPrivateChannelsSettingsKey() const
    {
        return privateChannelsSettingsKey();
    }
    QString testChatHistoryPath() const { return chatHistoryPath(); }
    void testCacheAccountSession(const QString &accountName,
                                 const QString &instanceUrl,
                                 const QString &token,
                                 bool desktopCapable,
                                 const QString &desktopPublicKey);
    QString testCachedAccountSession(const QString &accountName,
                                     const QString &instanceUrl) const;
    bool testCachedAccountSessionMaySendTo(const QString &accountName,
                                           const QString &instanceUrl,
                                           const QUrl &target) const;
    bool testCachedAccountSessionMayRestoreDesktop(
        const QString &accountName, const QString &instanceUrl,
        const QString &currentPublicKey,
        const QString &validatedAccountPublicKey) const;
    QString testAccountInstanceUrl(const QString &instanceUrl) const;
    QUrl testAccountApiUrlForInstance(const QString &instanceUrl,
                                      const QString &leaf) const;
    bool testAccountLoginResponseAccepted(const QString &requestedAccount,
                                          const QJsonObject &payload) const;
    void testClearAccountSessionCache();
    int testAccountSessionCacheSize() const { return m_accountSessionCache.size(); }
    bool testHasOwnerSigningCapability() const
    {
        return hasOwnerSigningCapability(m_accountName);
    }
    Q_INVOKABLE bool testColumnsBecomeResizable();
    Q_INVOKABLE bool testSpreadsheetResize();
    Q_INVOKABLE bool testSpreadsheetResizeAfterMove();
    Q_INVOKABLE bool testAgentListChromeHidden() const;
    Q_INVOKABLE QString testAgentColumnLayout() const;
    Q_INVOKABLE int testAddLocalRepository(const QString &owner, const QString &name,
                                           const QString &localPath);
    Q_INVOKABLE bool testOpenRepository(int index);
    int testResolveAgentRepositoryIndex(int preferred) const
    {
        return resolveAgentRepositoryIndex(preferred);
    }
    void testSetRepositoryMirrorPath(int index, const QString &mirrorPath)
    {
        if (index >= 0 && index < m_repositories.size())
            m_repositories[index].mirrorPath = mirrorPath;
    }
    int testAddRemoteMirrorRecord(const QString &owner, const QString &name,
                                  const QString &cloneUrl,
                                  const QString &mirrorPath)
    {
        RepositoryRecord repo;
        repo.owner = owner;
        repo.name = name;
        repo.cloneUrl = cloneUrl;
        repo.mirrorPath = mirrorPath;
        m_repositories.append(repo);
        return m_repositories.size() - 1;
    }
    void testSyncRepository(int index, bool quiet = true)
    {
        syncRepository(index, quiet);
        QElapsedTimer wait;
        wait.start();
        while (!m_mirrorSyncAssociationChecks.isEmpty() && wait.elapsed() < 5000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    void testReattachRepositorySource(int index, const QString &source);
    bool testRepositorySyncInProgress(int index) const
    {
        return m_syncingRepos.contains(index);
    }
    bool testRepositorySyncQuarantined(int index) const
    {
        return index >= 0 && index < m_repositories.size() &&
               m_repositories.at(index).mirrorSyncQuarantined;
    }
    static QString testMirrorSourceIdentity(const QString &source);
    QString testRepositorySyncQuarantineReason(int index) const
    {
        return index >= 0 && index < m_repositories.size()
                   ? m_repositories.at(index).mirrorSyncQuarantineReason
                   : QString();
    }
    void testDiscardRepositoryRecord(int index)
    {
        if (index < 0 || index >= m_repositories.size())
            return;
        m_repositories.removeAt(index);
        saveRepositories();
    }
    int testRepoIssuesTabBadgeCount() const;
    int testRepoPullsTabBadgeCount() const;
    int testRepoDiscussionsTabBadgeCount() const;
    void testSetDiscussionList(QList<Discussion> discussions)
    {
        ++m_discussionCountLoadGen; // invalidate in-flight background reloads
        m_currentDiscussions = std::move(discussions);
        updateRepoDiscussionCount();
    }
    void testMarkDiscussionRead(const Discussion &discussion)
    {
        if (m_repoDetailIndex >= 0 && m_repoDetailIndex < m_repositories.size()) {
            const RepositoryRecord &repo = m_repositories.at(m_repoDetailIndex);
            recordDiscussionRead(repo.owner + "/" + repo.name, discussion);
        }
    }
    void testSetRepoTabLoadedCounts(int openIssues, int pulls)
    {
        m_currentIssues.clear();
        for (int number = 1; number <= openIssues; ++number) {
            Issue issue;
            issue.number = number;
            issue.status = QStringLiteral("open");
            m_currentIssues.append(issue);
        }
        m_currentPulls.clear();
        for (int number = 1; number <= pulls; ++number) {
            PullRequest pull;
            pull.number = number;
            m_currentPulls.append(pull);
        }
        updateRepoIssueCount();
        updateRepoPullCount();
    }
    bool testRepoRequiresPeerApproval() const
    {
        return m_repoDetailIndex >= 0 &&
               m_repoDetailIndex < m_repositories.size() &&
               m_repositories.at(m_repoDetailIndex).requirePeerApproval;
    }
    void testSetRepoRequirePeerApproval(bool on)
    {
        setRepoRequirePeerApproval(on);
    }
    Q_INVOKABLE QString testRepoDefaultBranch() const;
    Q_INVOKABLE void testRefreshOpenRepoDetail() { refreshOpenRepoDetail(); }
    Q_INVOKABLE QString testRepoActionsTabText() const;
    Q_INVOKABLE QString testRepoBranchesButtonText() const;
    Q_INVOKABLE bool testShowRepoIssuesTab();
    void testSetPendingInboxCounts(int issues, int pulls, int discussions);
    int testIssueInboxBadgeCount() const;
    int testPullInboxBadgeCount() const;
    int testPullsTabAlertBadgeCount() const;
    QString testDiscussionInboxButtonText() const;
    Q_INVOKABLE bool testSaveRepoAboutMetadata(const QString &about,
                                               const QString &website)
    {
        return saveRepoAboutMetadata(about, website, nullptr);
    }
    Q_INVOKABLE int testProvisionNewRepository(const QString &dest,
                                               const QString &name,
                                               const QString &description,
                                               const QString &firstPrompt,
                                               bool addReadme,
                                               bool isPrivate = false,
                                               bool localOnly = false)
    {
        return provisionNewRepository(dest, name, description, firstPrompt,
                                      addReadme, isPrivate, localOnly, nullptr);
    }
    int testAddPublishedRepository(const QString &owner, const QString &name,
                                   const QString &mirrorPath);
    int testAddPrivateLocalRepository(const QString &owner, const QString &name,
                                      const QString &localPath);
    QString testRepoAgentGitDir(int index) const
    {
        return (index < 0 || index >= m_repositories.size())
                   ? QString()
                   : repoAgentGitDir(m_repositories.at(index));
    }
    int testRepoTabContentTop(); // y of the tab content within the window
    int testRepoTabGapAroundIssues() const;
    int testRailTabIconLineSkew() const;
    int testIssueLooperGapFromNewIssueButton() const;
    bool testIssueLooperRowAligned() const;
    int testTopNavTrailingGap() const;
    // The Timers package's top-bar clock: add a "timer", "countdown" (the
    // default) or "alarm" the way its menu does, read the chips back, act on
    // one by label, and move the strip's clock so an expiry can be watched
    // without waiting for it.
    bool testAddTimer(const QString &request, const QString &kind = QString());
    QStringList testTimerChipTexts() const;
    bool testTimerCommand(const QString &labelPrefix, const QString &command);
    void testAdvanceTimers(qint64 milliseconds);
    // The URL and JSON body sendTimerPing() would POST for an expiry, or an
    // empty URL when this window holds no credential for it.
    QUrl testTimerPingRequest(const QString &timerId, const QString &kind,
                              qint64 endsAtMs, const QString &label,
                              QByteArray *body = nullptr);
    QString testRepoGitDir() const { return repoGitDir(); }
    void testSetIssueHistoryDeleteRunner(TestIssueHistoryDeleteRunner runner)
    {
        m_testIssueHistoryDeleteRunner = std::move(runner);
    }
    void testSetIssueList(QList<Issue> issues)
    {
        m_currentIssues = std::move(issues);
        refreshIssueList();
    }
    void testShowIssue(int number) { showIssue(number); }
    // "Text" or "Text (disabled)" per action of the row's right-click menu;
    // separators are "-" and submenu entries are prefixed with "Submenu/".
    QStringList testIssueListContextMenuActions(int number);
    QList<int> testIssueListNumbers() const;
    // "row"/"avatar"/"assignee"/"votes"/"title"/"meta" rects of one summary row.
    QVariantMap testIssueRowGeometry(int issueNumber) const;
    void testDeleteIssueWithHistory(int number) { deleteCurrentIssueWithHistory(number); }
    bool testIssueHistoryDeleteInProgress() const
    {
        return m_issueHistoryDeleteInProgress;
    }
    Q_INVOKABLE int testQuickAddIssueNoAgent(const QString &title);
    int testQuickAddAgent(const QString &prompt);
    bool testIssueDetailVisible() const
    {
        return m_issueDetail && m_issueDetail->isVisible();
    }
    // Which page of the repository detail stack is up (3 is Agents).
    int testRepoDetailPage() const;
    // The issue sidebar's agent summary: the markup it renders, and following
    // the anchor inside it the way a click on the block does.
    QString testIssueAgentSummaryHtml() const;
    bool testFollowIssueAgentLink();
    // False when a sidebar section reserves more height than the page it is
    // showing needs — the empty band a plain QStackedWidget leaves behind.
    bool testIssueSidebarSectionsFitTheirPage() const;
    QString testQuickAddAgentProvider() const;
    QString testIssueAgentProvider() const;
    QString testIssueAgentModel() const;
    void testSetDefaultAgentProvider(const QString &provider);
    void testSetQuickAddAgentProvider(const QString &provider);
    QStringList testQuickAddModelLabels() const;
    bool testQuickAddModelVisible() const;
    bool testQuickAddModelEditable() const;
    void testRefreshQuickAddAgentModelSelector();
    QString testQuickAddAgentModelLabel(const QString &model) const;
    QString testQuickAddAgentModelMergedNote(const QString &model) const;
    QString testPromptOverlayPlacement() const;
    QStringList testShortcutMetadata(const QString &filePath) const;
    int testShortcutUsageCount(const QString &filePath,
                               const QString &scope) const
    {
        return shortcutUsageCount(shortcutUsageKey(filePath, scope));
    }
    void testRecordShortcutUsage(const QString &filePath, const QString &scope)
    {
        recordShortcutUsage(filePath, scope);
    }
    // One launch that finished with `ok` after `durationMs`, as a script run
    // would record it.
    void testRecordShortcutRun(const QString &filePath, const QString &scope,
                               bool ok, qint64 durationMs)
    {
        recordShortcutRunResult(recordShortcutUsage(filePath, scope), ok,
                                durationMs);
    }
    bool testRunShortcut(const QString &filePath) { return runShortcut(filePath); }
    QString testScrubTermShortcutPath(int repoIndex) const
    {
        return scrubTermShortcutPath(repoIndex);
    }
    QList<forkmesh::localcontrol::insights::ShortcutRun> testShortcutRuns() const
    {
        return m_shortcutRuns;
    }
    // A dedicated agent launched from an entry, then its session finishing.
    void testRecordShortcutAgentRun(int sessionId, const QString &filePath,
                                    const QString &scope, bool ok,
                                    qint64 durationMs)
    {
        trackShortcutAgentRun(sessionId, recordShortcutUsage(filePath, scope));
        recordShortcutAgentFinished(sessionId, ok, durationMs);
    }
    int testTeamSectionIndex() const { return kTeamSectionIndex; }
    void testShowTeamSection() { showSection(kTeamSectionIndex); }
    int testTeammateRepoIndex() const { return activeTeammateRepoIndex(); }
    void testChooseTeammateRepo(int repoIndex)
    {
        setActiveTeammateRepoIndex(repoIndex);
    }
    int testShortcutsTabIndex() const { return m_shortcutsTabIndex; }
    int testRepoSettingsTabIndex() const { return m_settingsTabIndex; }
    QString testTeammatesSummary() const;
    QString testSelectedTeammateId() const { return m_selectedTeammateId; }
    void testShowTeammateDetail(const QString &id) { showTeammateDetail(id); }
    void testShowTeammateHome(const QString &id) { showTeammateHome(id); }
    QString testTeammateHomeId() const
    {
        return teammateHomeVisible() ? m_teammateHomeId : QString();
    }
    QWidget *testTeammateHomePage() const { return m_teammateHomePage; }
    void testRefreshTeammateHome() { refreshTeammateHome(true); }
    void testSendTeammateHomeMessage(const QString &text)
    {
        sendTeammateHomeMessage(text);
    }
    forkmesh::teammates::Workspace testTeammateWorkspace(const QString &id) const
    {
        return loadTeammateWorkspace(id);
    }
    void testSaveTeammateWorkspace(const QString &id,
                                   const forkmesh::teammates::Workspace &workspace)
    {
        saveTeammateWorkspace(id, workspace);
    }
    void testSetTeammateStartHook(
        std::function<int(const QString &, const QString &)> hook)
    {
        m_testTeammateStartHook = std::move(hook);
    }
    void testSetTeammateContinueHook(
        std::function<void(int, const QString &)> hook)
    {
        m_testTeammateContinueHook = std::move(hook);
    }
    void testSetTeammateAgentOutput(int sessionId, const QString &output)
    {
        m_lastAssistantText.insert(sessionId, output);
    }
    int testTeammateTaskGrade(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions) {
            if (session.id == sessionId)
                return session.taskGrade;
        }
        return 0;
    }
    bool testSetTeammateTaskGrade(int sessionId, int grade)
    {
        return setTeammateTaskGrade(sessionId, grade);
    }
    bool testTeammatePanelVisible() const
    {
        return m_teammateDetailPanel && m_teammateDetailPanel->isVisible();
    }
    void testReloadTeammatesPanel() { loadTeammatesPanel(); }
    QWidget *testTeammateRail() const { return m_teammateRail; }
    QWidget *testPackageRail() const { return m_packageRail; }
    void testRefreshPackageRail()
    {
        m_packageRailSignature.clear();
        refreshPackageRail();
    }
    void testRefreshTeammateRail()
    {
        m_teammateRailSignature.clear();
        refreshTeammateRail();
    }
    void testSetTeammateRailEnabled(bool enabled)
    {
        setTeammateRailEnabled(enabled);
    }
    void testMarkTeammateDoneUnseen(int sessionId)
    {
        markTeammateDoneUnseen(sessionId);
    }
    bool testTeammateDoneUnseen(int sessionId) const
    {
        return m_teammateDoneUnseen.contains(sessionId);
    }
    bool testAgentSessionUnread(int sessionId)
    {
        const AgentSession *session = findAgentSession(sessionId);
        return session && agentSessionUnread(*session);
    }
    bool testAgentRowUnread(int sessionId) const;
    void testMarkAgentSessionRead(int sessionId)
    {
        markAgentSessionRead(sessionId);
    }
    void testSetAgentReadBaseline(qint64 baselineMs)
    {
        m_agentReadBaselineMs = baselineMs;
    }
    QString testAdoptCreatedTeammate(int repoIndex, const QString &output)
    {
        QString name;
        adoptCreatedTeammate(repoIndex, output, &name);
        return name;
    }
    QString testTeammateCreatorPrompt(int repoIndex) const
    {
        return buildTeammateCreatorPrompt(repoIndex);
    }
    forkmesh::teammates::AgentChoice testRecentlyUsedAgentChoice() const
    {
        return recentlyUsedAgentChoice();
    }
    forkmesh::teammates::AgentChoice testPromptAreaAgentChoice() const
    {
        return promptAreaAgentChoice();
    }
    QString testTeammatePromptDraft(const QString &id) const
    {
        return m_teammatePromptDrafts.value(id);
    }
    QString testTeammatePrompt(
        const QString &id,
        const QString &taskType = QStringLiteral("on-demand")) const
    {
        const forkmesh::teammates::LoadResult loaded =
            forkmesh::teammates::Store(teammatesDirPath()).load();
        for (const forkmesh::teammates::Config &teammate : loaded.teammates) {
            if (teammate.id == id)
                return buildTeammatePrompt(teammate, QStringLiteral("Test run"),
                                           taskType);
        }
        return {};
    }
    int testTeammateLastSessionId(const QString &id, int repoIndex,
                                  bool forget = false)
    {
        const forkmesh::teammates::LoadResult loaded =
            forkmesh::teammates::Store(teammatesDirPath(repoIndex)).load();
        for (const forkmesh::teammates::Config &teammate : loaded.teammates) {
            if (teammate.id != id)
                continue;
            if (forget) {
                forkmesh::teammates::Config wiped = teammate;
                wiped.lastSessionId = 0;
                saveTeammateRuntime(wiped, repoIndex);
            }
            return teammateWithRuntime(teammate, repoIndex).lastSessionId;
        }
        return 0;
    }
    bool testTeammateRunHasSubstantiveChanges(int sessionId)
    {
        for (const AgentSession &session : std::as_const(m_agentSessions)) {
            if (session.id == sessionId)
                return teammateRunHasSubstantiveChanges(session);
        }
        return false;
    }
    void testSwitchToWorktree(const QString &branch)
    {
        showOverviewWorktrees();
        loadWorktreesPanel();
        selectWorktreeRow(branch);
    }
    bool testMergeBaseIntoLinkedWorktree(const QString &worktree,
                                         const QString &branch,
                                         const QString &base)
    {
        return mergeBaseIntoLinkedWorktree(worktree, branch, base).status ==
               WorktreeMergeReport::Merged;
    }
    void testSetMergeConflictEditorHook(
        std::function<bool(
            const QString &, const QStringList &,
            const std::function<bool(QString *)> &)> hook)
    {
        m_testMergeConflictEditorHook = std::move(hook);
    }
    void testReloadWorktreesPanel() { loadWorktreesPanel(); }
    QString testWorktreePathForBranch(const QString &repoPath,
                                      const QString &branch) const
    {
        return worktreePathForBranch(repoPath, branch);
    }
    void testSetWorktreeListCompletedHook(std::function<void()> hook)
    {
        m_testWorktreeListCompletedHook = std::move(hook);
    }
    QString testAgentCommandApproval(int sessionId, const QString &command,
                                     const QString &cwd);
    QString testHighMemoryPanelActivation(bool activate);
    QString testSelectedWorktreeBranch() const { return m_worktreeSelectedBranch; }
    QString testWorktreeBranchLabel() const;
    QString testWorktreeAheadBehindText(const QString &branch) const;
    QStringList testWorktreeBranches() const;
    QString testArrowOnWorktrees(bool down);
    int testWorktreesTabIndex() const { return m_worktreesTabIndex; }
    void testClickRepoDetailTab(int id);
    bool testRepoDetailTabDeferred(int id) const;
    bool testOpenMostRecentCommit();
    bool testWorktreesTableHasKeyboardFocus() const;
    int testReleasesTabIndex() const { return m_releasesTabIndex; }
    int testRetiredMirrorNodesDetailTab() const { return kRetiredMirrorNodesDetailTab; }
    bool testRepoDetailTabButtonExists(int id) const;
    void testShowMirrorNodesOnNetwork() { showMirrorNodesOnNetwork(m_repoDetailIndex); }
    void testApplyRetiredMirrorNodesNav()
    {
        NavPlace place;
        place.section = 0;
        place.repoIndex = m_repoDetailIndex;
        place.detailTab = kRetiredMirrorNodesDetailTab;
        applyNavDetailTab(place);
    }
    bool testNetworkNodesTabShowing() const;
    QStringList testMirrorNodeHeaderCaptions() const;
    QStringList testNodeDirectoryHeaderCaptions() const;
    QString testMirrorNodeCellByCaption(const QString &nodeName,
                                        const QString &caption) const;
    bool testMirrorSectionOnNodesPage() const;
    void testSetNodesRoleFilter(const QString &roleKey);
    QString testNodesRoleFilterKey() const;
    QStringList testNodesRoleFilterKeys() const;
    void testSetRelayOnlineNodes(const QStringList &nodes);
    void testSetVultrFleetJson(const QByteArray &instancesJson);
    void testSetRunnersPayload(const QJsonObject &payload);
    QPushButton *testRunnerAttentionButton() const
    {
        return m_runnerAttentionButton;
    }
    QStringList testRelayRowNames() const;
    void testSetRelayProbe(const QString &relayName, const QString &state,
                           int latencyMs, const QString &version,
                           const QString &failure);
    QString testNodeDirectoryCellToolTip(const QString &node,
                                         const QString &columnCaption) const;
    void testActivateNodeDirectoryAction(const QString &node);
    QStringList testNodeLogFilterNodes() const;
    void testClearNodeLogFilter();
    bool testLogSectionShowing() const;
    QString testNodesStatusText() const;
    void testShowLegacyRelaysSection() { showSection(8); }
    int testVisibleNodeDirectoryRows() const;
    int testVisibleMirrorNodeRows() const;
    void testSetMirrorSectionCatalog(const QString &source, const QByteArray &body);
    QString testMirrorNodesRepoSource() const;
    int testControlNodeSectionIndex() const { return kControlNodeSectionIndex; }
    void testShowControlNode() { showSection(kControlNodeSectionIndex); }
    int testOrganizationTasksSectionIndex() const
    {
        return kOrganizationTasksSectionIndex;
    }
    void testShowOrganizationTasks()
    {
        showSection(kOrganizationTasksSectionIndex);
    }
    void testApplyOrganizationTasks(const QJsonObject &payload)
    {
        applyOrganizationTasks(payload);
    }
    QPixmap testOrganizationTaskAgentFace(const QString &taskId) const;
    void testSetAccountAvatar(const QString &account, const QPixmap &picture);
    QPixmap testOrganizationTaskCreatorPicture(const QString &taskId) const;
    QPixmap testOrganizationTaskAssigneePicture(const QString &taskId) const;
    void testSetOrganizationTaskListView(forkmesh::ui::ListViewMode view)
    {
        setOrganizationTaskListView(view);
    }
    bool testOrganizationTaskAgentWorking(const QString &taskId) const;
    bool testOrganizationTaskSpinTimerActive() const;
    QImage testOrganizationTaskRowImage(const QString &taskId) const;
    forkmesh::ui::ListViewMode testOrganizationTaskListView() const
    {
        return m_organizationTaskListView;
    }
    void testReloadOrganizationTaskListView()
    {
        setOrganizationTaskListView(forkmesh::ui::loadListViewMode(
            QStringLiteral("tasks/listView"), forkmesh::ui::ListViewMode::Full));
    }
    void testRefreshOrganizationTaskAgentRows()
    {
        refreshOrganizationTaskAgentRows();
    }
    void testTickOrganizationTaskSpinners() { tickOrganizationTaskSpinners(); }
    bool testReleasesTableHasKeyboardFocus() const;
    bool testMirrorNodesTableHasKeyboardFocus() const;
    Q_INVOKABLE QStringList testMirrorNodeRows() const;
    bool testMirrorNodesOnlineOnlyChecked() const;
    void testSetMirrorNodesOnlineOnly(bool checked);
    bool testMirrorNodesAreSingleLineRows() const;
    bool testMirrorNodeColumnsDraggable() const;
    bool testMirrorNodeHeaderHasOptionsButton() const;
    bool testMirrorNodeHeaderIsCompactAndVisible() const;
    QString testMirrorRoutingStateHash(const QString &mirrorPath) const
    {
        return mirrorRoutingStateHash(mirrorPath);
    }
    void testSetSshMirrorMetadata(const QString &source,
                                  const QJsonObject &metadata);
    QString testMirrorNodeCellText(const QString &nodeName, int column) const;
    QString testMirrorNodeCellToolTip(const QString &nodeName, int column) const;
    bool testDraftMirrorNodeDiagnostics(const QString &nodeName);
    QString testFleetBinaryInstallRemoteCommand(bool reinstall,
                                                qsizetype *uploadByteCount,
                                                QString *errorOut);
    QString testPublishedReleaseInstallRemoteCommand(QString *errorOut);
    QString testDirectBinaryInstallRemoteCommand(qsizetype *uploadByteCount,
                                                 QString *errorOut);
    QString testVultrGoMirrorInstallRemoteCommand(qsizetype *uploadByteCount,
                                                  QString *errorOut);
    void testReloadBranchesPanel();
    QString testBranchWorktreePath(const QString &branch) const;
    QString testBranchVisualBadges(const QString &branch) const;
    QString testBranchHealthIcon(const QString &branch) const;
    QString testBranchLinks(const QString &branch) const;
    bool testBranchesShowSortableColumns() const;
    bool testBranchesKeepFlexibleNameColumn() const;
    bool testBranchDelegatePaintsSingleTextLayer(const QString &branch) const;
    bool testBranchSelectedTextColorIsReadable(const QString &branch) const;
    void testAddAgentSession(const AgentSession &session)
    {
        if (m_agentStore)
            m_agentStore->saveSession(session);
        for (AgentSession &existing : m_agentSessions) {
            if (existing.id == session.id) {
                existing = session;
                return;
            }
        }
        m_agentSessions.append(session);
    }
    void testRefreshAgentQueueControls() { refreshAgentQueueControls(); }
    int testActiveAgentSessionId() const { return activeAgentSessionId(); }
    int testActiveAgentSessionIdWithQueued(int sessionId)
    {
        m_agentQueue.append(sessionId);
        const int active = activeAgentSessionId();
        m_agentQueue.removeAll(sessionId);
        return active;
    }
    void testShowAgentQueueLimitMenu() { showAgentQueueLimitMenu(); }
    void testQueueAgentSession(int sessionId)
    {
        if (!m_agentQueue.contains(sessionId))
            m_agentQueue.append(sessionId);
    }
    bool testMoveQueuedAgentSession(int sessionId, int to)
    {
        return moveQueuedAgentSession(sessionId, to);
    }
    QJsonObject testAgentQueuePayload() { return agentQueuePayload(); }
    QJsonObject testLocalControlAgentsPayload() { return localControlAgentsPayload(); }
    void testUpdateAgentActionState() { updateAgentActionState(); }
    void testSetActionRunsRestoreWorkerHook(std::function<void()> hook)
    {
        m_testActionRunsRestoreWorkerHook = hook;
    }
    bool testActionRunsRestoreInFlight() const
    {
        return m_actionRunsRestoreInFlight;
    }
    bool testActionRunsRestored() const { return m_actionRunsRestored; }
    void testQueueActionWorkflowScanDuringRestore();
    int testDeferredActionWorkflowScanCount() const;
    // Watching several runs at once: seed a history, then drive the runs table
    // the way a ctrl-click or the Watch live button does.
    void testSeedActionRuns(const QList<ActionRun> &runs);
    void testSelectActionRunRows(const QList<int> &rows);
    // Persists a cancelled run through the normal Actions store and checks
    // whether the current cleanup preference removes it immediately.
    bool testCancelledActionRunCleanup(bool expectDeleted);
    void testWatchAllLiveRuns() { watchAllLiveRuns(); }
    void testCloseParallelRun(int runId) { closeParallelRun(runId); }
    void testAppendRunLog(int runId, const QString &text)
    {
        onRunLog(runId, text);
    }
    QList<int> testOpenActionRunIds() const { return openActionRunIds(); }
    int testParallelRunColumnCount() const
    {
        return m_parallelRunColumns.size();
    }
    // The controls a watched run's column offers, in row order.
    QStringList testParallelRunControls(int runId) const;
    QStringList testActionRunControls() const;
    bool testActionRunSpinning(int runId) const;
    bool testActionRunControlsUnderTitle(int runId) const;
    QString testActionRunLogText(int runId) const;
    int testSelectedActionRunId() const { return m_selectedRunId; }
    // What "Send to agent" hands over for the selected run: the log's path on
    // this machine, and the ask to fix this run and the recent CI tests. The
    // Pulls page carries a button of the same name, so the offer is read off
    // this widget rather than searched for by label.
    QString testActionRunAgentPrompt(const ActionRun &run) const
    {
        return actionRunAgentPrompt(run);
    }
    bool testActionSendToAgentOffered() const;
    // The log location line under the selected run's meta row, as shown.
    QString testActionRunLogLocationText() const;
    void testRefreshAgentDiffStats()
    {
        m_agentDiffCooldownWaived = true;
        m_agentDiffRefreshPending = true;
        refreshAgentTable();
    }
    void testScheduleAgentLiveDiffRefresh(int sessionId)
    {
        scheduleAgentLiveDiffRefresh(sessionId);
    }
    int testAgentDiffPassCount() const { return m_testAgentDiffPassCount; }
    bool testAgentDiffStatsRefreshing() const
    {
        return m_agentDiffStatsRefreshing;
    }
    int testAgentSessionForPullId(int prNumber, const QString &headBranch) const
    {
        const AgentSession *session = agentSessionForPull(prNumber, headBranch);
        return session ? session->id : 0;
    }
    QString testResolvablePullHead(PullRequest pr) const
    {
        return resolvablePullHead(std::move(pr));
    }
    QString testLocalPullHeadForDeletion(PullRequest pr) const
    {
        return localPullHeadForDeletion(std::move(pr));
    }
    PullRequest testPullRecordForDeletion(int number) const
    {
        return pullRecordForDeletion(number);
    }
    void testSeedPullActivityCommits(int prNumber,
                                     const QList<PullActivityCommit> &commits)
    {
        m_pullActivityCommitsNumber = prNumber;
        m_pullActivityCommits = commits;
    }
    void testRenderPullThread(const PullRequest &pr) { renderPullThread(pr); }
    QStringList testPullThreadCardHeaders() const;
    int testPullThreadUnmanagedVisibleRows() const;
    // Numbers of the rows refreshPullList() puts on screen for `pulls`, with
    // `viewing` standing in for the pull the detail pane is showing.
    QList<int> testPullListRowNumbers(const QList<PullRequest> &pulls,
                                      int viewing);
    QString testPullDisplayStatus(const PullRequest &pull) const;
    // Rows on screen after a snapshot that was applied once while the Pull
    // requests tab was still deferred is applied again to the table the tab
    // built — the shape of a user landing on the tab.
    int testPullRowsAfterRepeatSnapshot(const QList<PullRequest> &pulls);
    // Whether the list's spinner row is up while a load runs with nothing on
    // screen, and down once that load's rows arrive.
    bool testPullListShowsLoaderWhileLoading(const QList<PullRequest> &pulls);
    // What the leading column paints about pull `number`'s head branch, as
    // "conflict ahead behind dirty worktree". Empty when no row shows it.
    QString testPullListBranchMeta(int number) const;
    QString testPullListRowTooltip(int number) const;
    QString testPullConflictNoticeBody(const PullRequest &pull,
                                       const QStringList &conflictFiles) const
    {
        return pullConflictNoticeBody(pull, conflictFiles);
    }
    bool testPullHasConflictNotice(const PullRequest &pull) const
    {
        return pullHasConflictNotice(pull);
    }
    // The muted composer label while this pull is selected in the PR detail.
    // Keeps the selected-pull routing test on the user-visible detail path.
    QString testQuickAddPlaceholderForPull(const PullRequest &pull);
    int testPullActivityExtraCards() const { return m_pullActivityExtraCards; }
    // The task the "Fix" button hands an agent for `pull`, and the agent+model
    // rows the combo beside it offers (label, then "provider|model").
    QString testPullFixAgentPrompt(const PullRequest &pull,
                                   const QString &checkoutBranch = QString()) const
    {
        return pullFixAgentPrompt(pull, checkoutBranch);
    }
    QString testPullAgentBranch(const PullRequest &pull) const
    {
        return pullAgentBranch(pull);
    }
    QStringList testPullFixModelRows() const;
    bool testSelectPullFixModel(const QString &provider, const QString &model);
    QString testPullFixProvider() const { return pullFixProvider(); }
    QString testPullFixModel() const { return pullFixModel(); }
    bool testBindAgentSessionsToPull(int prNumber, const QString &headBranch)
    {
        return bindAgentSessionsToPull(prNumber, headBranch);
    }
    int testAgentSessionPullNumber(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.prNumber;
        return 0;
    }
    QString testAgentPrButtonText(int sessionId);
    QString testAgentIssueButtonText(int sessionId);
    QString testAgentIssueButtonToolTip(int sessionId);
    // Clicks the agent detail's link back to the issue it was started from;
    // false when that session shows no such link.
    bool testClickAgentIssueButton(int sessionId);
    int testCurrentIssueNumber() const { return m_currentIssueNumber; }
    QString testAgentMetaHtml(int sessionId);
    QString testCurrentAgentMetaHtml() const;
    void testSetAgentSessionProcessId(int sessionId, qint64 processId);
    int testAgentProcessMetaScanLaunches() const;
    bool testAgentProcessMetaScanInFlight() const;
    void testExpireAgentProcessMetaCache();
    void testRefreshAgentDetailMeta(int sessionId, bool forceProcessRefresh)
    {
        refreshAgentDetailMeta(sessionId, forceProcessRefresh);
    }
    void testOpenAgentsOverview() { openAgentsOverview(); }
    void testApplyTranscriptEvent(int sessionId, const QJsonObject &ev)
    {
        applyTranscriptEvent(sessionId, ev);
    }
    void testSetStreamWorktree(int sessionId, const QString &path)
    {
        m_streamWorktree.insert(sessionId, path);
    }
    void testShowAgentSession(int sessionId) { showAgentSession(sessionId); }
    QTextBrowser *testAgentDiffView() const;
    QString testAgentFilesSummary() const;
    AgentToolEfficiencyLedger *testAgentToolLedger() const
    {
        return m_agentToolLedger.get();
    }
    AgentToolEfficiencyBoard *testAgentEfficiencyBoard() const
    {
        return m_agentEfficiencyBoard;
    }
    QStackedWidget *testAgentPageStack() const { return m_agentPageStack; }
    AgentModelTable *testAgentModelTable() const { return m_agentModelTable; }
    bool testAgentModelGitScanning() const { return m_agentModelGitScanning; }
    // View modes (AppViewMode.h): the switcher button, the surfaces a mode
    // hides, and the Workspace terminal pane.
    forkmesh::ui::AppViewMode testAppViewMode() const { return m_viewMode; }
    void testSetAppViewMode(forkmesh::ui::AppViewMode mode)
    {
        setAppViewMode(mode);
    }
    void testCycleAppViewMode() { cycleAppViewMode(); }
    QPushButton *testViewModeButton() const { return m_viewModeButton; }
    QWidget *testAppNavigationRail() const { return m_appNavigationRail; }
    QWidget *testAppStatusArea() const { return m_appStatusArea; }
    QWidget *testWorkspaceTerminalDock() const { return m_workspaceTerminalDock; }
    QSplitter *testAgentWorkspaceSplit() const { return m_agentWorkspaceSplit; }
    QWidget *testWorkspaceTerminal() const;
    AgentToolImprovementLog *testAgentToolImprovements() const
    {
        return m_agentToolImprovements.get();
    }
    AgentRunEfficiencyLog *testAgentRunLog() const { return m_agentRunLog.get(); }
    AgentRunTable *testAgentRunTable() const { return m_agentRunTable; }
    QWidget *testAgentEfficiencyDetailsPanel() const
    {
        return m_agentEfficiencyDetailsPanel;
    }
    QTextBrowser *testAgentEfficiencyDetails() const
    {
        return m_agentEfficiencyDetails;
    }
    void testRefreshAgentEfficiencyBoard() { refreshAgentEfficiencyBoard(); }
    QString testAgentToolImprovementsFixPrompt() const
    {
        return agentToolImprovementsFixPrompt();
    }
    AgentGuardrailLedger *testAgentGuardrailLedger() const
    {
        return m_agentGuardrailLedger.get();
    }
    AgentGuardrailBoard *testAgentGuardrailBoard() const
    {
        return m_agentGuardrailBoard;
    }
    void testRefreshAgentGuardrailBoard() { refreshAgentGuardrailBoard(); }
    QStringList testDisabledAgentToolFamilies() const
    {
        return disabledAgentToolFamilies();
    }
    void testSetAgentToolFamilyEnabled(const QString &family, bool enabled)
    {
        setAgentToolFamilyEnabled(family, enabled);
    }
    QJsonObject testAgentRuntimeLaunchOptions() const
    {
        return agentRuntimeLaunchOptions();
    }
    void testRefreshAgentDotMatrix() { refreshAgentDotMatrix(); }
    void testAnimateRunningAgentIcons() { animateRunningAgentIcons(); }
    qreal testAgentSpinAngle(int sessionId)
    {
        const AgentSession *session = findAgentSession(sessionId);
        return session ? agentSpinAngle(*session) : -1.0;
    }
    void testNoteAgentActivity(int sessionId, int bytes)
    {
        noteAgentActivity(sessionId, bytes);
    }
    int testAgentDotCount() const;
    void testSetAgentSessionStatus(int sessionId, const QString &status);
    QString testAgentSessionStatus(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.status;
        return {};
    }
    void testSetRepoAgentGuardrails(int repositoryIndex, bool enabled,
                                    const QStringList &directories)
    {
        if (repositoryIndex < 0 || repositoryIndex >= m_repositories.size())
            return;
        RepositoryRecord &repository = m_repositories[repositoryIndex];
        repository.agentGuardrailsEnabled = enabled;
        repository.agentAllowedDirectories =
            forkmesh::agentguard::normalizeDirectories(directories);
        saveRepositories();
    }
    QStringList testRepoAgentAllowedDirectories(int repositoryIndex) const
    {
        return repositoryIndex < 0 || repositoryIndex >= m_repositories.size()
                   ? QStringList()
                   : m_repositories.at(repositoryIndex).agentAllowedDirectories;
    }
    void testAuditAgentGuardrailEvent(
        int sessionId, const QString &worktree, const QStringList &allowed,
        const QJsonObject &event)
    {
        m_streamWorktree[sessionId] = worktree;
        forkmesh::agentguard::Config guardrails;
        guardrails.enabled = true;
        guardrails.allowedDirectories = allowed;
        auditAgentGuardrailEvent(sessionId, event, guardrails);
    }
    void testRemoveAgentSession(int sessionId);
    // "<id>:<status>" per asked-for session, plus ":q<position>" when it is
    // waiting in the run queue — how a test reads which agents a lowered
    // concurrent-agent limit parked, and in what order they resume.
    QString testAgentQueueSnapshot(const QList<int> &sessionIds) const;
    QString testAgentIntakeSessionState(int sessionId) const
    {
        return agentIntakeSessionState(sessionId);
    }
    QString testPromptAgentSessionState(int sessionId, const QString &prompt)
    {
        sendPromptToAgentSession(sessionId, prompt);
        return agentIntakeSessionState(sessionId);
    }
    void testSeedResumeConversationId(int sessionId, const QString &conversationId,
                                      bool codex)
    {
        m_streamEvents[sessionId].append(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("system")},
            {QStringLiteral("subtype"), QStringLiteral("init")},
            {codex ? QStringLiteral("thread_id") : QStringLiteral("session_id"),
             conversationId}});
    }
    void testSetAgentTranscriptEvents(int sessionId,
                                      const QList<QJsonObject> &events)
    {
        m_streamEvents[sessionId] = events;
        m_streamEventsLoading.remove(sessionId);
        m_streamEventsAbsent.remove(sessionId);
    }
    void testApplyAgentTranscriptEvent(int sessionId, const QJsonObject &event)
    {
        applyTranscriptEvent(sessionId, event);
    }
    void testFinishCliTranscriptRun(int sessionId, bool codex, int exitCode)
    {
        finishCliTranscriptRun(sessionId, codex, exitCode);
    }
    void testSetClosingDown(bool closing) { m_closingDown = closing; }
    bool testAgentQueued(int sessionId) const { return m_agentQueue.contains(sessionId); }
    QString testReplayCliExitsWithoutResult(int sessionId, bool codex, int times)
    {
        QString outcomes;
        for (int i = 0; i < times; ++i) {
            if (AgentSession *session = findAgentSession(sessionId))
                session->status = AgentStatus::Running;
            outcomes += applyCliExitWithoutResult(sessionId, codex, /*exitCode=*/1)
                            ? QLatin1Char('q')
                            : QLatin1Char('f');
            m_agentQueue.removeAll(sessionId);
        }
        return outcomes;
    }
    void testQueueAgentSteerMessage(int sessionId, const QString &prompt)
    {
        queueAgentSteerMessage(sessionId, prompt);
    }
    QString testPendingSteerMessage(int sessionId) const
    {
        return m_pendingSteerMessage.value(sessionId);
    }
    QString testTakeSteerMessageForLaunch(int sessionId)
    {
        return takeAgentSteerMessageForLaunch(sessionId);
    }
    QString testAgentSessionLastError(int sessionId)
    {
        const AgentSession *session = findAgentSession(sessionId);
        return session ? session->lastError : QString();
    }
    void testFailCliTranscriptLaunch(int sessionId, const QString &why)
    {
        failCliTranscriptLaunch(sessionId, why);
    }
    void testMarkAgentSessionRunning(int sessionId)
    {
        markAgentSessionRunning(sessionId);
    }
    bool testQueueDetachedRunningAgentSession(int sessionId)
    {
        continueAgentSession(sessionId, /*deferRefresh=*/true);
        const AgentSession *session = findAgentSession(sessionId);
        const bool queued = session && session->status == AgentStatus::Queued &&
                            m_agentQueue.contains(sessionId);
        m_agentQueue.removeAll(sessionId);
        return queued;
    }
    // One poll of the post-`result` drain against a real process tree rooted at
    // rootPid: returns "status|list label|pill text|transcript note" so a test
    // can see whether a session with live background work still claims to be
    // done. The pending check is dropped again, so the queued next poll no-ops.
    QString testPollAgentBackgroundWork(int sessionId, qint64 rootPid);
    // The conflict alert on an agent row, as clicked: returns the prompt it
    // parked for the session (empty when the click went nowhere).
    QString testClickAgentConflictIcon(int sessionId)
    {
        fixAgentConflictsWithAgent(sessionId);
        const QString parked = m_pendingSteerMessage.value(sessionId);
        m_agentQueue.removeAll(sessionId);
        return parked;
    }
#ifdef FORKMESH_WINDOW_TESTS
    void testAgentNeedsAttention(int sessionId, const QString &message)
    {
        onAgentNeedsAttention(sessionId, message);
    }
    void testAnswerAgentQuestion(int sessionId, const QString &toolUseId,
                                 const QString &answer, bool sensitive = false)
    {
        answerAgentQuestion(sessionId, toolUseId, answer, sensitive,
                            /*deferRefresh=*/true);
    }
    // Stands in for a CLI process that is still up. agentSessionHasLiveTransport()
    // reads this set, which is the only way a test can reach the state where a
    // click has to be routed to a running agent rather than to a restart: a real
    // live transport means really launching an agent CLI.
    void testSetAgentSessionLiveTransport(int sessionId, bool live)
    {
        if (live)
            m_testLiveTransportSessions.insert(sessionId);
        else
            m_testLiveTransportSessions.remove(sessionId);
    }
    // The quick-add "add" button pressed on an empty composer, which is the
    // "continue where you left off" click: returns the prompt the session
    // delivered to a stubbed live transport or parked for a restart.
    QString testClickQuickAddContinue(int sessionId);
    // Sends the same follow-up while the previous clean result is still
    // draining a child process. Returns prompt|status|watcher|background-state.
    QString testContinueAgentDuringBackgroundDrain(int sessionId);
    // The same click with nothing selected above, so the target can only come
    // from what is on screen — the branch diff's agent. Reads the nudge back off
    // the session it was expected to reach.
    QString testClickQuickAddContinueOnScreen(int expectSessionId);
    void testSendQuickAddFollowUp(const QString &text);
    QString testApplyErroredAgentResult(int sessionId)
    {
        m_testLiveTransportSessions.insert(sessionId);
        applyTranscriptEvent(
            sessionId,
            QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                        {QStringLiteral("is_error"), true},
                        {QStringLiteral("result"),
                         QStringLiteral("Test agent failure")}});
        const AgentSession *session = findAgentSession(sessionId);
        return QStringLiteral("%1|%2")
            .arg(session ? session->status : QStringLiteral("missing"))
            .arg(agentSessionHasLiveTransport(sessionId) ? 1 : 0);
    }
#endif
    QString testAgentSessionModel(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.model;
        return QString();
    }
    QString testAgentSessionProvider(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.provider;
        return QString();
    }
    QString testAgentSessionHandoffProvider(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.handoffFromProvider;
        return QString();
    }
    QString testAgentSessionRuntimeId(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.effectiveRuntimeId();
        return QString();
    }
    QString testAgentTerminalHandoffBlocker(int sessionId);
    QString testAgentSessionMode(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.mode;
        return QString();
    }
    QString testAgentSessionBaseBranch(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.baseBranch;
        return QString();
    }
    QString testAgentSessionBaseRef(int sessionId) const
    {
        for (const AgentSession &session : m_agentSessions)
            if (session.id == sessionId)
                return session.baseRef;
        return QString();
    }
    void testTypeGlobalSearch(const QString &text);
    void testOpenSearchResultsPage(const QString &query)
    {
        openSearchResultsPage(query);
    }
    bool testSearchPageRunning() const { return m_searchPending > 0; }
    void testCheckSavedSearchAlerts() { checkSavedSearchAlerts(); }
    void testSetSearchHiddenTerms(const QStringList &terms) { setSearchHiddenTerms(terms); }
    void testSetSearchHidesVariables(bool hide) { setSearchHidesVariables(hide); }
    void testRefreshAllSavedSearches() { refreshAllSavedSearches(); }
    void testLogSystemLine(const QString &text) { logSystem(text); }
    QStringList testNetworkLogTail(int count) const { return m_networkLog.mid(qMax(0, int(m_networkLog.size()) - count)); }
    void testSetSavedSearchExclusions(const QString &id, const QStringList &excluded)
    {
        setSavedSearchExclusions(id, excluded);
    }
    bool testSavedSearchAlertsIdle() const
    {
        return !m_savedSearchCheckRunning && m_savedSearchCheckQueue.isEmpty();
    }
    QString testSavedSearchPath() { return savedSearchStore().path(); }
    QVector<SecurityAlarm> testSecurityAlarms() const
    {
        return m_securityAlarmState.active();
    }
    bool testSecurityAlarmBannerVisible() const;
    bool testSecurityAlarmToneActive() const
    {
        return m_securityAlarmTone && m_securityAlarmTone->isActive();
    }
    QString testAgentSearchText() const;
    QString testTranscriptSearchText() const;
    bool testGlobalSearchClearVisible() const;
    void testTriggerGlobalSearchClear();
    // The "No results for … (clear search)" notice: its text while shown, empty
    // while the list itself is on screen.
    QString testAgentSearchEmptyText() const;
    void testActivateAgentSearchEmptyLink();
    void testAppendAgentTranscript(int sessionId, const QString &text)
    {
        if (const AgentSession *session = findAgentSession(sessionId))
            if (m_agentStore)
                m_agentStore->appendLog(*session, text);
    }
    void testRunAgentTranscriptSearch() { runAgentTranscriptSearch(); }
    void testScanAgentSessionImages()
    {
        m_agentImageScanPending = true;
        refreshAgentTable();
    }
    void testRefreshAgentTable() { refreshAgentTable(); }
    QStringList testAgentRowTitles() const;
    QString testCreateAgentFolder(const QString &name,
                                  const QList<int> &sessionIds)
    {
        return createAgentFolder(name, sessionIds);
    }
    QStringList testAgentListLayout() const;
    void testToggleAgentFolder(const QString &folderId)
    {
        toggleAgentFolder(folderId);
    }
    void testDropAgentSessions(const QList<int> &sessionIds, int targetSessionId,
                               const QString &folderId, bool after)
    {
        dropAgentSessions(sessionIds, targetSessionId, folderId, after);
    }
    void testDropAgentFolder(const QString &folderId,
                             const QString &targetFolderId, bool after)
    {
        dropAgentFolder(folderId, targetFolderId, after);
    }
    QStringList testAgentFolderMenuActions(const QString &folderId);
    bool testTriggerAgentFolderMenuAction(const QString &folderId,
                                          const QString &objectName);
    QStringList testAgentRowFolderMenuActions(int sessionId);
    bool testTriggerAgentRowFolderMenuAction(int sessionId,
                                             const QString &objectName);
    bool testClickAgentFolderRow(const QString &folderId);
    QImage testGrabAgentFolderRow(const QString &folderId);
    QStringList testAgentRowChipWords(int sessionId) const;
    bool testAgentLeadColumnHidden() const;
    QImage testGrabAgentTitleRow(int sessionId);
    QStringList testAgentRowImages(int sessionId) const;
    bool testAgentRowHasThumbnail(int sessionId) const;
    bool testBranchAttachmentHasIcon(const QString &branch) const;
    QStringList testBranchRowOrder() const;
    int testBranchesTabIndex() const { return m_branchesTabIndex; }
    int testOverviewBodyPage() const;
    QStringList testOverviewRowNames() const; // Code > Files rows, in view order
    bool testClickBranchRowInOverview(const QString &branch);
    bool testBranchesPanelOwnsDiffView() const;
    bool testGitWorkspaceIsExclusive() const;
    bool testGitPromptFloatsBottomLeft() const;
    QString testSwitchToBranchImmediateSelection(const QString &branch);
    QString testSwitchToWorktreeGitBranch(const QString &branch)
    {
        switchToWorktree(branch);
        return m_repoBranch;
    }
    QString testBrowsedBranch() const { return m_repoBranch; }
    void testSwitchToAgentBranch(int sessionId) { switchToAgentBranch(sessionId); }
    int testCommitWorkspacePage() const;
    QString testBranchDiffBranch() const { return m_branchDiffBranch; }
    QString testBranchDiffDragComment(const QString &path, int fromLine,
                                      int toLine);
    bool testSubmitBranchInlineComment(const QString &body, QString *error);
    QList<PullEvent> testBranchReviewEvents(const QString &branch) const;
    // The diff toolbar's link back to the agent that wrote the branch: its
    // caption when it is offered, empty when the branch belongs to no agent.
    QString testBranchAgentButtonText() const;
    bool testBranchAgentButtonUsesStatusIcon() const;
    bool testClickBranchAgentButton();
    // Which conversation the composer's "add" is pointed at right now.
    int testQuickAddFollowUpAgent() const
    {
        return quickAddFollowUpAgentSessionId();
    }
    QString testQuickAddPlaceholder() const;
    QStringList testBranchDiffFiles() const { return m_branchDiffFilePaths; }
    QString testBranchDiffText() const;
    QString testBranchDiffLogText() const;
    QString testBranchDiffLogButtonText() const;
    bool testBranchDiffLogPanelShown() const;
    void testToggleBranchDiffLog();
    bool testBranchDiffCached(const QString &branch) const;
    bool testBranchDiffPaintedFromCache() const
    {
        return m_branchDiffPaintedFromCache;
    }
    int testBranchScopeDiffReads() const { return m_branchScopeDiffGen; }
    QString testBranchesCurrentRowName() const;
    bool testBranchesPanelLoading() const
    {
        return m_branchesPanelLoading || m_branchesPanelReloadQueued;
    }
    QStringList testSourceControlPaths() const;
    QString testSourceControlDiffText() const;
    QString testScmCommitControlsState() const;
    QString testSourceControlGitDir() const { return sourceControlGitDir(); }
    // Drives the context menu's file revert without its confirmation prompt.
    QString testScmRevertPathToBase(const QString &path)
    {
        QString summary;
        const bool ok = scmRevertPathToBase(path, &summary);
        return (ok ? QStringLiteral("ok: ") : QStringLiteral("error: ")) + summary;
    }
    void testRefreshSourceControl() { refreshSourceControl(/*force=*/true); }
    bool testClickSourceControlPath(const QString &path);
    QString testArrowOnSourceControl(bool down);
    QString testBranchStickyHeaderText() const;
    bool testScrollBranchDiffViewportToFile(const QString &path);
    QString testLastSourceControlDiffPath() const
    {
        return m_lastSourceControlDiffPath;
    }
    QString testScmStrokedRowFile() const;
    QString testBranchOutlinedFile() const;
    QRect testBranchOutlineRect() const;
    QRect testBranchActiveRowRect() const;
    QString testBranchDiffSummaryText() const;
    QString testScmOutlinedFile() const;
    QRect testScmOutlineRect() const;
    bool testScmDiffUsesFullSurface() const;
    bool testScmDiffFilePinnedToTop(const QString &path) const;
    int testGitFilesSlotPage() const;
    int testGitHistorySlotPage() const;
    bool testGitMergedCoverShown() const { return gitMergedCoverShown(); }
    QString testGitMergedCoverText() const;
    QString testGitMergedStatsText() const;
    bool testClickGitMergedCoverButton(bool branches);
    void testShowGitMergedCover(const QString &branch, const QString &base)
    {
        showGitMergedCover(branch, base);
    }
    bool testClickGitLeaderboardButton();
    bool testScmAutoViewedRoundTrip();
    // Ticks Viewed on `path` through the pane's own anchor handler and then
    // unticks it, reporting what that cost: how many full re-renders it took,
    // where the scroll bar stood before and after each click, and whether
    // `bodyText` (a line of that file's diff) actually folded away and came
    // back. Formatted as
    // "renders=<n> scroll=<before>/<afterFold>/<afterReopen> folded=<0|1> reopened=<0|1>".
    QString testScmViewedClickKeepsViewport(const QString &path,
                                            const QString &bodyText);
    QString testScmNextFileBarText(int scrollTo);
    QString testScmStickyControlsHtml(int scrollTo);
    // Marks `path` viewed against `patch`, then re-reads the review state
    // against `next`. True when the mark stood through the second read.
    bool testDiffViewedSurvivesPatch(const QString &context, const QString &path,
                                     const QString &patch, const QString &next);
    QString testCompareIndicatorText() const;
    QString testComparedBranchText() const;
    void testSetCompareBase(const QString &base);
    void testCloseBranchRange();
    void testClickRailGitButton();
    void testRefreshRepoChangeBadge() { refreshRepoChangeBadge(); }
    int testGitChangeBadgeCount() const;
    int testGitPendingSyncCount() const;
    void testNavigateBack() { navigateBack(); }
    void testNavigateForward() { navigateForward(); }
    QString testNavBackToolTip() const;
    QString testNavForwardToolTip() const;
    QStringList testNavHistoryMenuEntries(bool forward);
    void testNavHistoryMenuJump(bool forward, int row);
    // The trail menu runs its own event loop, so a test drives the button the
    // way a mouse does instead: a plain click steps one place, while the click
    // that ends a press-and-hold must leave the trail where it stands.
    void testClickNavButton(bool forward, bool afterLongPress = false);
    void testOpenRepoFile(const QString &path)
    {
        openRepoFile(path);
        QElapsedTimer waited;
        waited.start();
        while ((!m_repoFileOpenWaiters.isEmpty() || m_repoFileTreeLoading) &&
               waited.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::processEvents();
    }
    QString testOpenRepoFilePath() const { return openRepoFilePath(); }
    void testOpenAgentTranscriptReference(const QString &href)
    {
        openAgentTranscriptReference(href);
    }
    static QStringList testTranscriptFilePathMatches(const QString &repoDir,
                                                     const QString &ref,
                                                     const QString &path)
    {
        return transcriptFilePathMatches(repoDir, ref, path);
    }
    void testOpenRepoDirectory(const QString &path) { loadRepoOverview(path); }
    bool testRepoOverviewLoading() const { return m_overviewLoading; }
    bool testCommitsLoading() const { return m_commitsLoading; }
    QString testOverviewDirectory() const { return m_overviewPath; }
    int testFilesStackPage() const;
    bool testFilesSectionShowing() const;
    QString testOpenCommitHash() const;
    void testShowCommit(const QString &hash) { showCommit(hash); }
    bool testClickBranchReviewMerge(bool deleteAll);
    bool testWorktreeMergeInProgress() const { return m_worktreeMergeInProgress; }
    bool testBranchCleanupInProgress(const QString &branch) const
    {
        return m_branchCleanupInProgress.contains(branch);
    }
    static void testSpinWhile(const std::function<bool()> &busy)
    {
        QElapsedTimer waited;
        waited.start();
        while (busy() && waited.elapsed() < 30000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    void testPullBaseIntoAllBranches()
    {
        pullBaseIntoAllBranches();
        testSpinWhile([this] { return m_pullAllBranchesRunning; });
    }
    bool testBranchMergeFlowBusy() const { return m_branchMergeFlowBusy; }
    bool testUpdateAllAgentWorktreesRunning() const
    {
        return m_updateAllAgentWorktreesRunning;
    }
    bool testBranchPullEnabled() const;
    bool testClickBranchPull();
    void testRefreshBranchLockState() { refreshBranchLockState(); }
    bool testBranchClearLockVisible() const;
    QString testBranchClearLockTooltip() const;
    bool testClickBranchClearLock();
    bool testMarkAgentBranchMerged(const QString &branch, bool mergeVerified = false)
    {
        return markAgentSessionsMerged(0, branch, mergeVerified);
    }
    bool testMergeBranchAndCleanUp(const QString &branch)
    {
        struct MergeWait {
            bool finished = false;
            bool merged = false;
        };
        const auto state = std::make_shared<MergeWait>();
        mergeWorktreeIntoMain(branch,
                              worktreePathForBranch(repoGitDir(), branch,
                                                    WorktreeLookup::Any),
                              true, [state](bool ok) {
                                  state->finished = true;
                                  state->merged = ok;
                              });
        testSpinWhile([state] { return !state->finished; });
        return state->merged;
    }
    bool testHasAgentSession(int sessionId)
    {
        return findAgentSession(sessionId) != nullptr;
    }
    void testRefreshAgentMergeState() { refreshAgentMergeState(); }
    int testAgentListRescanCount() const { return m_testAgentListRescanCount; }
    bool testAgentMergeStateRefreshing() const { return m_agentMergeStateRefreshing; }
    QString testAgentStatusCellText(int sessionId) const;
    QString testAgentDetailTitleText() const;
    // Why "Move to cloud" would refuse this session, or "" when it would go.
    QString testAgentCloudHandoffBlocker(int sessionId)
    {
        return agentCloudHandoff(sessionId).blocker;
    }
    // The Agents list's right-click menu for this row; the caller owns it.
    QMenu *testBuildAgentListContextMenu(int sessionId)
    {
        return buildAgentListContextMenu(sessionId, nullptr);
    }
    void testLinkAgentSessionToOrgTask(int sessionId, const QString &taskId,
                                       bool completeIfFinished)
    {
        linkAgentSessionToOrgTask(sessionId, taskId, completeIfFinished);
    }
    bool testCanLinkAgentSessionToOrgTask(int sessionId)
    {
        return canLinkAgentSessionToOrgTask(sessionId);
    }
    AgentSession testAgentSession(int sessionId)
    {
        const AgentSession *session = findAgentSession(sessionId);
        return session ? *session : AgentSession();
    }
    QMenu *testAgentTaskLinkMenu(int sessionId);
    QString testAgentViewTaskButtonState(int sessionId);
    bool testAgentDetailTitleWraps() const;
    QString testRenderAgentDetailTitle(const QString &text);
    QString testAgentStatusCellBadges(int sessionId, const AgentDiffStat &stat) const;
    QString testAgentSyncControls(int sessionId, const AgentDiffStat &stat);
    QString testAgentStatusCellToolTip(int sessionId,
                                       const AgentDiffStat &stat) const;
    // The hover control the row carries: 0 none, 1 start, 2 stop, -1 no row.
    int testAgentRunToggleMode(int sessionId) const;
    // Click where a hover control paints, through the real view so the
    // delegate's own hit test decides.
    bool testClickAgentRunToggle(int sessionId);
    bool testClickAgentRowDelete(int sessionId);
    bool testClickAgentRowMoveToTop(int sessionId);
    bool testClickAgentRowMoveToBottom(int sessionId);
    // action: 0 run toggle, 1 delete, 2 move to top, 3 move to bottom.
    bool testClickAgentRowAction(int sessionId, int action);
    // Hover the row and settle its reveal; hands back the cell the controls
    // are drawn in, so a test can measure them where a user would click.
    QRect testRevealAgentRowActions(int sessionId);
    // Whether pointing at the row repaints the end of it — the controls have
    // to be visible where that click lands, not a hidden hotspot.
    bool testAgentRowRevealsRunToggleOnHover(int sessionId);
    // Cloud rows: feed the list one relay payload as if it had just been
    // fetched, and read back what the list made of it.
    void testApplyCloudAgentPayload(const QString &owner, const QString &name,
                                    const QJsonObject &payload);
    // The account-wide payload: every repository's cloud runs in one answer.
    void testApplyCloudAgentAccountPayload(const QJsonObject &payload);
    QList<int> testCloudAgentRowIds() const;
    // How many cloud runs the list holds in all, including the ones whose
    // repository is not the one open.
    int testCloudAgentSessionCount() const { return m_cloudAgentSessions.size(); }
    bool testAgentRowIsCloud(int sessionId) const;
    QByteArray testCloudAgentSignedUrl(const QString &owner,
                                       const QString &name,
                                       const QString &sessionId,
                                       const QString &proof,
                                       const QString &prompt) const;
    QByteArray testCloudAgentAccountSignedUrl() const;
    QUrl testCloudAgentBranchWebUrl(int sessionId) const
    {
        return cloudAgentBranchWebUrl(sessionId);
    }
    // The teammate a row is badged for, or empty when it carries no badge.
    QString testAgentRowTeammate(int sessionId) const;
    void testMarkAgentSessionTeammate(
        int sessionId, const forkmesh::teammates::Config &teammate)
    {
        markAgentSessionTeammate(sessionId, teammate);
    }
    // The row's lead glyph, and its pixels: the teammate badge is painted into
    // the same decoration as the avatar, so this is what shows it landed.
    QRect testAgentRowLeadRect(int sessionId);
    QImage testGrabAgentRowLead(int sessionId);
    QImage testGrabAgentRow(int sessionId); // the whole row band
    // Click the status mark — where the run control used to be.
    bool testClickAgentRowLeadGlyph(int sessionId);
    void testSetCachedAgentDiffFiles(int sessionId, int files)
    {
        AgentDiffStat stat = m_agentDiffStats.value(sessionId);
        stat.files = files;
        m_agentDiffStats.insert(sessionId, stat);
    }
    void testSetCachedAgentSyncState(int sessionId, int behind, bool conflicted)
    {
        AgentDiffStat stat = m_agentDiffStats.value(sessionId);
        stat.behind = behind;
        stat.conflicted = conflicted;
        m_agentDiffStats.insert(sessionId, stat);
    }
    int testCachedAgentDiffFiles(int sessionId) const
    {
        return m_agentDiffStats.value(sessionId).files;
    }
    QStringList testAgentOwnedDiffPaths(const QString &gitDir,
                                        const QString &base,
                                        const QString &branch) const;
    bool testAgentSessionMerged(int sessionId) const;
    void testApplyCodexRateLimits(
        const QJsonObject &rateLimits,
        const QJsonObject &rateLimitsByLimitId = QJsonObject())
    {
        applyCodexRateLimits(rateLimits, rateLimitsByLimitId);
    }
    QString testCodexUsageToolTip() const
    {
        return m_navCodexUsage ? m_navCodexUsage->toolTip() : QString();
    }
    QString testClaudeUsageToolTip() const
    {
        return m_navTokenUsage ? m_navTokenUsage->toolTip() : QString();
    }
    void testShowAgentAccountMenu(const QString &provider)
    {
        showAgentAccountMenu(provider, QPoint(20, 20));
    }
    void testShowAgentUsageMenu() { showAgentUsageMenu(QPoint(20, 20)); }
    void testApplyClaudeUsageResponse(const QJsonObject &response)
    {
        applyClaudeUsageResponse(response);
    }
    void testSelectAgentAccount(const QString &provider,
                                const QString &accountId)
    {
        selectAgentAccount(provider, accountId);
    }
    bool testRenameAgentAccount(const QString &provider,
                                const QString &accountId,
                                const QString &label)
    {
        return renameAgentAccount(provider, accountId, label);
    }
    bool testDeleteAgentAccount(const QString &provider,
                                const QString &accountId)
    {
        return deleteAgentAccount(provider, accountId);
    }
#endif

    ChatBackend *currentBackend() const { return m_backend; }

signals:
    void backendAttached(ChatBackend *backend);
    void chatUserDirectoryChanged();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;
    void recordMessageBoxNotification(QMessageBox *box);
    // Shared by the footer log and the debug bar's tail — they render the
    // same line HTML, so they answer clicks and tooltips the same way.
    bool handleFooterLogViewportEvent(QTextEdit *view, QEvent *event);
    bool handleSettingsLogViewportEvent(QEvent *event);
    Qt::Edges resizeEdgesAtGlobalPos(const QPoint &globalPos) const;
    bool handleFramelessResizeEvent(QObject *obj, QEvent *event);
    void updateFramelessResizeCursor(Qt::Edges edges);
    bool maybeShowSendToPromptMenu(QObject *obj, QContextMenuEvent *ce);
    void appendTextToActivePrompt(const QString &text);
    void sendLogFrequencyEntryToPrompt(const QString &message,
                                       const QString &category, int count,
                                       double share);
    void resizeEvent(QResizeEvent *event) override;

private:
    static constexpr int kNetworkReposSectionIndex = 11;
    static constexpr int kNetworkDiagnosticsSectionIndex = 12;
    static constexpr int kNodesSectionIndex = 13;
    static constexpr int kControlNodeSectionIndex = 14;
    static constexpr int kOrganizationTasksSectionIndex = 15;
    static constexpr int kNotesSectionIndex = 16;
    static constexpr int kUsersSectionIndex = 17;
    static constexpr int kFilesSectionIndex = 18;
    static constexpr int kCalendarSectionIndex = 19;
    static constexpr int kMarketSectionIndex = 20;
    static constexpr int kLedgerSectionIndex = 21;
    static constexpr int kTeamSectionIndex = 22;
    static constexpr int kStatsSectionIndex = 23; // BI-style stat tiles, under Files
    // Tiny health cards for every source file, under Stats.
    static constexpr int kComponentHealthSectionIndex = 24;
    // Live breakdown of this process's memory, under Health. The last
    // section: the deferred-section loop runs up to it.
    static constexpr int kMemorySectionIndex = 25;
    static constexpr int kHomeGridSectionIndex = 26;
    static constexpr int kGovernorSectionIndex = 27;
    static constexpr int kProjectSettingsSectionIndex = 28;

    // What the relay stores for a task title, and so what the composer
    // accepts and what a converted note is elided to before it gets there.
    static constexpr int kOrganizationTaskTitleLimit = 160;
    // The one-line cap every "file this text as an issue" path elides to,
    // so a chat message and a note reach the composer the same length.
    static constexpr int kIssueTitleLimit = 200;

    QWidget *buildSetupPage();
    void initializeFirstRunOnboardingState();
    void maybeShowFirstRunWelcome();
    void showFirstRunWelcome();
    void startFeatureTour();
    void completeFirstRunWelcome();
    void ensureTourTargetVisible(QWidget *target);
    void syncTourPackage(bool installed);
    void syncStatusPagePackage(bool installed);
    void startSession();
    void runDeferredStartup();
    // Opened from Settings -> General -> Membership and the account menu.
    // Nothing opens it on a timer: membership is offered where it is asked
    // for, never pushed.
    void openMembershipOptions();
    void showMembershipPrompt(const QJsonObject &activity);
    // Page-specific warm-up is idempotent; repository tabs remain on-demand.
    void warmDeferredStartupSurfaces();
    bool m_deferredStartupStarted = false; // showEvent armed the triggers
    bool m_deferredStartupRun = false;     // runDeferredStartup already ran
    bool m_startupSurfacesWarmed = false;  // warmDeferredStartupSurfaces ran
    bool m_firstFramePainted = false;      // splash handover happened
    bool m_startupAuthResolved = false;
    bool m_framelessResizeCursorActive = false;
    int m_pendingRestoreRepoIndex = -1;    // last repo to reopen, or -1
    bool m_pendingSilentAuth = false;      // attempt auto-connect on first frame
    bool m_freshInstall = false;
    bool m_firstRunWelcomePending = false;
    bool m_firstRunWelcomeScheduled = false;
    bool m_tourPackageInstalled = false;
    QString m_pendingAutoOpenRepoKey;
    QString m_pendingForkRepoKey;
    bool ensureNodeAccount(const QString &accountName, const QString &solana);
    bool authenticateSilently(const QString &accountName);
    // In-app join: pick a username and you're in. Joining is free — the
    // username is reserved and activated against this device key (no donation,
    // no email/password); the user can attach more nodes to it later.
    // Cross-device credentials can be added later.
    bool runSignupFlow(const QString &accountName, const QString &solana);
    bool runLoginFlow(const QString &accountName);
    QJsonArray fetchCatalogRepos();
    int fetchNodesOnline();
    bool hasActiveAccountSession() const;
    bool hasOwnerSigningCapability(
        const QString &signerAccount = QString()) const;
    void setDesktopCapability(const QString &accountName, bool capable);
    bool restoreDesktopCapability(const QString &accountName);
    void mirrorCatalogRepo(const QString &owner, const QString &name,
                           const QString &cloneUrl, bool isPrivate = false);
    QString hostedCloneUrl(const QString &owner, const QString &name) const;
    // Log in by email (+ optional TOTP) to the exact account selected in the
    // instance dialog. If *fatal is non-null it is set true when retrying the same
    // credentials cannot satisfy that selection, so the caller can stop
    // re-prompting instead of looping the login dialog forever.
    bool verifyTotpLogin(const QString &email, const QString &password,
                         const QString &totp, const QString &accountName,
                         bool *fatal = nullptr);
    void activateAccountSession(const QJsonObject &payload,
                                const QString &fallbackAccount,
                                bool ownsDesktopKey,
                                const QString &instanceUrl);
    void cacheActiveAccountSession(const QString &instanceUrl);
    bool restoreCachedAccountSession(const QString &accountName,
                                     const QString &instanceUrl);
    QString currentAccountInstanceUrl() const;
    QString accountSessionTokenForUrl(const QUrl &url) const;
    QString activeAccountSessionToken() const;
    QUrl accountApiUrlForInstance(const QString &instanceUrl,
                                  const QString &leaf) const;
    void clearAccountSessionMemory();
    // Periodic signed heartbeat that keeps this node eligible for the reward
    // split and refreshes its payout Solana address. It normally rides the
    // account event socket; HTTPS remains the compatibility/recovery path.
    void sendNodeHeartbeat();
    void postNodeHeartbeatFallback();
    void retryNodeHeartbeatOverHttp();
    void onNodeHeartbeatFrame(const QString &requestId, int status,
                              const QJsonObject &response);
    void applyNodeHeartbeatResult(int status, const QJsonObject &response);
    void acknowledgePingPauseDelivery();
    // Ping settings (#347): a local switch is pushed on the next beat and the
    // relay's copy adopted from every other reply, like the pause above.
    void acknowledgePingSettingsDelivery();
    void markPingSettingsChanged();
    void refreshPingSettingsSurfaces();
    void finishNodeHeartbeatDelivery();
    QJsonObject emailNotificationPreferencesPayload() const;
    QString emailNotificationDelivery() const;
    void showNodeClaimCode(const QString &user, const QString &code);
    // Admin node-ownership takeover: an admin viewing another
    // node's profile can request ownership; the heartbeat reply on the TARGET
    // node then carries the pending request, prompted here for that node's own
    // owner to approve or deny. requestNodeOwnership is the admin-side trigger.
    void requestNodeOwnership();
    void showOwnershipTransferPrompt(const QString &admin);
    void submitOwnershipTransferDecision(bool approve);
    // Installer link-code flow: the hosts/SSH installer printed a
    // link code on the fresh machine; confirm + submit it here, signed with
    // this account's key, so the new node is attached to this user.
    void promptHostLinkCode(const QString &code);
    void submitHostLinkCode(const QString &code);
    // "Log in as a user" from the node profile: this node holds its own key, so
    // proving the user's password lets the relay attach this node to that user
    // (users can own many nodes). promptLinkNodeToUser asks for the credentials;
    // submitLinkNodeToUser signs with this node's key and POSTs link-self.
    void promptLinkNodeToUser();
    void submitLinkNodeToUser(const QString &identifier, const QString &password);
    void openLinkNodeInBrowser();
    void pollLinkNodeGrant();
    QString linkErrorMessage(const QString &code) const;
    void pollPendingUsers();
    void fetchRoomPassphrase();
    void startOfficeChannelMirror();
    bool isOfficeConversation(const QString &conversation) const;
    bool isPrivateChatConversation(const QString &conversation) const;
    void purgePrivateChatPings(const QSet<QString> &savedPrivate = {});
    QSet<QString> savedPrivateChannels() const;
    // The local mesh room: people running ForkMesh on this network, found
    // automatically, with nothing about the room leaving it.
    void startLocalMesh();
    // The phone on this network driving the desktop over the mesh port
    // (LocalControl.h; MainWindowLocalControl.cpp).
    void startLocalControl();
    // Creates the control server (LAN or relay, whichever reaches it first).
    bool ensureLocalControlServer();
    void handleLocalControlRequest(int connectionId, int requestId,
                                   const QString &method,
                                   const QJsonObject &params);
    // "Phone (alice), over the relay" for the audit lines.
    QString localControlDeviceLabel(int connectionId) const;
    // Host management from the owner's phone (MainWindowLocalControlHosts.cpp):
    // the Hosts page's own code paths behind a fresh confirmation per change.
    void handleLocalControlHostsRequest(int connectionId, int requestId,
                                        const QString &method,
                                        const QJsonObject &params);
    // Gate 2 local control families (MainWindowLocalControlFamilies.cpp): one
    // handler per family, each in its own MainWindowLocalControl<Family>.cpp.
    void registerLocalControlFamilies();
    void handleLocalControlAgentOpsRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // agentops. WS10
    QJsonArray localControlAgentOpsApprovals(const QByteArray &salt,
                                             QHash<QString, QPair<int, QString>> *index = nullptr);
    QJsonObject localControlAgentOpsUsage();
    void armLocalControlAgentOpsBell();
    void ringLocalControlAgentOpsBell();
    void pushAgentApprovalPings(const QJsonArray &approvals);
    bool prepareAgentApprovalPingRequest(const QString &sessionId, const QString &approvalId,
                                         const QString &agent, const QString &summary,
                                         QNetworkRequest *request, QByteArray *body);
    void handleLocalControlMergeQueueRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // mergequeue. WS11
    QJsonObject localControlMergeQueueRow(int index) const;
    void runLocalControlMergeQueueAction(int connectionId, int requestId, int index,
                                         const QString &action, int number, int delta);
    forkmesh::localcontrol::hosts::ConfirmationStore m_mergeQueueConfirmations;
    void handleLocalControlFleetRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // fleet. WS16
    QJsonObject localControlFleetSummary();
    QJsonObject localControlProvisionStatus();
    void runLocalControlFleetAction(int connectionId, int requestId,
                                    const QString &action, const QString &subject,
                                    const QJsonObject &params);
    void finishLocalControlFleetAction(const QString &action, const QString &subject,
                                       bool ok, const QString &message);
    forkmesh::localcontrol::hosts::ConfirmationStore m_fleetControlConfirmations;
    QSet<QString> m_fleetControlRunning;
    void handleLocalControlFilesRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // files.browse. WS16
    QJsonArray localControlFileRoots();
    QString localControlFileRootPath(const QString &rootId, QString *label = nullptr);
    void runLocalControlFilesGit(int connectionId, int requestId, const QString &action,
                                 const QString &rootId);
    forkmesh::localcontrol::hosts::ConfirmationStore m_filesControlConfirmations;
    QSet<QString> m_filesGitRunning;
    void handleLocalControlDiagnosticsRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // diag. WS16
    QJsonObject localControlHealthSummary();
    void sampleHostVitals();
    QList<forkmesh::localcontrol::diag::VitalsSample> m_hostVitals;
    QSet<int> m_hostVitalsWatchers;
    void answerLocalControlProcesses(int connectionId, int requestId);
    void runLocalControlDiagnosticsAction(int connectionId, int requestId,
                                          const QString &action, const QString &subject);
    forkmesh::localcontrol::hosts::ConfirmationStore m_diagControlConfirmations;
    QJsonObject localControlBackupStatus() const;
    void noteBackupResult(const QString &failure);
    QString m_lastBackupFailure;
    qint64 m_lastBackupResultMs = 0;
    void handleLocalControlTeammatesRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // teammates. WS17
    QString localControlTeammateRepoKey(int repoIndex) const;
    QString localControlTeammateStatus(const forkmesh::teammates::Config &config) const;
    QJsonObject localControlTeammatesPayload();
    bool localControlFindTeammate(const QString &id, forkmesh::teammates::Config *config);
    void runLocalControlTeammateAction(int connectionId, int requestId, const QString &action,
                                       const QString &subject, const QJsonObject &params);
    forkmesh::localcontrol::hosts::ConfirmationStore m_teammateControlConfirmations;
    void handleLocalControlInsightsRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // insights. WS17
    QString localControlShortcutDir(const QString &repoKey, const QString &scope,
                                    int *repoIndex) const;
    void answerLocalControlNetwork(int connectionId, int requestId);
    void runLocalControlInsightsAction(int connectionId, int requestId, const QString &action,
                                       const QString &subject);
    forkmesh::localcontrol::hosts::ConfirmationStore m_insightsControlConfirmations;
    void handleLocalControlDiscordRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // discord. WS13
    void handleLocalControlWalletRequest(int connectionId, int requestId,
                                      const QString &method, const QJsonObject &params); // wallet. WS19
    void handleLocalControlSecretsRequest(int connectionId, int requestId,
                                          const QString &method, const QJsonObject &params);
    void handleLocalControlStatsRequest(int connectionId, int requestId,
                                        const QString &method, const QJsonObject &params);
    void answerLocalControlStatsRepo(int connectionId, int requestId, int repoIndex);
    void answerLocalControlStatsQuality(int connectionId, int requestId, int repoIndex);
    QString localControlStatsRepoDir(int repoIndex) const;
    void handleLocalControlReposRequest(int connectionId, int requestId,
                                        const QString &method, const QJsonObject &params);
    void handleLocalControlUpdateRequest(int connectionId, int requestId,
                                         const QString &method, const QJsonObject &params);
    QJsonObject localControlUpdateState() const;
    void watchLocalControlUpdate(int connectionId);
    void flushLocalControlUpdate();
    void startLocalControlUpdate();
    void restartComputer();
    struct UpdateWatcher {
        qint64 sentSeq = 0;
        QJsonObject sentState;
    };
    forkmesh::localcontrol::update::ProgressFeed m_updateFeed;
    QHash<int, UpdateWatcher> m_updateWatchers;
    QTimer *m_updateWatchTimer = nullptr;
    bool m_computerRestarting = false;
    forkmesh::localcontrol::hosts::ConfirmationStore m_updateControlConfirmations;
    QJsonArray localControlRepoRows() const;
    void answerLocalControlRepoActions(int connectionId, int requestId, int repoIndex);
    void runLocalControlRepoAction(int connectionId, int requestId, const QString &who,
                                   const forkmesh::localcontrol::repos::Intent &intent);
    void ringLocalControlRepos();
    void onDesktopCommandsEvent(const QString &eventRepo);
    void drainAllDesktopCommands();
    void fetchDesktopCommands(const QString &owner, const QString &name);
    void handleDesktopCommandsDrain(const QString &owner, const QString &name,
                                    const QJsonObject &payload);
    QList<forkmesh::desktopcommands::TrustedKey> desktopCommandTrustedKeys();
    void showNextDesktopCommandPrompt();
    void finishDesktopCommandPrompt(bool approved, bool trustSigner);
    QString desktopCommandRepoOwner(const QString &owner, const QString &name) const;
    void runDesktopCommand(const QString &owner, const QString &name,
                           const forkmesh::desktopcommands::Command &command,
                           const QString &signerLabel);
    void settleDesktopCommand(const QString &owner, const QString &name,
                              const forkmesh::desktopcommands::Command &command,
                              const QString &signerLabel, int outcome,
                              const QString &detail);
    bool m_desktopCommandsUnsupported = false;
    qint64 m_desktopCommandsSweptAtMs = 0;
    QSet<QString> m_desktopCommandsFetching;
    QString m_desktopCommandReleaseTag;
    QJsonObject localControlHostsPayload();
    QJsonObject localControlHostDetail(const QString &host);
    void runLocalControlHostAction(int connectionId, int requestId,
                                   const QString &action, const QString &host,
                                   const QJsonObject &params);
    void finishLocalControlHostAction(const QString &action, const QString &host,
                                      bool ok, const QString &message);
    void tapHostLogLineForPhones(const QString &node, int source,
                                 const QString &text);
    void flushLocalControlHostLogs();
    void closeLocalControlStreams(int connectionId);
    QJsonObject localControlAgentsPayload();
    QJsonArray localControlAgentFolders() const;
    void applyLocalControlAgentChoice(int sessionId, const QJsonObject &params);
    void notifyLocalControlAgentsChanged();
    void answerLocalControlOutsideTranscript(int connectionId, int requestId, int id,
                                             qint64 since);
    void readLocalControlOutsideTranscript(
        int id, qint64 since,
        std::function<void(const QJsonObject &payload, const QString &error)> done);
    QString localControlOutsideTranscriptPath(int id) const;
    void readLocalControlOwnTranscript(
        int sessionId, qint64 since, forkmesh::localcontrol::LogTail tail,
        std::function<void(const QJsonObject &payload, const QString &error,
                           forkmesh::localcontrol::LogTail tail)>
            done);
    void handleLocalControlFollowRequest(int connectionId, int requestId,
                                         const QString &method,
                                         const QJsonObject &params);
    void startLocalControlFollowRead(int connectionId, int sessionId, int requestId);
    void markLocalControlFollowDirty(int sessionId);
    void markLocalControlFollowsDirty();
    void scheduleLocalControlFollowPump();
    void pumpLocalControlFollows();
    void watchLocalControlOutsideFollows();
    void dropLocalControlFollows(int connectionId);
    void ringLocalControlOutsideTranscripts();
    void answerLocalControlOutsidePrompt(int connectionId, int requestId, int id,
                                         const QString &text);
    bool outsideAgentSteerable(int id) const;
    const TerminalHandoffSession *outsideAgentTerminal(int id) const;
    const TerminalHandoffSession *outsideAgentRegistration(int id) const;
    int outsideAgentReach(int id) const;
    void handleLocalControlTerminalRequest(int connectionId, int requestId,
                                           const QString &method,
                                           const QJsonObject &params);
    void startLocalControlTerminal(int connectionId, int requestId, const QJsonObject &params);
    void resumeLocalControlTerminal(int connectionId, int requestId, const QJsonObject &params);
    void startLocalControlHostTerminal(int connectionId, int requestId, const QString &host,
                                       const QJsonObject &params);
    void attachLocalControlTerminal(int connectionId, int requestId,
                                    forkmesh::localcontrol::terminals::Terminal &terminal, int cols,
                                    int rows);
    void onLocalControlTerminalOutput(const QString &id, const QByteArray &chunk);
    void onLocalControlTerminalFinished(const QString &id, int code);
    void endLocalControlTerminal(const QString &id, int code);
    void scheduleLocalControlTerminalFlush();
    void flushLocalControlTerminals();
    void dropLocalControlTerminalViewer(int connectionId);
    QSet<qint64> localControlTerminalPids() const;
    QString localControlTerminalForOutside(int id) const;
    QString localControlTerminalForRow(int id) const;
    bool answerLocalControlTerminalAgent(int connectionId, int requestId,
                                         const QString &method, int id,
                                         const QJsonObject &params);
    void typeIntoLocalControlTerminal(const QString &id, const QString &text);
    void answerLocalControlOutsideStop(int connectionId, int requestId, int id);
    void showLocalControlPairingPrompt(int connectionId, const QString &name,
                                       const QString &user, const QString &code);
    void checkLocalControlAccountDevice(
        const QString &key,
        const std::function<void(bool answered, bool owned)> &done);
    QWidget *buildLocalControlTab();
    void refreshLocalControlTab();
    void sendFileToLocalPhone();
    // Marks one of this device's #mesh messages "queued" while no device has
    // taken it yet, and clears the mark once one has.
    void applyLocalMeshQueuedMark(MessageRow *row);
    bool isLocalMeshConversation(const QString &conversation) const;
    void startDiscordClient();
    bool isDiscordConversation(const QString &conversation) const;
    void showDiscordConnectionDialog();
    void disconnectDiscord();
    void clearDiscordLocalHistory();
    // The Discord runtime is a Market package (marketplace/packages/discord):
    // attach shows its sidebar connector and starts the client, detach tears
    // the client down and clears every trace the package kept on this device.
    bool discordPackageInstalled() const;
    void attachDiscordPackage();
    void detachDiscordPackage();
    void purgeDiscordAvatarCache();
    void updateDiscordConnectButton(const QString &status = QString(),
                                    bool connected = false,
                                    bool error = false);
    // Native Market packages contribute through generated, package-owned
    // factories. Core only mounts their generic categories and Log pages.
    void syncDesktopPackageExtensions();
    void clearDesktopPackageExtensions();
    void ensureDesktopPackageLogPages();
    void refreshCurrentDesktopPackageLogPage();
    void refreshDesktopPackageExtensions();
    void rebuildBranchPackageActions();
    void refreshBranchPackageActions(const QString &branch, const QString &dir);
    void logDesktopPackageActivity(const QString &text, const QString &badge);
    void showAdminVerifyDialog();
    // Delivery is not always possible, so an administrator can close a
    // stranded signup out by hand; the Worker records the override as
    // such rather than as a recipient confirmation. A queue that is all
    // unconfirmed signup noise is swept in one press instead.
    bool adminMarkEmailVerified(const QString &target, QString *detail);
    bool adminQuarantinePendingUsers(QWidget *parent);
    void quarantinePendingUsers(const QStringList &targets);
    // Pending federated-instance join requests: a freshly launched
    // instance pings the main relay asking to join; for admins the pending
    // count rides the signed heartbeat reply, a red dot over the top-left
    // relay favicon flags it, and the Approve button beside it opens the link
    // dialog. Approving marks the relay approved upstream, which adds it to
    // /api/world/instances — the World then runs its firework show for the
    // newly joined instance.
    void setPendingRelayJoins(int count);
    void showRelayJoinApprovalDialog();
    bool adminRelayApprove(const QString &pubkey, const QString &action);
    void verifyWallet();
    QUrl accountsApiUrl(const QString &leaf) const;
    QJsonObject postAccountSync(const QString &leaf, const QJsonObject &body,
                                int *status);
    // *transportError receives the reply's failure text when the request
    // failed (for example a TLS handshake or proxy error that has no status).
    QJsonObject postAccountSyncForInstance(const QString &instanceUrl,
                                           const QString &leaf,
                                           const QJsonObject &body,
                                           int *status,
                                           QString *transportError = nullptr);
    QJsonObject getAccountSync(const QString &leaf, int *status);
    // GET /api/accounts/<name> answers with the entire account record — name,
    // status, key, admin flag and the full avatar PNG — and that record only
    // changes when its owner changes it. Re-reading it on a timer and on every
    // repaint put hundreds of identical requests into one session, so the
    // record is read once per account per session (at login, or the first time
    // a screen needs it) and every later reader is served from this cache.
    // Writers that change the record call invalidateAccountRecord().
    void cacheAccountRecord(const QString &account, const QJsonObject &record);
    QJsonObject cachedAccountRecord(const QString &account) const;
    void invalidateAccountRecord(const QString &account = QString());
    QString accountOwner() const; // the registered account name (repo namespace)
    QString hostLinkUserName();
    QString hostLinkSigningAccountName(QString *userName = nullptr);
    void applyAccountEmailVerified(const QString &accountName, bool verified);
    bool accountEmailVerified(const QString &accountName) const;
    QString settingsAccountName() const;
    void refreshSettingsEmailVerifiedBadge();
    // The owner a repo is published/browsed under on the website. It must match
    // the owner used by the signed catalog, direct-HTTPS endpoint binding, and
    // minimal repository update channel.
    QString catalogOwner(const RepositoryRecord &repo) const;
    void runQuickUpdate();
    void updateRebuildRestart();
    void maybeAutoUpdate();
    bool tryPrebuiltAutoUpdate(const QString &clientDir, const QString &tag,
                               const QString &tagCommit);
    void installPrebuiltAndRelaunch(const QString &artifactPath,
                                    const QString &tag);
    void installMirrorCompanionThenRelaunch(const QString &artifactPath,
                                            const QString &tag,
                                            const QString &owner,
                                            const QString &name,
                                            const QString &companionHash);
    QString resolveInstallCloneUrl();
    void buildAndRelaunch(const QString &clientDir, const QString &asUser = QString(),
                          const QString &relaunchPath = QString(),
                          const QString &buildType = QStringLiteral("Release"));
    void buildMirrorNodeCompanion(const QString &clientDir, const QString &buildDir,
                                  std::function<void()> onDone);
    void installAndRelaunch(const QString &built, const QString &appPath);
    void runUpdateStep(const QString &program, const QStringList &arguments,
                       const QString &workingDir, std::function<void()> onSuccess,
                       std::function<void()> onFailure = {},
                       const QMap<QString, QString> &extraEnv = {});
    void runUpdateStepUser(const QString &program, const QStringList &arguments,
                           const QString &workingDir, std::function<void()> onSuccess,
                           std::function<void()> onFailure = {},
                           const QMap<QString, QString> &extraEnv = {});
    // A restart is a long chain of child processes. Clicking the restart button
    // a second time while it spins stops the chain instead of starting another.
    bool restartInProgress() const;
    bool cancelRestartIfRunning();
    void cancelRestart();
    void killUpdateSteps();
    void setUpdateStatus(const QString &status, bool isError = false);
    void showUpdateLog();
    void appendUpdateLog(const QString &text);
    void setFooterUpdateLine(const QString &line);
    // The debug bar's five-line drop-down tail. Fed by the footer line
    // above and, separately, by the CLOUD lines the footer skips.
    void appendDebugLogTailLine(const QString &line);
    void armFooterLogFlush();
    void flushFooterLogLines();
    void pulseLogActivityLights(const QString &badge);
    QString footerLogLineHtml(const QString &clean, QTextEdit *view);
    void styleFooterUpdateLog();
    void openFullLogAtFooterLine(const QString &rawLine);
    void openFullLogForCategory(const QString &category);
    void openFullLogForAll();
    // Jump to the log section with a free-text filter applied — how the Web
    // Requests tab hands a route group ("accounts/*") over to the log.
    void openFullLogForText(const QString &text);
    // The log filtered to one URL, or to "://host" for a whole destination.
    void openNetworkLogForUrl(const QString &urlText);
    void setLogTextFilter(const QString &text);
    void persistProfile();

    QWidget *buildChatPage();
    void ensureSectionBuilt(int index);
    QWidget *buildMarketSection();
    void refreshMarketPage();
    QPixmap marketPreviewPixmap(const QString &path, const QSize &box);
    void applyMarketSearch();
    QList<const forkmesh::market::PackageManifest *> sortedMarketPackages() const;
    void appendMarketPackCards();
    void appendMarketGuideCards();
    void installPackageSet(const QString &label, const QStringList &ids);
    void installPackageFromSavedCopy(const QString &id);
    QWidget *buildPackageSettingsPage(
        const forkmesh::market::PackageManifest &manifest);
    void showPackageSettings(const QString &id);
    void startPackageAgentInstall(const QString &id);
    void cancelPackageAgentInstall();
    void completePackageAgentInstall(bool downloaded);
    bool removePackageAgentModels(const QString &id,
                                  const QStringList &models);
    bool setPackageInstalled(const QString &id, bool installed,
                             bool announce = true);
    // The user-facing install and remove: they ask about the package's data
    // first when there is any to save or restore, then call
    // setPackageInstalled. fromRail always confirms, since a right-click is
    // an easy thing to do by accident.
    void requestPackageInstall(const QString &id);
    void requestPackageRemoval(const QString &id, bool fromRail = false);
    // Writes the package's local data, plus whatever server-side records its
    // page can export, to one JSON file. done(proceed, path): proceed is
    // false when the user cancelled or saving failed (already reported);
    // path is empty when there was nothing to save.
    void savePackageDataCopy(
        const QString &id,
        std::function<void(bool proceed, const QString &path)> done);
    bool importPackageDataFile(const QString &id, const QString &path,
                               QString *error);
    QString packageDataBackupPath(const QString &id) const;
    // Right-click on a package's rail button: settings, export, remove.
    void attachPackageRailMenu(QPushButton *button, const QString &entrypoint);
    QMenu *buildPackageRailMenu(const QString &entrypoint, QWidget *parent);
    void addPackageFeedbackActions(QMenu *menu, const QString &id);
    QMenu *buildPackageActionsMenu(const QString &id, QWidget *parent);
    QPushButton *createPackageActionsButton(std::function<QString()> packageId,
                                            QWidget *parent);
    void showPackageActionsMenu(QPushButton *button, const QString &id);
    void openPackageRating(const QString &id);
    void showPackageFeedbackDialog(const QString &id);
    QString currentNativePackageId() const;
    void updatePackageActionsButton();
    void syncPackageRuntime();
    void syncPackageSurfaces();
    bool packageSurfaceShown(const QString &packageId) const;
    void syncNativePackagePages();
    void showNativePackage(const QString &id);
    void openNativePackageLink(const QString &id);
    QUrl nativePackageLinkUrl(const QString &id) const;
    forkmesh::market::NativePackageWidget *quickAddPackagePromptTarget() const;
    bool submitQuickAddToPackage();
    void openNativePackageWindow(const QString &id);
    void dockNativePackageWindow(const QString &id);
    void requestNativePackage(
        const QString &id,
        const forkmesh::market::NativePackageContribution &contribution,
        const QByteArray &method, const QString &path,
        const QJsonObject &body,
        forkmesh::market::NativePackageReplyHandler handler,
        int attempt = 0);
    bool signForkMeshRequest(QNetworkRequest &request, const QByteArray &method,
                             const QString &cleanPath,
                             const QByteArray &encoded,
                             const QString &proof) const;
    void sendSignedForkMeshRequest(
        const QByteArray &method, const QString &path, const QJsonObject &body,
        const QString &proof,
        forkmesh::market::NativePackageReplyHandler handler);
    void refreshAccountPackages(bool force = false);
    void applyAccountPackages(const QJsonArray &packages);
    void pushAccountPackageChange(const QString &id, const QString &operation,
                                  const QJsonObject &body = QJsonObject());
    void setDeclaredPackageSetting(const QString &id, const QString &key,
                                   const QVariant &value);
    void appendDeclaredPackageSettings(
        QVBoxLayout *layout, const forkmesh::market::PackageManifest &manifest,
        bool installed);
    void appendStatusMonitorSettings(QVBoxLayout *layout);
    void refreshStatusMonitors(bool force = false);
    void applyStatusMonitorsPayload(const QJsonObject &payload);
    void loadCachedStatusMonitors();
    void addStatusMonitor(const QString &name, const QString &url);
    void removeStatusMonitor(const QString &monitorId);
    void finishStatusMonitorWrite(bool ok, const QJsonObject &payload,
                                  const QString &error);
    void probeCustomStatusMonitor(const QString &monitorId);
    bool statusSystemShown(const QString &systemId) const;
    // Anonymous Market popularity counts (/api/market/installs). The fetch
    // fills m_packageInstallCounts and repaints the shelf; the report is sent
    // only while the node's diagnostics setting is on and carries a rotating
    // random id, never an account, node name, or address.
    void refreshPackageInstallCounts();
    void refreshPackageMarketScores();
    void reportPackageInstallCounts(bool throttled = false);
    QString packageStatsRunId() const;
    void populateAppThemeCombo(QComboBox *combo) const;
    void refreshAppThemeChoice();
    void openLedger();

    // Timers (MainWindowTimers.cpp). The installed Timers package owns the
    // timer, countdown and alarm model; the top-bar clock, its chips, their
    // menus and the once-a-second repaint are the host side of that
    // contribution. Everything is inert while the package is not installed.
    QWidget *createTimersNavStrip();
    void attachTimersPackage();
    void detachTimersPackage();
    void refreshTimersNavStrip();
    void showTimersClockMenu();
    void showTimerChipMenu(const QString &id, const QPoint &globalPos);
    bool addTimer(const QString &request, forkmesh::timers::TimerKind kind);
    // A countdown named `label` that runs out at `resetAtMs` (epoch ms), for
    // the usage-limit rows of the agent account menus.
    bool addUsageResetCountdown(const QString &label, qint64 resetAtMs);
    void saveTimers();
    qint64 timersNowMs() const;
    // A countdown or alarm ran out: raise the account ping that reaches the
    // phone (POST /api/notifications/timer). The relay files it in this
    // account's Pings, so on true the desktop raises only its OS card; if the
    // relay then refuses it, the desktop files its own Pings row instead.
    // False when nothing was sent (a stopwatch, a stale expiry, signed out).
    bool sendTimerPing(const forkmesh::timers::Timer &timer,
                       const QString &title, const QString &body);
    bool prepareTimerPingRequest(const QString &timerId, const QString &kind,
                                 qint64 endsAtMs, const QString &label,
                                 QNetworkRequest *request, QByteArray *body);

    // Ledger (MainWindowLedger.cpp). The installed Ledger package projects the
    // relay's public financial records into a page here, and lets the owner
    // settle a pending contributor reward with the wallet linked to this app.
    // The transfer itself reuses the audited tip signer; this page only
    // decides which record is payable and where the receipt is written back.
    QWidget *buildLedgerSection();
    void refreshLedgerPage(bool force = false);
    // `refreshBalances` passes the reader's Refresh press through to the
    // relay so it re-reads the system wallet balances it publishes with the
    // page. An ordinary visit must cost no on-chain read.
    void fetchLedgerRecords(bool refreshBalances = false);
    void fetchLedgerPayableRewards();
    void rebuildLedgerTable();
    // The wallet strip above the table: the two public system wallets the
    // relay names in its own ledger response, plus the two this machine
    // resolves locally — the payout address published on the signed-in
    // account, and the wallet linked here that actually signs a payment.
    // A payout signed from an empty wallet is rejected by the network before
    // it is ever a transaction, so the balances belong on the page that
    // offers to sign one.
    void rebuildLedgerWalletStrip();
    // The payout address published on the account signed in here, preferring
    // what the relay last told us over the locally saved copy.
    QString ledgerMemberPayoutAddress() const;
    // Public balance for one address, from the relay's shared hourly display
    // cache. `force` passes the reader's Refresh press through to it, which
    // the relay still floors to one live read per address per minute.
    void refreshLedgerLocalWalletBalances(bool force);
    void fetchLedgerWalletBalance(const QString &address, bool force);
    // Install a balance only when it is at least as recent as the card's
    // current reading. Payment receipts use this to beat an older page-opening
    // request that may still finish later.
    void applyLedgerWalletBalance(const QString &address, qint64 lamports,
                                  qint64 checkedAt);
    // The entry form above the strip. The relay owns every public record on
    // this page; this is the reader's own book kept beside it, so that the
    // Ledger tracks any expense or income, not only the money ForkMesh moved.
    // Nothing typed here is sent anywhere: it is stored in this app's
    // settings and merged into the page on the way to the table.
    void addLedgerEntry();
    // Resolve a ForkMesh username to its public payout address, collect the
    // task/work-product context and a SOL or USD amount, then hand the exact
    // transfer to the same local signer used by member tips.
    QStringList ledgerRecipientCandidates() const;
    void sendLedgerPayment();
    void settleLocalLedgerRecord(const QString &recordId);
    void removeLocalLedgerRecord(const QString &recordId);
    void loadLocalLedgerRecords();
    void saveLocalLedgerRecords();
    // Watch and fund a Solana address the relay does not name on this page.
    // The entry form's counterparty field doubles as this, so recording a
    // payment to an address starts tracking it in the same keystrokes.
    void trackLedgerWallet(const QString &address);
    // Enable/disable the existing Pay buttons in place. Called whenever the
    // tip wallet's in-flight state changes, so a transfer that fails on the
    // way out re-arms the row without destroying the widgets a click may
    // still be running inside.
    void updateLedgerPayButtons();
    // The password dialog has accepted the wallet passphrase. Change the
    // originating row immediately, before local signing begins, so the reader
    // sees which payment owns the async transfer and reconciliation work.
    void markLedgerPaymentProcessing(const QString &walletAddress);
    // Approved and waiting behind the transfer that owns the wallet now.
    bool ledgerRecordQueued(const QString &recordId) const;
    void payLedgerRecord(const QString &recordId);
    // One unresolved public receipt blocks every contributor payment. Keep its
    // recovery controls on the Ledger page: retry the exact receipt without
    // signing again, inspect its chain signature, or deliberately remove only
    // the local hold after a double-payment warning.
    void retryHeldLedgerPayout();
    void openHeldLedgerPayoutInExplorer();
    void undoHeldLedgerPayout();
    // Ask the recipient of a reward nobody can pay to publish a payout
    // address. The relay owns who may ask and how often; this only names the
    // reward and reports what came back.
    void remindLedgerRecipient(const QString &recordId);
    // When each contributor still owed a payout address was last reminded.
    // Only the owner-scoped estimate list carries it, so it is read after the
    // public list, while signed in, for the repositories that list names.
    void fetchLedgerReminderTimes(const QByteArray &publicEstimatesBody);
    void applyLedgerReminderTimes(const QByteArray &ownerEstimatesBody);
    void openLedgerInBrowser();
    // Where a ledger cell leads, and the hand-off that follows it. Split out
    // of the table's click handlers so the wiring is testable without a
    // browser: one click on the Transaction column opens that transfer, and a
    // double-click anywhere else opens the row's subject.
    QUrl ledgerCellLink(int row, int column) const;
    void openLedgerLink(const QUrl &url);
    void setLedgerStatus(const QString &message);
    // Write a settled reward back onto the issue or pull it paid for. Called
    // once the relay has confirmed the exact finalized transfer, so the local
    // record and the public ledger agree on the same signature.
    void recordRewardReceiptLocally(const QString &rewardId,
                                    const QString &chainSignature);

    // Optional public payout-address notice; crypto never gates the core app.
    QWidget *buildSolanaNotice();
    void updateSolanaNotice();
    void promptSetSolanaAddress();
    void enablePaidMirroring();

    QWidget *buildBreadcrumb();
    void updateBreadcrumb();

    // MainWindowViews.cpp - the window's layout presets (AppViewMode.h) and
    // the "Change view" button the chrome bar's right edge carries.
    QPushButton *buildViewModeButton();
    void showViewModeMenu();
    void restoreAppViewMode();
    void setAppViewMode(forkmesh::ui::AppViewMode mode);
    void cycleAppViewMode();
    void applyAppViewMode();
    void updateViewModeButton();
    // Agents' third splitter pane: a header and a shell terminal, shown only
    // in Workspace. The shell starts the first time the pane is shown, in the
    // selected session's worktree; "+" replaces it with a fresh one there.
    QWidget *buildWorkspaceTerminalDock();
    void startWorkspaceTerminal();
    QPushButton *buildProjectMenuButton(); // project picker, far left of the top bar
    void updateProjectSwitcher();  // refresh the picker's logo and tooltip
    void showProjectMenu();        // dropdown to switch projects or add one
    QPoint topBarMenuAnchor(QWidget *button) const;
    void addProjectContextRows(QMenu &menu);
    void promptAddProject();
    void promptInviteToProject();
    bool projectInstancePending() const;
    void promptChooseProjectInstance();
    void connectProjectInstance(const QString &url, const QString &name);
    void openProject(const QString &slug, bool keepThisOneOpen);
    void showProjectSettings(const QString &slug); // the gear beside each project
    QWidget *buildProjectSettingsSection();
    bool loadProjectSettingsPage(const QString &slug);
    void leaveProjectSettingsPage();
    QScrollArea *m_projectSettingsScroll = nullptr;
    QString m_projectSettingsSlug;
    void showRelayMenu();          // searchable dropdown to switch/add relays
    void updateRelaySwitcher();    // refresh top-bar relay icon / domain / count
    void probeRelayLatency();      // measure round-trip to the active relay (radar)
    void refreshFooterWebsiteStatus();
    void refreshFooterStatusOnActivation();
    bool applyFooterWebsiteStatusPayload(const QJsonObject &payload);
    void applyFooterWebsiteStatusFailure(int httpStatus);
    void refreshDesktopWebsiteProbes();
    void probeDesktopWebsite(const QString &id);
    void crossCheckDesktopWebsiteProbe(const QString &id, const QUrl &url,
                                       const QString &transportError);
    void applyDesktopWebsiteProbe(const QString &id, int httpStatus,
                                  const QByteArray &body,
                                  const QString &transportError,
                                  bool reachableByOtherPrograms = false);
    struct FooterStatusRow; // defined with the rest of the footer state below
    void alertOnDesktopEdgeOutage(const FooterStatusRow &row);
    void alertOnLocalFirewallBlock(const QString &host, const QString &firewall);
    void publishFooterWebsiteStatuses();
    // Clicking a dot opens the surface that dot is about: the site, /status,
    // the API host, the flagship repository page, or the admin error console
    // An invalid URL means only the admin console, which is
    // resolved separately because its path is a deployment secret.
    QUrl websiteStatusTargetUrl(const QString &statusId) const;
    void openWebsiteStatusTarget(const QString &statusId);
    void recheckWebsiteStatus(const QString &statusId);
    void openAdminErrorConsole();
    void onRelayLatencySampled(int ms);
    void initRelayReachabilityWatch(); // OS reachability → instant speed-dot flips
    void openServerWebsite(int index); // open a relay's site in the browser
    void showNodeMenu();           // searchable dropdown to pick a node
    void showNodesWindow();        // full window listing nodes, status, public wallet
    QString topBarUserName() const; // linked user/account name shown in the top bar
    QString nodeOwnerDisplayName() const; // user account that owns this node, if known
    QString chatDisplayName() const; // user identity used for chat sender names
    bool chatIdentityIsGuest() const;
    QString guestChatName() const;   // stable "Guest ####" for this install
    QString machineNodeName() const; // THIS machine's node name (never the username)
    void saveMachineNodeName(const QString &name); // persist + re-advertise
    void saveActionNodeLabels(const QString &labels);
    void updateChatIdentity();     // push user name/avatar into the chat backend
    void updateUserSwitcher();     // refresh top-bar user label/avatar
    QStringList recentUserAccounts() const;
    void rememberUserAccount(const QString &accountName,
                             const QString &instanceUrl = QString());
    bool switchToUserAccount(const QString &accountName,
                             const QString &serverUrl = QString());
    void showUserAccountMenu();
    void openAccountManagement();
    // Away flag and custom status (UserStatus.h). The server is the source
    // of truth: the signed-in account's status is read after sign-in and
    // every change goes through POST /api/accounts/user-status; what is kept
    // here is only the last answer.
    void fetchOwnUserStatus();
    void postOwnUserStatus(
        const QJsonObject &body,
        const std::function<void(bool ok, const QString &error)> &done =
            nullptr);
    void applyOwnUserStatus(const forkmesh::userstatus::UserStatus &status);
    void forgetOwnUserStatus(); // sign-out / account switch
    void startUserActivitySync();
    void syncUserActivity(bool force);
    void forgetUserActivitySync();
    void installChatRetentionKeyResolver();
    void fetchChatRetention();
    void postChatRetention(const QJsonObject &body);
    void applyChatRetention(const QJsonObject &object);
    void setChatRetentionPreferences(
        const forkmesh::chatretention::Preferences &prefs);
    void syncChatRetentionControls();
    void setDefaultChatRetention(qint64 ms);
    void setChatRetentionFor(const QString &conversation, qint64 ms);
    void addChatRetentionMenu(QMenu *menu, const QString &conversation);
    void showSetUserStatusDialog();
    // The status to paint beside a name right now (lapsed emoji/text
    // removed). Our own comes from the server answer; a live peer's own relay
    // frame beats the directory copy; otherwise the directory's.
    forkmesh::userstatus::UserStatus userStatusFor(
        const QString &account, const QString &peerId = QString(),
        bool self = false) const;
    // Repaint every surface that shows a status beside a name, and re-arm the
    // timer that does it again when the next status lapses.
    void refreshUserStatusPresentation();
    void refreshVisibleRowStatuses();
    void scheduleUserStatusExpiry();
    void updateNodeSwitcher();     // refresh top-bar node label / count
    void updateNavSolanaBalance();
    void refreshWebUserSolanaAddress();
    void cacheWebUserSolanaProfile(const QString &account,
                                   const QJsonObject &profile);
    void renderNavSolanaBalance();
    void refreshNavSolanaBalance(bool force = false);
    void queryNavSolanaBalance(const QString &addr, int endpointIndex);
    void showRepoMenu();           // dropdown to open repos / add a local repo
    void updateRepoSwitcher();     // refresh top-bar repo label / count
    void refreshRepoSyncIndicators();
    void refreshSourceControlOutgoing();
    void updateScmCommitControlVisibility();
    void pushCurrentRepoUpstream();
    void undoLatestOutgoingCommit();
    // Launch the async `git push` for a repo whose secret scan has completed and
    // been approved (see pushCurrentRepoUpstream). The repo must already be marked
    // in m_pushingRepos.
    void startRepoPush(int index, const RepositoryRecord &repo,
                       const QString &upstream, int ahead);
    static QString mirrorStateHash(const QString &mirrorPath);
    // Direct repository health proofs intentionally cover the stable routing
    // generation (the served default branch plus tags), not transient agent
    // and collaboration branches. Keep that digest separate from the full
    // heads-and-tags integrity fingerprint above.
    static QString mirrorRoutingStateHash(const QString &mirrorPath);
    // Signed catalog-list URL (adds our viewer token so the relay also returns our
    // own private repos). Shared by fetchCatalogRepos() and refreshRepoPinBanner().
    QUrl catalogListUrl();
    void refreshRepoPinBanner();
    void resetRepoPin();
    static bool relayPublishRepo(const RepositoryRecord &repo,
                                 const QString &relayHost, QString *localBranch,
                                 int *unpublished);
    struct PushTarget {
        bool isRelay = false;
        bool hasUpstream = false;
        QString upstream;
        QString remoteUrl;
    };
    static PushTarget readPushTarget(const RepositoryRecord &repo,
                                     const QString &relayHost);
    void continueRepoPush(int index, const RepositoryRecord &repo,
                          const PushTarget &target);
    void showChatView();           // open the chat view from the top-bar button
    void updateChatButton();       // refresh the top-bar chat unread indicator
    bool isChatViewVisible() const; // chat tab open + window active (i.e. being read)
    void clearActiveConversationUnread();
    qint64 newestChatTimestamp(const QString &conversation) const;
    void queueReadMark(const QString &conversation);
    void flushReadMarks();
    void applyReadMarks(const QHash<QString, qint64> &marks);
    void fetchReadMarks();
    QString mostRecentUnreadConversation() const;
    void updateChatUnreadBanner();
    void markAllChatRead();
    void openNextUnreadConversation();
    QStringList unlistedChatConversations() const;
    QString chatConversationLabel(const QString &conversation) const;
    QString officeChannelLabel(const QString &conversation) const;
    bool chatPeerOnline(const QString &peerId, const QString &peerName) const;
    void refreshChatSidebarChrome();
    void refreshChatWorkspaceUser();
    void updateConnectionStatus(); // top-right "● Connected · N nodes online"
    void setNodeOffline(bool offline);
    void updateNodeOnlineControls();
    // Offline mode: the app stops using the network at all. Turned on from the
    // switch beside the footer version, or by the connectivity watch when the
    // relay cannot be reached -- `automatic` records which, because a mode the
    // app turned on for itself is allowed to turn itself back off when the
    // network returns, and one the reader asked for is not.
    void setOfflineMode(bool offline, bool automatic = false);
    // Startup: decide which offline mode this session opens in, then (once the
    // shell is up) say so if a hand-picked one survived the restart.
    void restorePersistedOfflineMode();
    void announceRestoredOfflineMode();
    void updateOfflineModeControls();
    bool offlineModeActive() const { return m_offlineMode; }
    // Bring the presence transports back up. Both the node event socket and
    // the signed heartbeat stop while this machine is offline or the app is in
    // offline mode, and both have to come back when either flag clears.
    void startNodePresence();
    // One connectivity verdict from the relay reachability probe. Enough of
    // them in a row is what turns offline mode on by itself; one success is
    // what turns an automatic one back off.
    void noteRelayReachability(bool reachable);
    QWidget *buildNetworkLogDock();
    void initializeDeferredGlobalLogDock();
    QWidget *buildStatusBar();
    void attachDebugActivityLights();
    quint64 beginBackgroundTask(const QString &kind,
                                const QString &detail = QString(),
                                const forkmesh::LogSource &source =
                                    forkmesh::logSourceHere());
    void finishBackgroundTask(quint64 id, bool success,
                              const QString &detail = QString(),
                              const QString &failureKind = QString());
    void noteBackgroundActivity(const forkmesh::BackgroundTicket &ticket,
                                bool backgrounded, bool started);
    void tickBackgroundQueue();
    // What a word's open tickets have said about themselves so far (adhoc
    // #186); folded into the outcome tally when the last of them retires.
    struct BackgroundTaskContext {
        QStringList routines;     // distinct, oldest first, capped
        QString workingDirectory; // newest non-empty
        qint64 longestQueuedMs = -1;
        int failures = 0;
        QString lastFailure; // "exit 128", "timed out", …
    };
    void recordBackgroundOutcome(const QString &word, qint64 elapsedMs,
                                 const QString &detail, qint64 now,
                                 bool backgrounded,
                                 const forkmesh::LogSource &source,
                                 const BackgroundTaskContext &context);
    void flushBackgroundOutcomes(bool force);
    // Names the checkout a ticket's working directory belongs to — the
    // repository, and the agent session whose worktree it is — for the
    // outcome line (adhoc #186).
    void describeBackgroundWorkingDirectory(
        const QString &dir, forkmesh::BackgroundOutcomeContext *context) const;
    static QString backgroundTaskWord(const QString &kind);
    void updateFooterGitIdentity();
    void updateFooterCommitInfo();
    void startDiagnostics();
    static QString stallLogPath();
    void updateFooterDiagnostics();
    void updateQtDiagnosticTiles();
    void showQtDiagnosticsDialog();
    void checkFileDescriptorPressure();
    void refreshRepositoryStats();
    void toggleRepositoryRatchet(bool enabled);
    void onUiStall(qint64 peakMs, const QString &stallId,
                   const QString &likelyOrigin, const QString &blockingCall,
                   const QString &report, qint64 startedAtMs = -1);
    void maybeTriggerStallMonitoringTeammates();
    int stallReportRepoIndex() const;
    void clearStallLog();
    bool sendStallLogToAgent();
    QString stallLogLocationsBlock() const;
    QString stallFixPrompt() const;
    void sendStallReportToComposer();
    QString diagnosticsReport(bool advanceBaseline);
    void copyDiagnosticsReport();
    void sendDiagnosticsReportToAgent();
    void resetDiagnosticsReportBaseline();
    void showDiagnosticsDialog();
    void showDesktopResourceDiagnostics();
    void showDesktopProcessDetails(const forkmesh::desktopprocs::Process &process);
    void showHighMemoryProcessPanel(bool activate = true);
    void updateHighMemoryHeading();
    void refreshHighMemoryProcessTable();
    void killHighMemoryProcess(qint64 pid, const QString &name);
    void killAllHighMemoryProcesses(const QString &name, const QList<qint64> &pids);
    void stopHighMemoryAgent(int sessionId, const QString &label);
    void autoKillTopHighMemoryProcess(const QList<QPair<qint64, QString>> &candidates);
    void continueHighMemoryAutoKill(qint64 pid, int waitedSeconds);
    void endHighMemoryAutoKill();
    QWidget *buildLogSection();
    // Resolve everything a Wrangler tail needs: the Worker bundle directory,
    // npx, the account, and the API token (from the settings field or the
    // stored deploy secret). Returns false after explaining why in a notice. The
    // token is returned to the caller so it can be scrubbed from memory once
    // the tail ends.
    bool prepareCloudflareTail(QString *token, QString *workerDirectory,
                               forkmesh::control::CloudflareBootstrapCommand
                                   *command);
    void setCloudLogMonitorEnabled(bool enabled);
    bool cloudLogMonitorTokenAvailable() const;
    QString cloudLogMonitorNodeRequirement() const;
    void startCloudLogMonitorIfConfigured();
    void stopCloudLogMonitorAfterFailure();
    void handleCloudLogMonitorEnded(int exitCode, qint64 uptimeMs);
    void scheduleCloudLogMonitorRestart(int delayMs);
    void cancelCloudLogMonitorRestart();
    void restartCloudLogMonitor();
    void releaseCloudLogMonitorProcess();
    void applyDebugBarStartupPreference();
    void readCloudLogMonitorOutput();
    void consumeCloudLogMonitorBytes(const QByteArray &chunk);
    void drainCloudLogMonitorRecords();
    void handleCloudLogMonitorLine(const QString &line);
    void updateCloudLogMonitorTooltip();
    bool cloudLogMonitorRunning() const;
    void showNetworkLogPopout();

    void loadServers();
    void saveServers();
    QString activeServerInstanceId() const;
    QString activeServerInstanceUrl() const;
    bool activeServerMatches(const QString &instanceId,
                             const QString &instanceUrl) const;
    void isolateAccountForServerSwitch(const QString &accountName);
    void forgetServerInstance(const ServerConfig &server);
    void switchToServer(int index);
    void promptAddServer();
    void renameServer(int index);
    void removeServer(int index);
    void loadActiveServerIntoEdits();
    void persistEditsToActiveServer();
    void loadCachedFavicons();
    void fetchFavicon(int index);
    void fetchFaviconForHost(const QString &host);
    void fetchFaviconFromUrl(const QString &host, const QUrl &url);
    QPixmap faviconFor(const ServerConfig &server) const;
    QString logFaviconTag(const QString &message, QTextEdit *view);
    void registerLogFaviconResource(const QString &host, QTextEdit *view);
    void refreshLogFavicon(const QString &host);
    QWidget *buildHomeSection();
    QWidget *buildNodeProfileSection();
    QWidget *buildNodeProfilePanel(); // builds inner scroll area; called by buildNodeProfileSection
    void showNodeProfile(const QString &nodeId, const QString &nodeName,
                         bool navigate = true);
    void hostNodeProfilePanel(bool inSettings);
    // One row on the Nodes page, three to a row on the narrower Settings page.
    void layoutProfileActions(bool inSettings);
    bool profilePanelInSettings() const
    {
        return m_nodeProfilePanel && m_settingsProfileHost &&
               m_nodeProfilePanel->parentWidget() == m_settingsProfileHost;
    }
    void syncSettingsProfileTab();
    void refreshProfileHostingStats(); // rebuild the per-repo hosting lines
    // "USER ACCOUNT" section: link state + CTA. Repainting the profile reads the
    // account record from cacheAccountRecord(); force=true is for the callers
    // that just changed it (node linking, ownership transfer) and must re-read.
    void refreshProfileAccountStatus(bool force = false);
    void applyProfileAccountRecord(const QString &node, const QJsonObject &record);
    void renderProfileAccountStatus();  // paint the section from cached state only
    QString linkedNodesHtml() const;    // "<b>a</b>, <b>b</b>" of m_profileLinkedNodes
    void fetchLinkedNodesFromOwner(const QString &node, const QString &owner);
    void rescaleProfileAvatar();       // re-render the full-width avatar banner
    void hideNodeProfile();
    void checkNodeBalance();
    void querySolanaBalance(const QString &addr, int endpointIndex);
    void selectNode(const QString &node);
    QWidget *buildIssuesSection();
    QHBoxLayout *buildIssueListToolbar(QWidget *page, QWidget *listPane,
                                       QPushButton *&issueBurnupButton);
    void buildIssueListViews();
    QWidget *buildIssueDetailCenter(QPushButton *&closeDetailButton);
    QWidget *buildIssueSidebar();
    void buildIssueSidebarValues();
    void buildIssueSidebarEditors(QWidget *meta);
    QWidget *buildIssueAgentBox(QWidget *meta);
    void copyIssueMcpPrompt();
    void layoutIssueSidebar(QWidget *meta, QWidget *agentBox);
    void buildIssueDetailPane(QWidget *center, QWidget *meta);
    void connectIssuesSectionSignals(QButtonGroup *issueTabGroup,
                                     QPushButton *issueBurnupButton);
    QWidget *buildOrganizationTasksSection();
    void refreshOrganizationTasks();
    void updateOrganizationTaskRefreshAge();
    QWidget *buildCalendarSection();
    void refreshCalendar();
    void refreshRepositoryCalendarEvents();
    void scanRepositoryCalendarSlice();
    void publishRepositoryCalendarEvents();
    void rebuildCalendarList();
    void renderCalendar();
    void openCalendarEventDialog(const QString &eventId = QString(),
                                 const QDate &date = QDate());
    using CalendarReplyHandler = std::function<void(
        bool, const QJsonObject &, const QString &)>;
    void requestCalendar(const QByteArray &method, const QString &path,
                         const QJsonObject &body,
                         CalendarReplyHandler handler);
    QWidget *buildNotesSection();
    void refreshNotes();
    void renderNotesList(const QString &selectId = QString());
    void selectNote(const QString &id, const QString &storage);
    void createNote();
    void saveNote();
    void deleteNote();
    void updateNotePublishState();
    QJsonObject cloudNoteFor(const QJsonObject &note) const;
    void shareNote();
    void attachNoteConversation();
    // Hand the note on screen to the tracker that actually chases work: an
    // organization task, or an issue on one of this node's repositories. The
    // note is copied, never moved — it stays in the list as the working draft.
    void convertNoteToTask();
    void convertNoteToIssue();
    // The title and body a conversion starts from, or false when there is no
    // note selected to convert.
    bool noteConversionSource(QString *title, QString *markdown);
    // The relay's id for the selected note, or empty while it only exists on
    // this computer.
    QString selectedNoteCloudId() const;
    // The public address of the selected note, or empty unless it is
    // published — a private note has no address worth quoting.
    QString selectedNotePublicUrl() const;
    void showNoteVersions();
    using NoteReplyHandler = std::function<void(
        bool, const QJsonObject &, const QString &)>;
    void requestNotes(const QByteArray &method, const QString &path,
                      const QJsonObject &body, NoteReplyHandler handler);
    void applyOrganizationTasks(const QJsonObject &payload);
    // Apply the authoritative task returned by a mutation without depending
    // on a second catalog read. In particular, an edit may finish while a
    // prior refresh is still in flight, which would otherwise leave its row
    // showing the old values until the next manual refresh.
    bool replaceOrganizationTask(const QJsonObject &task);
    bool settleOrganizationTaskEdit(const QString &taskId,
                                    const QJsonObject &previous, bool ok,
                                    const QJsonObject &payload,
                                    const QString &error, bool *updated);
    bool organizationTaskCheckboxToggleable(int index) const;
    void toggleOrganizationTaskCheckbox(int index);
    // Re-apply this device's saved or in-flight task edits over a catalog
    // read that was issued before those writes landed.
    void overlayOrganizationTaskLocalEdits();
    void linkExistingAgentsToOrganizationTasks(const QJsonArray &tasks);
    void renderOrganizationTaskRows(const QString &selectTaskId = QString());
    void appendOrganizationTaskRows(int count);
    void refreshOrganizationTaskRowThumbnails(const QString &taskId);
    void updateOrganizationTaskPageLabel();
    void openCreatedOrganizationTask(const QJsonObject &task);
    void openOrganizationTask(const QString &taskId);
    void revealOrganizationTaskStatus(const QJsonObject &task);
    void setOrganizationTaskBadge(int openCount);
    void restoreOrganizationTaskBadge();
    void refreshOrganizationTaskBadge();
    void onOrganizationTaskEvent();
    void drainPendingOrganizationTaskEvent();
    void renderOrganizationTaskDetail();
    void updateOrganizationTaskActions();
    void createOrganizationTask();
    // The composer behind "New task" and behind a converted note. A
    // conversion opens the same dialog with the note's title and body already
    // typed in, and passes `onCreated` so it can say what happened somewhere
    // the operator is actually looking — the tasks page is not on screen.
    void composeOrganizationTask(const QString &windowTitle,
                                 const QString &prefillTitle,
                                 const QString &prefillDetails,
                                 std::function<void()> onCreated);
    void createQuickAddOrganizationTask();
    QStringList organizationTaskMemberNames() const;
    QString organizationTaskActorName() const;
    void refreshQuickAddTaskAssignees();
    QString quickAddTaskAssignment() const;
    bool takeQuickAddNote(const QString &text);
    void openComposerModelsPage();
    void createOrganizationTaskFollowUp();
    void moveOrganizationTaskToIssue();
    void addOrganizationTaskToPrompt();
    // One click from the task detail to a running agent: the task is attached to
    // the footer prompt box and launched with that box's live settings
    // (repository, provider, model, permission mode, effort, attachments), and
    // the run it starts is bound back to this task instead of opening a second
    // one — so status and completion report against the task the operator was
    // looking at.
    void startOrganizationTaskAgentFromPrompt();
    int localAgentSessionIdForTask(const QString &taskId) const;
    QString localAgentRunLabelForTask(const QString &taskId) const;
    bool organizationTaskAgentFace(const QJsonObject &task, QPixmap *face,
                                   bool *working) const;
    void applyOrganizationTaskAgentFace(QTableWidgetItem *item,
                                        const QJsonObject &task);
    QJsonObject organizationTaskForRow(int row) const;
    int organizationTaskRowForId(const QString &taskId) const;
    void refreshOrganizationTaskAgentRows();
    void applyOrganizationTaskPeopleFaces(QTableWidgetItem *item);
    void refreshOrganizationTaskPeopleFaces();
    void rebuildOrganizationTaskPeopleStrip();
    void updateOrganizationTaskPeopleStrip(
        const QHash<QString, int> &openByAssignee);
    void setOrganizationTaskAssigneeFilter(const QString &filter);
    bool organizationTaskRowsHaveWorkingAgent() const;
    void updateOrganizationTaskSpinTimer();
    void setOrganizationTaskListView(forkmesh::ui::ListViewMode view);
    void tickOrganizationTaskSpinners();
    void editOrganizationTask();
    void assignOrganizationTaskToAgent();
    void queueOrganizationTaskAgent(const QJsonObject &task);
    void runOrganizationTaskAction(const QString &action);
    void sendOrganizationTaskAction(const QString &taskId,
                                    const QString &action,
                                    const QJsonObject &body);
    void patchOrganizationTask(const QJsonObject &task,
                               const QJsonObject &body);
    void syncOrganizationTaskProperties(const QJsonObject &task);
    void commitOrganizationTaskProperty(const QString &field);
    // Comment on the selected task, and check in on it. Both are ordinary
    // task writes that also ping: a comment pings the task's floor plus every
    // @name it uses, a check-in tells the same people how the work is going.
    void commentOnOrganizationTask();
    void postOrganizationTaskComment(const QString &body);
    void submitOrganizationTaskQuickComment();
    void checkInOnOrganizationTask();
    void remindOrganizationTaskAssignee();
    // The task's own timeline. Fetched per selected task rather than with the
    // catalog: one page of the list is 100 rows and no row shows its history.
    void loadOrganizationTaskHistory(const QString &taskId);
    void deleteOrganizationTask();
    void sendOrganizationTaskDelete(const QString &taskId, int cleanupMode,
                                    bool force = false);
    void deleteClosedOrganizationTasks();
    QString taskWorkItemNodeName() const;
    forkmesh::taskwork::Guard taskWorkItemGuard() const;
    QStringList taskWorkItemRepositories(const QString &repository,
                                         int *preferredCount = nullptr) const;
    QJsonObject taskWorkItemCache() const;
    void saveTaskWorkItemCache(const QJsonObject &cache);
    QList<forkmesh::taskwork::WorkItem> localTaskWorkItems(
        const QString &taskId, const QJsonArray &serverItems) const;
    void rememberOrganizationTaskWorkItems(const QString &taskId,
                                           const QJsonArray &serverItems);
    void reconcileOrganizationTaskWorkItems(const QJsonArray &tasks,
                                            bool complete);
    void attachOrganizationTaskWorkItem(const QString &taskId,
                                        const QString &repository,
                                        const QString &branch,
                                        const QString &worktree);
    void attachOrgTaskWorkItemForSession(int sessionId);
    void sendOrganizationTaskWorkItemAttach(const QString &taskId);
    void attachExistingWorktreeToOrganizationTask();
    void inspectOrganizationTaskWorkItems(
        const QList<forkmesh::taskwork::WorkItem> &items,
        std::function<void(const QList<forkmesh::taskwork::Inspection> &)>
            done);
    void cleanupOrganizationTaskWorkItems(
        const QString &taskId,
        const QList<forkmesh::taskwork::WorkItem> &items, int mode);
    void removeOrganizationTaskWorkItems(
        const QString &taskId,
        const QList<forkmesh::taskwork::Inspection> &inspections,
        bool discard);
    void deleteNextClosedOrganizationTask();
    void finishClosedOrganizationTaskDelete();
    using OrganizationTaskReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using OrganizationTaskAttachmentHandler =
        std::function<void(bool, const QByteArray &, const QString &,
                           const QString &)>;
    void requestOrganizationTasks(
        const QByteArray &method, const QString &path, const QJsonObject &body,
        OrganizationTaskReplyHandler handler,
        const QList<int> &expectedHttpStatuses = {});
    void requestOrganizationTaskAttachment(
        const QString &taskId, const QString &attachmentId,
        OrganizationTaskAttachmentHandler handler);
    void requestOrganizationTaskCatalog(OrganizationTaskReplyHandler handler);
    void requestOrganizationTaskCatalogPage(
        int offset, QJsonArray catalog, QJsonObject firstPage,
        OrganizationTaskReplyHandler handler);
    QWidget *buildChatSection();
    QWidget *buildChatSidebar(QPushButton *&addChannelButton);
    QWidget *buildChatHeader();
    QWidget *buildChatMainColumn(QWidget *header, QPushButton *&sendButton);
    void buildChatThreadPane();
    void buildChatMembersPanel();
    void syncChatMembersPanel();
    QWidget *chatDayDivider(const QDate &day);
    void connectChatSectionSignals(QPushButton *addChannelButton,
                                   QPushButton *sendButton);
    QWidget *buildSettingsSection();
    // Re-tints the Settings section icons for the current theme.
    void refreshSettingsSectionIcons();
    void restoreSettingsSectionsExpanded();
    // buildSettingsSection()'s pieces: part builders create each section's
    // widgets (in the original creation order), tab builders lay them out.
    struct SettingsSectionParts;
    void buildSettingsProfileParts(SettingsSectionParts &parts);
    void buildSettingsStartupParts(SettingsSectionParts &parts);
    void buildSettingsVmParts(SettingsSectionParts &parts);
    void wireSettingsVmActions(SettingsSectionParts &parts);
    void buildSettingsAgentBehaviourParts(SettingsSectionParts &parts);
    void buildSettingsPingsParts(SettingsSectionParts &parts);
    void buildSettingsEmailNotifyParts(SettingsSectionParts &parts);
    void buildSettingsVoiceParts(SettingsSectionParts &parts);
    void buildSettingsAppearanceParts(SettingsSectionParts &parts);
    void buildSettingsMembershipParts(SettingsSectionParts &parts);
    void buildSettingsAgentProviderParts(SettingsSectionParts &parts);
    void buildSettingsAgentBackendParts(SettingsSectionParts &parts);
    void buildSettingsAgentUsageParts(SettingsSectionParts &parts);
    void buildSettingsRepoSyncParts(SettingsSectionParts &parts);
    void buildSettingsImportAndSecretsParts(SettingsSectionParts &parts);
    void buildSettingsFooterParts(SettingsSectionParts &parts);
    QWidget *buildSettingsGeneralTab(const SettingsSectionParts &parts);
    QWidget *buildSettingsReposTab(const SettingsSectionParts &parts);
    QWidget *buildSettingsSshKeysTab();
    QWidget *buildSettingsPingsTab(const SettingsSectionParts &parts);
    QWidget *buildSettingsAgentsTab(const SettingsSectionParts &parts);
    QWidget *buildSettingsVoiceTab(const SettingsSectionParts &parts);
    void showAgentRecycleBinDialog();
    // Settings -> Apps: external command-line tools ForkMesh can use, with
    // live PATH checks, version tests and package-manager installation.
    QWidget *buildSuggestedAppsTab();
    void refreshSuggestedApps();
    void testSuggestedApp(const QString &id);
    void installSuggestedApp(const QString &id);
    // Settings -> Organizations tab: the desktop twin of the website's
    // organization panel — the orgs this account belongs to, their member
    // roster and roles, their permission teams, and the repositories each org
    // name fronts at /<org>/<repo>. MainWindowOrganizations.cpp.
    QWidget *buildOrganizationsTab();
    using OrganizationAdminReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    // The canonical prefix and the body fields it binds for one organization
    // request, or "" when the account key may not sign for it at all. Must
    // stay in lockstep with _org_admin_operation in the worker's entry.py.
    static QString organizationAdminProof(const QByteArray &method,
                                          const QString &path,
                                          QStringList *signedFields = nullptr);
    static QString organizationCanonicalField(const QJsonValue &value);
    void requestOrganizationAdmin(const QByteArray &method,
                                  const QString &path, const QJsonObject &body,
                                  OrganizationAdminReplyHandler handler);
    void refreshOrganizations();
    void renderOrganizations(const QString &keepSelected);
    void createOrganization();
    void deleteOrganization();
    void refreshOrganizationDetail();
    void applyOrganizationProfile(const QString &org,
                                  const QJsonObject &payload);
    void saveOrganizationProfile();
    void loadOrganizationMembers(const QString &org);
    void saveOrganizationMember();
    void removeOrganizationMember();
    void loadOrganizationTeams(const QString &org);
    void onOrganizationTeamSelected();
    void saveOrganizationTeam();
    void deleteOrganizationTeam();
    void addOrganizationTeamMember();
    void removeOrganizationTeamMember();
    bool organizationHasMember(const QString &member) const;
    void loadOrganizationRepositories(const QString &org);
    void linkOrganizationRepository();
    void unlinkOrganizationRepository();
    void populateNewRepositoryOrganizationChoices(QComboBox *choices,
                                                   QLabel *hint);
    void linkNewRepositoryToOrganization(const QString &organization,
                                         const QString &repository,
                                         const QString &node,
                                         int retry = 0);
    QString selectedOrganization() const;
    void setOrganizationStatus(const QString &text, bool error);
    // Settings -> Security tab: private vulnerability reporting form.
    QWidget *buildVulnReportTab();
    void submitVulnerabilityReport();
    QWidget *buildMcpConnectorTab();
    void refreshMcpConnectorTab();
    void generateMcpConnector();
    void revokeMcpConnector();
    void testMcpConnector();
    QString mcpServerScriptPath() const;
    void startGenieAgent();
    // launchGenieRun() is the part that actually starts the tracked session, so
    // the credential mint below can call it once the relay answers.
    // requestGenieCredential() is the "we're already logged in" path
    // this install signs for its own task-only remote-MCP bearer with the
    // account key it already holds, saves it like the pasted one, and then
    // launches — no website round trip, no Settings detour.
    void launchGenieRun(int repoIndex, const QString &typedGuidance);
    void requestGenieCredential(int repoIndex, const QString &typedGuidance);
    QString genieSetupPrompt(const QString &extraInstruction) const;
    QUrl genieMcpUrl() const;
    void applyGenieTaskTitle(int sessionId, const QString &assistantText);
    void saveGenieSettings();
    // True while a credential mint is in flight, so a second press of "genie"
    // waits for the first instead of minting a second credential.
    bool m_genieCredentialPending = false;
    // Settings -> Quick Setup tab: provision a fresh instance in one pass —
    // identity and workflow credentials applied together.
    QWidget *buildQuickSetupTab();
    void applyQuickSetup(QLineEdit *userEdit, QLineEdit *nodeEdit,
                         QLineEdit *cfTokenEdit, QLineEdit *cfAccountEdit,
                         QLineEdit *vultrTokenEdit, QLabel *statusLabel);
    QWidget *buildDataSection();
    void refreshDataDirTable();
    void scanDataDirRow(quint64 generation, int row);
    void exportConfigData();
    void importConfigData();
    void deleteDataDir(const QString &label, const QString &path, bool critical);
    void deleteAllData();
    void setDataStatus(const QString &text, bool error = false);
    void stopLiveServicesForDataOp();
    void relaunchForkMesh();
    void startAutoBackups();
    void takeBackupNow(bool automatic);
    void refreshBackupTable();
    void pruneOldBackups();
    void restoreConfigArchive(const QString &archivePath);
    // Second half of an import, resumed once the unpacking `tar` has exited.
    // Split out so the unpack itself can run asynchronously: it is a multi-
    // gigabyte read for a busy node and used to park the GUI thread for
    // minutes.
    void finishConfigRestore(const QString &workRoot, const QString &stamp);
    // Greys out Export/Import while either one owns m_configArchiveProcess.
    void setConfigArchiveBusy(bool busy);
    void setBackupStatus(const QString &text, bool error = false);
    QString backupRoot() const;
    bool autoBackupEnabled() const;
    int backupKeepCount() const;
    void installBackupHost();
    void installCleanupHost();
    QWidget *buildNotificationsSection();
    QWidget *buildNetworkReposSection();
    void refreshNetworkReposPage();
    void renderNetworkRepos(const QJsonArray &repos);
    // renderNetworkRepos pieces: group the advertised copies of a repository,
    // reduce each group to one display row, and build a row's action buttons.
    QList<QList<QJsonObject>> groupNetworkRepoRecords(const QJsonArray &repos);
    QList<QJsonObject> canonicalNetworkRepoRows(
        const QList<QList<QJsonObject>> &groups);
    void addNetworkRepoRowActions(int row, const QString &routeOwner,
                                  const QString &routeName,
                                  const QString &cloneUrl, bool isPrivate,
                                  int localFork, int mirroredIndex);
    // The repositories on this machine the relay catalog above never lists:
    // private repositories, which are not advertised publicly, and local-only
    // checkouts that were never published.
    void renderLocalOnlyRepos();
    void fillNetworkRepoDataCells(QTableWidget *table, int row,
                                  const QJsonObject &repo);
    void fetchNetworkRepoMirrors(const QString &owner, const QString &name,
                                 int row, int generation);
    void applyNetworkRepoMirrors(const QByteArray &body, const QString &owner,
                                 const QString &name, int row, int generation);
    bool networkRepoMirrorCell(const QString &owner, const QString &name,
                               int row, int generation) const;
    int findNetworkRepoIndex(const QString &owner, const QString &name,
                             bool includePreview = true) const;
    int findNetworkLocalForkIndex(const QString &owner, const QString &name) const;
    void openNetworkRepo(const QString &owner, const QString &name,
                         const QString &cloneUrl, bool isPrivate);
    void forkNetworkRepo(const QString &owner, const QString &name,
                         const QString &cloneUrl, bool isPrivate);
    void mirrorNetworkRepo(const QString &owner, const QString &name,
                           const QString &cloneUrl, bool isPrivate);
    // An administrator can erase a repository this machine holds no copy of:
    // the catalog row and every relay-side record (adhoc #196).
    bool canAdminPurgeRepos() const;
    void adminPurgeNetworkRepo(const QString &owner, const QString &name);
    void discardNetworkRepoPreview(const QString &owner, const QString &name);

    // Local control node: one operational surface for this machine's mirrors,
    // sync/health/logs, local identity + wallet public address, repository
    // permissions, Cloudflare relay bootstrap, and remote host deployment.
    QWidget *buildControlNodeSection();
    void refreshControlNode();
    void refreshControlMirrorReadiness();
    void runControlNodeHealthCheck();
    void syncControlNodeMirrors();
    void updateControlRepositoryPermission(QTableWidgetItem *item);
    void saveControlWalletAddress();
    void runCloudflareBootstrap(bool dryRun);
    void cancelCloudflareBootstrap();
    void installCloudflareFirstMirror();
    void provisionDirectMirrorEndpoint(bool dryRun);
    bool rebuildDirectMirrorGatewayConfiguration(
        QString *error = nullptr, bool restartRunningGateway = false);
    bool directMirrorGatewayConfigured() const;
    // The node name, mirror hostname, and Worker router public key the direct
    // gateway would be built from, returning whether all three are usable.
    bool resolveDirectMirrorGatewayIdentity(
        QString *nodeName, QString *hostname,
        QString *routerPublicKey) const;
    void appendControlNodeOutput(const QString &text);
    QWidget *buildSiteDeployCard();
    void runSiteDeploy(const QString &target = QStringLiteral("changed"));
    void cancelSiteDeploy();
    QWidget *buildNodeDeployCard();
    void runNodeDeploy();
    void setNodeDeployRunning(bool running);
    void appendNodeDeployOutput(const QString &text);
    void updateControlDeployActivity();
    // Owner-only clear-text JSON recovery package: all settings, variables,
    // keys and application data, plus deployment defaults. Export and import
    // are full pages of this section rather than dialogs: the export page
    // lists every value and every application-data folder that will be
    // written, and the import page starts blank and is filled in from a
    // chosen bundle before Deploy hands a validated request to the existing
    // durable Vultr provisioner.
    QWidget *buildSystemExportCard();
    QWidget *buildSystemImportCard();
    void showSystemExportPage();
    void showSystemImportPage();
    QJsonObject currentDeploymentDescriptor();
    void refreshSystemExportValues();
    void startSystemExportSurvey();
    void applySystemExportSurvey(
        const QList<forkmesh::control::CompleteSystemDataEntry> &entries);
    void updateSystemExportSummary();
    QStringList systemExportExcludedNames() const;
    void exportCompleteSystemBundle();
    void chooseSystemImportBundle();
    void applySystemImportBundle(const QJsonObject &bundle, const QString &path);
    void clearSystemImportForm();
    void deployImportedSystemBundle();
    void setSiteDeployRunning(bool running);
    void appendSiteDeployOutput(const QString &text);
    void connectToDeployedRelay(const QString &hostname);
    // API token tab: check a Cloudflare token against the exact
    // permissions the deploy path needs, and mint a correctly scoped
    // replacement into this device's variables and app/
    // env.production. Token values live in the password edit, the in-memory
    // redaction copy and those two stores only.
    QWidget *buildCloudflareTokenCard();
    QString resolvedCloudflareApiToken() const;
    void testCloudflareApiToken();
    void checkCloudflareTokenPolicies(const QString &token,
                                      const QString &tokenId);
    void resolveCloudflareTokenTopology(const QString &token);
    void runCloudflareTokenProbe(const QString &token, int index);
    void renderCloudflareTokenReport();
    void setCloudflareTokenBusy(bool busy);
    void endCloudflareTokenRun();
    void appendCloudflareTokenOutput(const QString &text);
    void generateCloudflareApiToken();
    void adoptRotatedCloudflareToken(const QString &token);
    // Persist a validated Cloudflare token in both credential stores used by
    // the Qt app and deployment scripts.
    QStringList rememberCloudflareApiToken(const QString &token,
                                           QString *error = nullptr);
    // First-instance-owner community reward-pool signer. The Solana private key
    // is imported into an encrypted local vault and never leaves this desktop;
    // the Worker only authors public intents and records public reconciliation.
    QWidget *buildRewardPoolControlCard();
    void fetchContributorRewards();
    void fetchContributorRewardsForAutomation();
    void fetchContributorRewardsRequest(bool automated);
    void renderContributorRewards(const QJsonArray &rewards);
    void continueContributorPayoutRun();
    void paySelectedContributorReward();
    // The CFO teammate is a native, fail-closed workflow. It can use only the
    // desktop-linked tip wallet, only after explicit session arming, and it
    // never hands financial authority to an agent prompt.
    void runContributorPayoutTeammate(const QString &teammateId,
                                      const QString &trigger);
    void toggleContributorAutoPayouts();
    bool armContributorAutoPayouts();
    void finishContributorPayoutTransfer(bool paid,
                                         const QString &rewardId,
                                         quint64 lamports,
                                         const QString &detail);
    void refreshRewardPoolControls();
    void importRewardPoolKey();
    void saveRewardPoolRpcConfiguration();
    void fetchRewardPoolIntents();
    void reviewAndSignRewardIntent();
    // Microjob payouts are fixed, tiny, already-verified amounts, so the owner
    // may arm them once per session instead of approving each one. Arming is
    // explicit, holds the passphrase only in memory, applies only to the
    // microjob_payout kind, and stops at a hard daily lamport cap.
    void toggleMicrojobAutoPayouts();
    void autoSignMicrojobPayouts();
    // A non-empty sessionPassphrase signs without prompting; the interactive
    // path passes none and asks for the passphrase for that one transaction.
    void prepareRewardPoolTransaction(const QJsonObject &job,
                                      const QString &sessionPassphrase = {});
    void submitRewardPoolTransaction(const QJsonObject &intent,
                                     const QString &signedTransactionBase64,
                                     const QString &chainSignature);
    void submitRewardSignatureReceipt(const QString &intentId,
                                      const QString &chainSignature,
                                      bool finalizationObserved);
    void pollRewardPoolFinalization(int attempt);
    void reconcileRewardPoolIntent(const QString &intentId,
                                   const QString &chainSignature,
                                   const QString &observedStatus,
                                   const QString &slot);
    QUrl rewardPoolWorkerEndpoint(const QString &path) const;

    // Member-to-member tips (MainWindowTips.cpp). The user links their own
    // Solana key into a passphrase-encrypted local vault; this desktop builds
    // and signs one System Program transfer and broadcasts it to a public RPC.
    // The relay is told only the resulting public chain signature, so it can
    // email the recipient. See forkmesh::tips.
    void refreshTipWalletControls();
    void linkTipWallet();
    void unlinkTipWallet();
    // Hands the vault's secret key back to its owner, behind the passphrase,
    // so a wallet generated here can be managed anywhere else.
    void exportTipWalletSecret();
    void showTipSecretKey(const QString &address, const QByteArray &keypairJson,
                          const QString &base58Secret);
    void sendTipToProfileMember();
    // reportToLedger: this transfer was started from the Ledger page, so its
    // running commentary belongs on that page's status line as well as in the
    // system log. See tipStatus() and payLedgerRecord().
    void sendTipToMember(const QString &recipientName,
                         const QString &recipientAddress,
                         quint64 fixedLamports = 0,
                         const QString &rewardId = QString(),
                         bool reportToLedger = false);
    // "Send tip" in the Users directory header, acting on the selected row.
    void sendTipToSelectedUser();
    void refreshUsersTipButton();
    bool selectedUsersTipTarget(QString *name, QString *address) const;
    void startTipTransfer(const QString &recipientName,
                          const QString &recipientAddress, quint64 lamports,
                          const QString &rewardId = QString(),
                          bool automated = false);
    // One reviewed manual payment, with everything its transfer reads once it
    // owns the wallet. A payment approved while another transfer is in flight
    // waits in m_tipTransferQueue as one of these and is signed as soon as
    // that transfer ends. The passphrase has exactly one owner: always move a
    // request, never copy it, since secureErase() only wipes its own copy.
    struct TipTransferRequest {
        QString recipientName;
        QString recipientAddress;
        quint64 lamports = 0;
        QString rewardId;
        bool reportToLedger = false;
        QString ledgerRecordId; // the Ledger row that shows its progress
        QString passphrase;     // empty: the signer asks when its turn comes
        QString taskDescription;
        QUrl outputUrl;
        QString enteredCurrency = QStringLiteral("SOL");
        qint64 enteredUsdMicros = 0;
        qint64 solUsdPriceMicros = 0;
        qint64 priceObservedAt = 0;
    };
    void beginOrQueueTipTransfer(TipTransferRequest &&request);
    void startTipTransferRequest(TipTransferRequest &&request);
    void drainTipTransferQueue();
    // The held receipt is the one the transfer in flight saved for itself, so
    // it holds nothing a queued payment has to be refused over yet.
    bool heldPayoutIsInFlight(
        const ContributorPayoutReceipt::Pending &held) const;
    bool promptTipPassphrase(const QString &recipientName, quint64 lamports,
                             QString *passphrase);
    void signAndBroadcastTip(const QString &recipientName,
                             const QString &recipientAddress,
                             const QString &blockhash,
                             quint64 lastValidBlockHeight, quint64 lamports,
                             int endpointIndex,
                             const QString &rewardId = QString(),
                             bool automated = false);
    void announceTip(const QString &recipientName, quint64 lamports,
                     const QString &chainSignature,
                     const QString &sourceAddress = QString(),
                     const QString &rewardId = QString(),
                     bool automated = false, int attempt = 0);
    // `automated` keeps the CFO retry cadence; an explicit Ledger retry uses
    // the shorter interactive path and reports progress back to that page.
    bool resumePendingContributorReceipt(bool automated = true,
                                         bool reportToLedger = false);
    bool savePendingContributorReceipt(const QString &recipientName,
                                       quint64 lamports,
                                       const QString &chainSignature,
                                       const QString &sourceAddress,
                                       const QString &rewardId,
                                       bool automated);
    void clearPendingContributorReceipt(const QString &rewardId,
                                        const QString &chainSignature);
    // Last resort before a broadcast payout wedges the queue: ask the public
    // chain what the relay could not be asked. A finalized signature settles
    // the receipt without any ForkMesh account, and a finalized failure proves
    // nothing moved. See ContributorPayoutReceipt::Settled.
    void settlePendingContributorPayoutOnChain(const QString &recipientName,
                                               quint64 lamports,
                                               const QString &chainSignature,
                                               const QString &sourceAddress,
                                               const QString &rewardId,
                                               bool automated,
                                               int endpointIndex = 0);
    bool recordSettledContributorPayout(const QString &recipientName,
                                        quint64 lamports,
                                        const QString &chainSignature,
                                        const QString &sourceAddress,
                                        const QString &rewardId,
                                        const QUrl &endpoint);
    // Send the notices those settlements owe, one at a time, whenever a
    // session exists to send them with. Never signs or broadcasts anything.
    // announce = a reader pressed Report to ForkMesh and is waiting for the
    // answer on the Ledger's status line. The background passes stay silent.
    void flushSettledContributorNotices(bool announce = false);
    // Authorize one payout notice, by session token if this app has one and
    // otherwise by signing the payout with the account key it always has.
    // False means neither credential was available. See TIP_NOTICE_PROOF.
    bool prepareContributorNoticeRequest(const QUrl &endpoint,
                                         const QString &recipientName,
                                         quint64 lamports,
                                         const QString &chainSignature,
                                         const QString &rewardId,
                                         const QString &sourceAddress,
                                         QNetworkRequest *request);
    // The modeless warning a refused reward payment raises, naming the payout
    // that is holding the queue. See ContributorPayoutReceipt.h.
    void showHeldContributorPayoutAlert(
        const ContributorPayoutReceipt::Pending &pending,
        const QString &attemptedRecipient = QString());
    void tipStatus(const QString &message);

    QWidget *buildHostsSection();
    // Hosts live view: every saved host side by side, each pane streaming the
    // host's node log and its sampled network activity (TCP connections opened
    // and closed, interface throughput) over its own parallel SSH session.
    // Streams run only while the view is on screen.
    QWidget *buildHostsLiveView();
    void setHostsLiveViewActive(bool active);
    void startHostsLiveView();
    void stopHostsLiveView();
    // The main log's own feed from every saved host: one background SSH stream
    // per host carrying its node log and whole journal into the main log,
    // reconnecting with backoff, whether or not the Hosts page is open. Called
    // once after startup and again whenever the saved host list is shown, so
    // added hosts start streaming and removed ones stop.
    void syncHostLogFeeds();
    void launchHostLogFeed(const QString &key);
    void forwardHostLogLine(const QString &node, int source, const QString &text);
    void syncCoreLogFeeds();
    void stopCoreLogFeeds();
    void startDeployLogFollower();
    void launchCoreLogFeed(const QString &node);
    void readCoreLogFeedOutput(const QString &node);
    void drainCoreLogFeedLines();
    void handleCoreLogFeedLine(const QString &node, const QByteArray &line);
    void emitCoreLogEntry(const QString &node,
                          const forkmesh::corelogs::JournalEntry &entry);
    void noteCoreLogWebRequestCounted();
    void ensureIpCountryTable();
    void loadIpCountryTable();
    QString accessWhoisLabel(const QString &address);
    forkmesh::corelogs::ClientDetails logClientDetails(const QString &address,
                                                       bool wantCountry,
                                                       bool wantNetwork);
    QString withLogClientDetails(const QString &text);
    void startAccessWhois();
    void saveCoreLogCursors(bool force);
    void armFleetInventoryWatch();
    void refreshFleetInventoryFromVultr();
    void scheduleFleetInventoryRefresh(qint64 delayMs);
    void saveFleetInventory();
    void showNodeLogFilterMenu();
    void updateNodeLogFilterButton();
    void scheduleCoreLogD1Refresh();
    bool confirmHostKeyOnFirstUse(const QString &host);
    void runHostInstall(bool forceUploadBinary = false,
                        std::function<void(bool)> onFinished = {},
                        bool reinstall = false, bool fromSource = false,
                        bool suppressFailureStatus = false);
    void runHostUninstall();
    void viewHostLogsForSelection(int row);
    void runHostLogSession(const QString &ip, const QString &user,
                          const QString &pass, const QString &node);
    // Browse a saved host's disk usage over its authenticated SSH channel: one
    // read-only `du` level per directory, so the biggest consumers of the
    // host's disk can be drilled into without leaving the app.
    void browseHostDiskUsageForSelection(int row);
    void runHostDiskUsageBrowser(const QString &ip, const QString &user,
                                 const QString &pass, const QString &node);
    void promptHostDiskSshPassword(
        QPointer<QDialog> guard, QLabel *status,
        const std::shared_ptr<std::function<void(const QString &)>> &loadPath,
        const std::shared_ptr<QString> &sessionPass,
        const QString &credentialKey, const QString &path, const QString &ip,
        const QString &user);
    // Configure a saved mirror host's Actions executor over its authenticated
    // SSH channel. Secret values are collected in a one-shot dialog and sent
    // only in a bounded JSON stdin payload; they are never saved in QSettings
    // or placed in process arguments/logs.
    void configureHostActionsForSelection(int row);
    // Install the official user-scoped Claude Code and Codex CLI binaries on
    // one selected mirror over its existing TOFU-pinned SSH connection.
    // Authentication is intentionally separate and never copied by this action.
    void installAgentClisForHost(int row);
    // Open a live in-app terminal on one mirror so the provider sign-ins an
    // install without copied credentials skips (`claude` then /login,
    // `codex login`) can be completed by hand. Opened automatically once such
    // an install finishes, and available on demand from the host row.
    void openHostAgentLoginTerminalForSelection(int row);
    void openHostAgentLoginTerminal(const QString &node, const QString &ip,
                                    const QString &user,
                                    const QString &provider = QString());
    // Shared driver behind that button and the one-click Vultr flow's "also
    // install the agent CLIs" option. With copyCredentials the
    // run additionally hands the mirror this device's provider logins on the
    // SSH session's stdin, so it comes up able to run agent sessions; without
    // it only the binaries are installed. onFinished(ok, message) reports the
    // outcome; the caller owns whatever it puts on screen.
    void runAgentCliInstall(const QString &node, const QString &ip,
                            const QString &user, const QString &sshPassword,
                            const QString &identityFile, bool copyCredentials,
                            std::function<void(bool, QString)> onFinished);
    // This device's copyable agent logins: the Claude Code and Codex CLI
    // credential files, plus the Settings API key for a provider with no CLI
    // login (a key alongside a login makes the CLI warn and switch billing).
    static forkmesh::control::AgentCliCredentials localAgentCliCredentials();
    void runHostActionsConfiguration(
        forkmesh::control::MirrorActionsConfigurationRequest request,
        const QString &sshPassword);
    void runHostInstallAllFromBinary();
    void runHostReinstallAllFromBinary();
    void runHostUpdateAllFromSource();
    void appendHostInstallLog(const QString &text);
    enum class FleetDeployMode { InstallBinary, Reinstall, UpdateSource };
    struct PublishedReleaseDeployment {
        QString tag;
        QString version;
        QString buildCommit;
        QString manifestSha256;
        QString linuxX86_64Sha256;
        QString linuxArm64Sha256;
        QStringList artifactSha256s;

        bool isValid() const
        {
            return !tag.isEmpty() && !version.isEmpty() &&
                   !buildCommit.isEmpty() && !manifestSha256.isEmpty() &&
                   !artifactSha256s.isEmpty() &&
                   (!linuxX86_64Sha256.isEmpty() ||
                    !linuxArm64Sha256.isEmpty());
        }
    };
    struct FleetDeployOptions {
        bool uploadBinary = false;
        bool reinstall = false;
        bool fromSource = false;
        bool requirePublishedBinary = false;
        bool activeHostsOnly = false;
        PublishedReleaseDeployment publishedRelease;
    };
    static FleetDeployOptions fleetDeployOptions(FleetDeployMode mode);
    struct HostDeploySession {
        QString node;
        QString ip;
        QString user;
        QString pass;
        QProcess *proc = nullptr;
        QPlainTextEdit *log = nullptr;
        QLabel *header = nullptr;
        QString ansiCarry;
        int ansiFg = -1;
        bool ansiBold = false;
        QString linkTail;
        bool linkPrompted = false;
        bool finished = false;
    };
    void runHostDeployAllParallel(FleetDeployMode mode);
    void runHostDeployAllParallel(
        FleetDeployMode mode,
        const PublishedReleaseDeployment &publishedRelease);
    void startHostDeploySession(HostDeploySession *session,
                                const FleetDeployOptions &options);
    void appendHostDeployLog(HostDeploySession *session, const QString &text);
    void onHostDeploySessionFinished(HostDeploySession *session, bool ok);
    bool buildHostInstallCommand(const QString &ip, const QString &user,
                                 const QString &node, bool uploadBinary,
                                 bool reinstall, bool fromSource,
                                 bool requirePublishedBinary,
                                 QString *remoteCmd, QByteArray *uploadBytes,
                                 QString *errorOut,
                                 const PublishedReleaseDeployment
                                     &publishedRelease = {});
    bool buildVultrMirrorNodeInstallCommand(const QString &node,
                                            const QString &publicAddress,
                                            QString *remoteCmd,
                                            QByteArray *uploadBytes,
                                            QString *errorOut) const;
    void addHostFromForm();
    void rememberHost(const QString &name, const QString &ip, const QString &user,
                      const QString &pass, const QString &status = QStringLiteral("installed"),
                      const QString &identityFile = QString(),
                      const QJsonObject &metadata = QJsonObject());
    QString savedHostIdentityFile(const QString &name, const QString &ip,
                                  const QString &user) const;
    // Rewrites a saved host's stale "install failed"/"installing" label to
    // "installed" when mirrors (catalog rows or verified health proofs) show
    // its node healthy. Writes QSettings only when a status changes.
    void promoteSavedHostsProvenHealthy(const QJsonArray &mirrors,
                                        const QString &evidence);
    void refreshHostsTable();
    void probeSavedHosts();
    void updateHostsFleetSummary();
    void refreshFleetNodesTable();
    void updateAddCoreNodeButton();
    void ensureAddCoreNodeController();
    void openAddCoreNodeDialog();
    void ensureRemoveCoreNodeController();
    void openRemoveCoreNodeDialog(const QString &name);
    void refreshRunners(std::function<void()> then = {});
    void renderRunnersTable();
    void refreshRunnerAttention(bool force = false);
    void setRunnerAttention(
        const QList<forkmesh::runners::RunnerAttentionItem> &items);
    void openRunnerAttention();
    bool openRunnerSignInLink(const QUrl &url);
    void requestRunnerAccountChange(
        const QString &runner, const QString &provider,
        const forkmesh::runners::RunnerAccountInfo &account,
        const QString &action);
    void fillAgentRunnerAccountsMenu(QMenu *menu, const QString &provider);
    void refreshAgentRunnerAccountMenus();
    void openRunnerAgentLoginTerminal(const QString &runner,
                                      const QString &provider,
                                      std::function<void()> onSignedIn = {});
    void withRunnerForCloudAgent(
        const forkmesh::agents::CloudAgentSession &cloud,
        std::function<void(const QString &)> then);
    void probeSavedHost(const QString &name, const QString &ip,
                        const QString &user, const QString &savedStatus);
    void forgetHostAtRow(int row);
    // Tombstones for hosts deleted here, so the Mirror nodes panel stops
    // listing a server this app destroyed while the relay's freshness lease
    // still reports its last signed catalog row as online and healthy.
    const QJsonObject &decommissionedMirrorNodes();
    void recordDecommissionedMirrorNode(const QString &node);
    void clearDecommissionedMirrorNode(const QString &node);
    bool mirrorNodeWasDecommissioned(const QString &node, qint64 lastSeenMs);
    void destroyVultrHostAtRow(int row);
    // The destroy behind the row's button, after its confirmation: also what
    // the owner's phone runs after its typed-name confirmation.
    void destroyVultrHostNamed(const QString &name,
                               std::function<void(bool ok, QString message)>
                                   onDone = {});
    void sendVultrInstanceDestroy(const QString &apiKey,
                                  const QString &instanceId,
                                  const QString &name,
                                  std::function<void(bool ok, QString message)>
                                      onDone = {});
    void createVultrMirrorFromForm();
    // Six-stage durable deployment UI/state. The non-secret checkpoint is
    // written after every transition and restored when Hosts is rebuilt, so a
    // restarted desktop resumes boot polling, installation, or public mirror
    // verification instead of starting another billable instance.
    void setVultrProvisionStage(int stage, const QString &detail = QString(),
                                bool failed = false);
    void pingVultrProvisionStage(int stage, bool ok,
                                 const QString &detail = QString());
    void renderVultrProvisionProgress(bool failed = false);
    void recordVultrStageDetail(int stage, const QString &detail);
    void resetVultrStageDetails();
    void persistVultrProvisionState(const QString &state = QStringLiteral("active"),
                                    const QString &message = QString());
    void restoreVultrProvision();
    void resumeVultrProvision();
    void endVultrProvision();
    void updateVultrEndDeploymentButtonVisibility();
    QString vultrProvisionLogPath() const;
    void saveVultrProvisionLog();
    void scheduleVultrProvisionLogSave();
    void findVultrProvisionInstance(
        const QString &apiKey, const QString &node,
        std::function<void(QString instanceId, QString error)> onDone);
    // Persist a Vultr API key Vultr itself has just accepted, in the two places
    // this app reads provisioning credentials from: the canonical
    // VULTR_API_KEY device variable (Settings > Variables / Secrets, injected
    // into every action run) and app/.env.production beside the
    // Cloudflare deploy credentials. Returns where it was written — empty when
    // the key is unusable or already stored everywhere — and reports a failed
    // file write through *error without undoing the variable that succeeded

    QStringList rememberVultrApiKey(const QString &apiKey,
                                    QString *error = nullptr);
    void vultrApiCall(const QString &apiKey, const QString &path,
                      const QByteArray &method, const QJsonObject &body,
                      std::function<void(QJsonObject, QString)> onDone);
    // Generate — or adopt, on a device that already has one — the single SSH
    // key the whole host fleet authorizes. Every provisioning and deploy path
    // authenticates with it; the private half never leaves this device.
    void ensureSharedHostKeypair(
        std::function<void(QString privateKeyPath, QString publicKey,
                           QString error)> onDone);
    void resolveVultrSshKeyId(
        const QString &apiKey, const QString &publicKey,
        std::function<void(QString keyId, QString error)> onDone);
    void pollVultrInstance(const QString &apiKey, const QString &instanceId,
                           const QString &node, const QString &identityFile);
    void waitForVultrSshReady(const QString &node, const QString &ip,
                              const QString &identityFile);
    void startVultrHostInstall(const QString &node, const QString &ip,
                               const QString &identityFile);
    // Do not report a provisioned mirror as complete merely because SSH and
    // systemd succeeded. Verify its signed flagship repository proof directly
    // against the mirror's HTTPS origin, without depending on the relay.
    void waitForVultrMirrorPublication(const QString &node,
                                       const QString &successMessage,
                                       int attempt = 0);
    // Push every head and tag from this node's local bare mirror into the
    // freshly installed host over the provisioning SSH identity. When the
    // public mesh is healthy the push is a cheap no-op against the node's own
    // clone; during a total outage it is the only way the node can ever get
    // repository content, so provisioning no longer depends on the mesh
    // being alive. onDone(false) on any failure — seeding is best-effort and
    // verification stays the arbiter.
    void runVultrMirrorSeedPush(std::function<void(bool)> onDone);
    void seedVultrMirrorRepositoryThenVerify(const QString &node,
                                             const QString &successMessage);
    // Ask a mirror that has not published yet why. A node whose first clone
    // failed serves nothing and says nothing to the catalog, so verification
    // would otherwise stall with no visible cause; this reads the node's own
    // service state and last sync warning over SSH, and optionally restarts the
    // service to force a fresh clone attempt. Reports an empty string when the
    // host cannot be reached or has no saved address.
    void diagnoseVultrMirrorNode(const QString &node, bool restartService,
                                 std::function<void(QString)> onDone);
    // One command on a mirror node as root over its saved (or the shared)
    // host key: the SSH transport diagnoseVultrMirrorNode uses. `onDone` gets
    // the redacted tail of the output; ok is a clean exit within `timeoutMs`.
    void runMirrorNodeSsh(const QString &node, const QString &remoteCommand,
                          int timeoutMs,
                          std::function<void(bool ok, QString output)> onDone,
                          bool preferProvisioningHost = false);
    QString savedHostAddressNamed(const QString &node) const;
    void finishVultrProvision(bool ok, const QString &message);
    void appendVultrAttemptHistory();
    void cloudflareApiCall(const QString &apiToken, const QString &path,
                           const QByteArray &method, const QJsonObject &body,
                           std::function<void(QJsonObject, QString)> onDone);
    // Point "<node>.<zone>" at a freshly booted Vultr instance so it joins the
    // mesh under a stable name like the other mirrors. Reports the hostname it
    // provisioned, or an empty string when no Cloudflare credentials/zone are
    // configured or the record could not be written — provisioning continues
    // either way, the node just keeps its raw address.
    void ensureVultrMirrorDns(const QString &node, const QString &ip,
                              std::function<void(QString hostname)> onDone);
    void removeVultrMirrorDns(const QString &node,
                              std::function<void(QString outcome)> onDone);
    void loadHostIntoForm(int row, int column);
    QString installScriptUrl() const;
    QString uninstallScriptUrl() const;

    void refreshRelayNodes();
    void probeRelayRow(int row);
    void updateRelayNodeRows();
    void refreshNodesStatusLine();
    void fillRelayNodeRow(int row, int serverIndex);
    void fillRunnerNodeRow(int row, const forkmesh::nodedir::RunnerRow &runner,
                           qint64 nowMs);
    QList<forkmesh::nodedir::RunnerRow> directoryRunnerRows() const;
    void activateNodeDirectoryRow(int row);
    void showNodesForRole(const QString &roleKey);
    void openNodeLogsFor(const QString &feedName);
    QWidget *buildNodesSection();
    void refreshNodesTable();           // re-list the known nodes into the table
    void mergeMirrorRowsIntoNodesTable();
    void sortNodesTable();
    bool mirrorNodesOnScreen() const;
    void refreshNodeActionButtons();
    // --- Delete a node for good ----------------------------------
    // The Nodes page's Delete button: the same permanent removal the World
    // panel performs, plus the provider teardown the web has no credentials
    // for. In order — destroy the Vultr instance behind the node, drop its
    // Cloudflare DNS record, remove every trace of it from the relay (accounts,
    // mirrors, agent jobs and the /status history), then forget the saved SSH
    // host locally. The provider steps are best-effort and never stop the mesh
    // removal; the mesh removal itself is the one step that must succeed.
    void deleteMeshNodeCompletely(const QString &node, const QString &nodeId);
    void destroyVultrServerForNode(const QString &node,
                                   std::function<void(QString outcome)> onDone);
    void sendNodeVultrDestroy(const QString &apiKey, const QString &instanceId,
                              const QString &node,
                              std::function<void(QString outcome)> onDone);
    // POST /api/world/admin/nodes/delete. A desktop that signed in with a
    // password holds a session token; the ordinary launch authenticates
    // silently and holds keys only, so this falls back to the signed
    // node/ts/sig proof the worker accepts for this one deletion.
    void sendMeshNodeDeleteRequest(const QString &node, const QString &nodeId);
    void forgetSavedHostNamed(const QString &node);
    void setNodeDeleteStatus(const QString &text);
    // Separate from the host ledger: merely removing a saved SSH host must not
    // hide its still-registered node from the Nodes directory.
    const QJsonObject &deletedMeshNodes();
    void recordDeletedMeshNode(const QString &node);
    void clearDeletedMeshNode(const QString &node);
    bool meshNodeWasDeleted(const QString &node, qint64 lastSeenMs);
    // Fetch the relay's list of currently-online node names (/api/network/stats
    // "onlineNodes": repository update channel or fresh signed heartbeat). Headless
    // mirror nodes serve through the relay without joining this client's chat
    // room, so room presence alone painted them offline.
    void fetchRelayOnlineNodes(bool force = false);
    void fetchNodesCatalogInfo(bool force = false);
    void applyCatalogNodeInfo(MemberInfo &info, const QString &node) const;
    QWidget *buildFirewallSection();
    void refreshFirewallTables();
    // Opens the log filtered to the traffic a rule covers.
    void openLogForFirewallRule(const QString &rule);
    // Rewrites only the count cells and the status line, so live traffic
    // keeps them current without rebuilding the grid under a selection.
    void updateFirewallRuleCounts();
    void addFirewallRuleFromEdit(bool deny = false);
    void removeSelectedFirewallRules();
    // Opens a rule for editing: change its pattern, flip it between allow and
    // deny. Given the request a decision was about, it edits whatever rule
    // decides that request now and offers the prompt's scope ladder.
    void editFirewallRule(const QString &rule, bool denied,
                          const QString &method = QString(),
                          const QUrl &url = QUrl());
    void editFirewallRuleForDecision(int row);
    // Double-click and right-click editing on the rules and decisions grids.
    void installFirewallRuleEditing(QPushButton *editDecisionButton = nullptr);
    void clearFirewallHistory();
    BackoffNetworkAccessManager::FirewallVerdict
    promptFirewallRequest(const QString &method, const QUrl &url,
                          QString *ruleOut);
    bool authorizeFirewallConnection(const QString &method, const QUrl &url);
    // Says, once a session, that this build is older than the minimum
    // supported release and that is why nothing reaches the network.
    void warnOutdatedClientVersion();
    void recordFirewallRequest(const QString &method, const QUrl &url,
                               const QString &rule, bool allowed);
    // Network diagnostics: live websocket / Durable Object details, plus the
    // Relays / Nodes / Hosts pages as its first tabs.
    QWidget *buildNetworkDiagnosticsSection();
    void refreshNetworkDiagnostics();
    void updateNetworkCounts(int relays, int nodes, int hosts);
    // Green lamp on the Network rail button while a mirror node is being stood
    // up, mirroring what the Control rail shows during a site deploy.
    void updateNetworkDeployBlink();
    void showNetworkTab(int tabIndex);
    void openFirewallRules(const QString &rule);
    void refreshNetworkTab(int tabIndex);
    void refreshCurrentNetworkTab(); // the page header's one Refresh button
    void showEndpointRequestDetails(int row, int column);
    void refreshNetworkWebRequests();
    void renderNetworkWebRequests(const QJsonObject &payload);
    void openLogForWebRequestsRow(int row);

    QWidget *buildUsersSection();
    void refreshUsersPage(bool force = false);
    void renderUsersPage(const QJsonArray &users);
    // One Users-table row, column group by column group (renderUsersPage).
    void fillUsersIdentityCells(int row, const QJsonObject &user,
                                const QString &name);
    void fillUsersReferralCells(int row, const QJsonObject &user);
    void fillUsersSignupCells(int row, const QJsonObject &user);
    void fillUsersActivityCells(int row, const QJsonObject &user);
    void renderUsersQuarantinePage(const QJsonArray &users);
    void renderUsersSignupReferralCode(const QJsonObject &state);
    void updateUsersStatus();
    QStringList selectedUsersActionTargets() const;
    void removeUsersFromActiveDirectory(const QStringList &targets);
    void refreshUsersQuarantineButton();
    void quarantineSelectedUsers();
    void quarantineNextSelectedUser();
    void finishUsersQuarantineBatch();
    void refreshUsersUnquarantineButton();
    void unquarantineSelectedUser();
    void refreshUsersDeleteButton();
    void deleteSelectedUsers();
    void deleteNextSelectedUser();
    void finishUsersDeleteBatch();

    QWidget *buildFilesSection();
    QLayout *buildFilesEditorBar();

    // Stats: a BI-style grid of every number the app already tracks, as
    // tiles with big values and small charts. Implemented in MainWindowStats.cpp.
    QWidget *buildStatsSection();
    void refreshStatsPage();

    // Component health: one tiny card per source file, drilling into that
    // file's functions. The scan runs on a worker (runOffThread) behind a
    // progress row; implemented in MainWindowComponentHealth.cpp.
    QWidget *buildComponentHealthSection();
    void refreshComponentHealth(bool force = false);
    void showComponentHealthOverview();
    void showComponentHealthFile(const QString &path);
    void updateComponentHealthStatus();
    void showComponentHealthView(int view);
    void refreshBinarySize(bool force = false);
    void updateBinarySizeCrumbs(int focus);

    // Memory: every mapping the kernel has for this process, grouped, with the
    // allocator's split and the app's own cache counts, sampled on a worker
    // while the page is on screen; implemented in MainWindowMemory.cpp.
    QWidget *buildHomeGridSection();
    void refreshHomeGridPage();
    bool homeGridOnScreen() const;
    QVector<HomeGroup> homeGridGroups() const;
    QString homeGridTileDetail(const QString &key, QWidget *widget) const;
    QString homeGridHeadline() const;
    bool activateHomeGridTile(const QString &key);
    void showHomeGridSearch(const QString &typed = QString());
    void hideHomeGridSearch();
    HomeBoard *m_homeGridBoard = nullptr;
    HomePageDots *m_homeGridDots = nullptr;
    QWidget *m_homeGridSearch = nullptr;
    QLabel *m_homeGridGreeting = nullptr;
    QLabel *m_homeGridHeadline = nullptr;
    QLineEdit *m_homeGridFilter = nullptr;
    QTimer *m_homeGridTimer = nullptr;
    QWidget *buildMemorySection();
    void refreshMemoryPage();
    void sampleMemoryPage();
    bool memoryPageOnScreen() const;
    void applyMemorySample(const forkmesh::memory::ProcessMemorySnapshot &snapshot);
    void installMemoryProbes();
    void openMemoryMap();
    MemoryUsageChart *m_memoryChart = nullptr;
    QTreeWidget *m_memoryTree = nullptr;
    QTreeWidget *m_memoryAppTree = nullptr;
    QLabel *m_memoryStatus = nullptr;
    QLineEdit *m_memoryFilter = nullptr;
    QComboBox *m_memoryInterval = nullptr;
    QPushButton *m_memoryPause = nullptr;
    QTimer *m_memoryTimer = nullptr;
    QHash<QString, QLabel *> m_memoryTiles;
    QHash<QString, QLabel *> m_memoryTileNotes;
    // Key -> value at the previous sample, for the change column.
    QHash<QString, qint64> m_memoryPrevious;
    qint64 m_memoryPreviousAtMs = 0;
    bool m_memorySampling = false;
    bool m_memoryProbesInstalled = false;
    QString m_memoryLastReport;
    QWidget *buildGovernorSection();
    void refreshGovernorPage();
    void sampleGovernorPage();
    bool governorPageOnScreen() const;
    void applyGovernorSample(
        const forkmesh::agentresources::GovernorSnapshot &snapshot,
        const forkmesh::agentresources::SliceCounters &slice,
        const forkmesh::SystemPressureSnapshot &machine);
    QLabel *m_governorStatus = nullptr;
    QLabel *m_governorSummary = nullptr;
    QHash<QString, QLabel *> m_governorTiles;
    QHash<QString, QLabel *> m_governorTileNotes;
    QTreeWidget *m_governorUnits = nullptr;
    QTreeWidget *m_governorEvents = nullptr;
    QTreeWidget *m_governorSettings = nullptr;
    QTimer *m_governorTimer = nullptr;
    bool m_governorSampling = false;
    QString m_governorLastReport;
    ComponentHealthBoard *m_componentHealthBoard = nullptr;
    QScrollArea *m_componentHealthScroll = nullptr;
    QLabel *m_componentHealthStatus = nullptr;
    QWidget *m_componentHealthProgressRow = nullptr;
    QProgressBar *m_componentHealthProgress = nullptr;
    QLabel *m_componentHealthProgressLabel = nullptr;
    QPushButton *m_componentHealthBack = nullptr;
    QLineEdit *m_componentHealthFilter = nullptr;
    QComboBox *m_componentHealthSort = nullptr;
    QTimer *m_componentHealthProgressTimer = nullptr;
    forkmesh::codehealth::Report m_componentHealthReport;
    QString m_componentHealthOpenFile;
    bool m_componentHealthScanning = false;
    // Written by the scan worker, read by the GUI-thread progress timer. Two
    // atomics rather than a queued signal per file: a 310-file scan then costs
    // a handful of repaints instead of 310 posted events.
    std::atomic_int m_componentHealthScanDone{0};
    std::atomic_int m_componentHealthScanTotal{0};
    QStackedWidget *m_componentHealthStack = nullptr;
    QButtonGroup *m_componentHealthViews = nullptr;
    QWidget *m_componentHealthSourceTools = nullptr;
    BinarySizeMap *m_binarySizeMap = nullptr;
    QLabel *m_binarySizeCrumbs = nullptr;
    bool m_binarySizeScanning = false;
    QVector<StatsTileCatalogEntry> statsTileCatalog() const;
    QVector<StatsTilePlacement> defaultStatsLayout() const;
    StatsTileData statsTileData(const QString &id);
    void saveStatsLayout();
    // Per-tick host samples (CPU, memory) so the system tiles have a trend
    // even though nothing else in the app keeps that history.
    void sampleStatsHistory();
    StatsDashboard *m_statsDashboard = nullptr;
    bool m_statsNetworkRefreshQueued = false; // live Network requests repaint
    bool m_firewallRuleCountsQueued = false; // live Firewall Rules counts
    bool m_outdatedClientVersionWarned = false; // update popup already raised
    QLabel *m_statsStatus = nullptr;
    QTimer *m_statsRefreshTimer = nullptr;
    QTimer *m_userActivitySyncTimer = nullptr;
    quint64 m_userActivitySentRevision = ~quint64(0);
    qint64 m_userActivitySyncedMs = 0;
    bool m_userActivitySyncBusy = false;
    QVector<double> m_statsCpuSamples;
    QVector<double> m_statsRssSamples;
    QVector<double> m_statsHostMemorySamples;
    QStringList m_statsSampleLabels;
    // Rolling readings for the realtime tiles, keyed by tile id, so a
    // right-now number (tool calls in flight, queue depth, relay latency)
    // has a trend over the last few minutes; same labels as the host samples.
    QHash<QString, QVector<double>> m_statsLiveSamples;
    qint64 m_statsLastNetworkRequestTotal = -1;
    // Ratchet Mode readings come from git config, so the Stats page reads
    // them off-thread and keeps the last answer per repository directory.
    struct StatsRatchetEntry {
        RepoRatchetReading reading;
        qint64 sampledAtMs = 0;
        bool inFlight = false;
    };
    QHash<QString, StatsRatchetEntry> m_statsRatchet;
    // The cached reading, kicking a refresh when stale; nullptr until the
    // first read lands.
    const RepoRatchetReading *statsRatchetReading(const QString &dir);
    void refreshFilesPage();
    // What the rail's Files button does: open the section rooted back at the
    // open repository's working tree, however deep the last visit drilled.
    void openFilesSectionAtRepoRoot();
    void showFilesRepoTree();
    void showFilesCoveTree();
    void showFilesDiskTree();
    // Show or hide the byte-size column on both explorer trees; off by default.
    void setFilesSizeColumnVisible(bool on);
    void openLocalFileTab(const QString &path);
    void commitFilesTabChanges();
    void updateFilesCommitActions();
    void clearRepoFileTabs();
    void setFileExplorerRoot(const QString &path);
    void populateFileExplorerItem(QTreeWidgetItem *item);
    QTreeWidgetItem *addFileExplorerRow(QTreeWidgetItem *parent,
                                        const QFileInfo &entry);
    QFileInfoList fileExplorerEntries(const QString &dir) const;
    void showFileExplorerMenu(const QPoint &pos);
    void previewFileExplorerFile(const QString &path, int line = 0);
    void revealFileInExplorer(const QString &path, int line = 0);
    void revealLogSourceInExplorer(const QString &relativePath, int line);
    void highlightFileExplorerPreviewLine(int line);

    // ---- Files editor: the IDE half of the page (adhoc #1607). Right-clicking
    // a symbol traces it through the repository — its definitions, every
    // occurrence, who calls it (backwards) and what it calls (forwards) — and
    // the usual editor conveniences sit alongside: a find/replace bar, go to
    // line, go to symbol, a tab context menu and a breadcrumb naming whichever
    // file the open tab is showing.
    QWidget *buildFilesEditorColumn();
    QWidget *buildFilesFindBar();
    QWidget *buildFilesSymbolPanel();
    // Wire an editor tab (or the preview) into the page: context menu, cursor
    // readout, and the find bar's document.
    void wireFilesEditor(QPlainTextEdit *editor);
    QPlainTextEdit *currentFilesEditor() const;
    // Repo-relative path of the open tab, empty for the preview or a tab on a
    // file outside the repository.
    QString currentFilesEditorRepoPath() const;
    void showFilesEditorMenu(QPlainTextEdit *editor, const QPoint &pos);
    void showFilesTabMenu(const QPoint &pos);
    // Close one editor tab (the permanent preview never closes), forgetting the
    // find-bar matches when they belonged to it.
    void closeFilesTab(int index);
    // The identifier under the editor's cursor (or its selection), "" when the
    // cursor is not on one.
    QString filesEditorSymbol() const;
    // Breadcrumb + "Ln n, Col n" readout, and the tree selection following the
    // open tab so the file being edited is always visible on the left.
    void updateFilesEditorStatus();
    void syncFilesTreeToOpenTab();
    void syncCoveTreeToOpenTab();

    // Symbol panel under the editor. `mode` is a forkmesh::code::TraceMode.
    void traceFilesSymbol(const QString &symbol, int mode);
    void runFilesSymbolQuery();
    void expandFilesSymbolRow(QTreeWidgetItem *item);
    void activateFilesSymbolRow(QTreeWidgetItem *item, int column);
    void closeFilesSymbolPanel();

    // Find / replace over the open tab.
    void openFilesFindBar(bool replace);
    void closeFilesFindBar();
    void recomputeFilesFind();
    void stepFilesFind(int delta);
    void replaceInFilesEditor(bool all);
    void goToLineInFilesEditor();
    void showFilesSymbolOutline();

    // Repo detail view (files + issues tabs), opened by clicking a repository.
    void ensureRepoDetailSectionBuilt();
    void ensureRepoDetailTabBuilt(int index);
    void ensureRepoOverviewBodyBuilt(int index);
    QWidget *buildRepoDetailSection();
    QWidget *buildRepoFilesPanel();
    QWidget *buildRepoOverviewPage();
    QWidget *buildFilesCovePanel();
    QWidget *buildRepoCommitsTab();
    void buildCommitsUnsyncedBanner(QWidget *listPage);
    QWidget *buildCommitDetailPage();
    void showCommit(const QString &hash); // open the commit diff detail view
    void syncCurrentCommitRow(const QString &hash);
    void renderCommitDetail(const QString &dir, const QString &hash,
                            const QStringList &metaFields,
                            const QByteArray &patchRaw);
    void showCommitList();                // reset the right pane to working changes
    void openMostRecentCommit();          // select newest commit + expand its diff
    void deleteCommit(const QString &hash);
    void revertCommit(const QString &hash);
    void showCommitsBanner(const QString &html);
    void hideCommitsBanner();
    void openCommitReference(int number);
    void openIssueReference(int number);
    void openPullReference(int number);
    void openCommitHashReference(const QString &hash);
    void openReferenceLink(const QString &href);
    // Browser-style find for the page currently shown in the central stack.
    // Code editors and diff review panes keep their richer local find bars;
    // openPageFind() routes to those before falling back to this dialog.
    void openPageFind();
    QWidget *pageFindRoot() const;
    QPointer<QObject> m_pageFindController;
    void openGlobalSearch();
    void runGlobalSearch(const QString &query);
    void rebuildGlobalSearchList(const QString &query);
    void activateGlobalSearchHit(const GlobalSearchHit &hit);
    // Branch / worktree / tag / tracked-path hits, read straight from git.
    // Static and member-free so both the Ctrl+K overlay and the full results
    // page can run it on their own worker thread from the one implementation.
    // repoIndex/repoLabel tag hits from a repository other than the open one
    // (the label prefixes each detail line); an empty ref skips tracked paths.
    static QVector<GlobalSearchHit> scanGitCatalogHits(const QString &gitDir,
                                                       const QString &ref,
                                                       const QString &query,
                                                       int capPerKind,
                                                       int repoIndex = -1,
                                                       const QString &repoLabel = {});
    // The same catalog read across every repository with a git dir on this
    // node, the open one first, so a branch or worktree is found without
    // opening its repository beforehand.
    static QVector<GlobalSearchHit> scanAllGitCatalogHits(
        const QVector<GlobalSearchGitRepo> &repos, int openIndex,
        const QString &openRef, const QString &query, int capPerKind);
    QVector<GlobalSearchGitRepo> globalSearchGitRepos() const;
    // Content search inside the open repository's worktrees, limited to the
    // files each one changed or added against ref — exactly the uncommitted
    // and branch-only work the current-tree code search cannot see.
    static QVector<GlobalSearchHit> scanWorktreeCodeHits(const QString &gitDir,
                                                         const QString &ref,
                                                         const QString &query,
                                                         int cap);
    static QVector<GlobalSearchHit> scanProjectFileHits(const QString &workDir,
                                                        const QString &query,
                                                        int cap);
    QDialog *m_searchDialog = nullptr;
    QLineEdit *m_searchInput = nullptr;
    QListWidget *m_searchList = nullptr;
    QLabel *m_searchStatus = nullptr;
    int m_searchGen = 0;
    QVector<GlobalSearchHit> m_searchHits;
    int m_searchWavesPending = 0;
    void openBodyReference(const QString &href);
    void openAgentTranscriptReference(const QString &href);
    // Repo-relative paths in `ref`'s tree that end with `path`, shallowest
    // first — how a transcript's bare or partial filename finds its file.
    static QStringList transcriptFilePathMatches(const QString &repoDir,
                                                 const QString &ref,
                                                 const QString &path);
    void copyReferenceLink(const QString &kind, const QString &id);
    void filterCommits(const QString &query);
    void downloadCommitPatch();           // save the open commit as a .patch file
    void updateDiffSplitButton(QPushButton *button);
    void addConversationCard(QVBoxLayout *layout, const QString &author,
                             const QString &headerHtml, const QString &body,
                             const QString &accent = QString(),
                             const QString &copyLink = QString(),
                             const QString &authorId = QString(),
                             const std::function<void()> &onDelete = {},
                             const QList<QPair<QString, std::function<void()>>>
                                 &extraActions = {});
    QWidget *buildRepoSecurityTab();
    QWidget *buildRepoQualityTab();
    QWidget *buildInsightsTab();
    QWidget *buildSizeMapTab();
    void refreshSizeMapTab(bool force, bool allowElevation = false);
    QString sizeMapRoot() const;
    void chooseSizeMapFolder();
    void setSizeMapRootOverride(const QString &path);
    QWidget *buildSizeMapVolumesPanel();
    void refreshSizeMapVolumes();
    QSet<QString> sizeMapPrunedPaths(const QString &path) const;
    static QSet<QString> readSizeMapIgnoredPaths(const QString &path);
    bool sizeMapIgnoredPathsReady(const QString &path,
                                  std::function<void()> resume);
    QString m_sizeMapIgnoredPath;
    QSet<QString> m_sizeMapIgnoredPaths;
    bool m_sizeMapIgnoredFresh = false;
    bool m_sizeMapIgnoredReading = false;
    QList<std::function<void()>> m_sizeMapIgnoredResumes;
    void rescanSizeMapElevated(bool upfront = false);
    void stopSizeMapScan();
    void showSizeMapScanProgress(
        const QString &root,
        const forkmesh::DirectorySizeScanProgressUpdate &update, bool elevated);
    void clearSizeMapWorkerLines();
    void applySizeMapResult(const QString &path,
                            forkmesh::DirectorySizeScanResult result,
                            bool hideIgnored, bool elevated);
    QWidget *buildPlaceholderTab(const QString &name);

    // Discussions tab (signed repository discussions with inbox fallback).
    QWidget *buildDiscussionsTab();
    DiscussionStore discussionStoreForCurrentRepo() const;
    void reloadDiscussions();
    void reloadDiscussionCountInBackground();
    void showDiscussion(int number);
    void renderDiscussionThread(const Discussion &discussion);
    void updateDiscussionActionState();
    void createDiscussionDialog();
    void postDiscussionComment();
    void deleteDiscussionComment(int number, const QString &eventId);
    void markDiscussionAnswer(int number, const QString &commentId);
    void setDiscussionStatus(const QString &status);
    void deleteCurrentDiscussion();
    void startDiscussionFromComposer();
    static QString discussionTitleFromBody(const QString &body);
    void submitDiscussionEventToInbox(int number, const DiscussionEvent &ev,
                                      const QString &titleIfNew = QString());
    QUrl discussionsApiUrl(const RepositoryRecord &repo) const;
    void syncDiscussionsInbox();
	void drainDiscussionsInboxFor(RepositoryRecord repo, bool interactive);
    void setDiscussionInlineNotice(const QString &message, bool isError = false);

    QWidget *buildPullsTab();
    QWidget *buildPullListPane(QWidget *page);
    QHBoxLayout *buildPullHeaderRow();
    void buildPullDiffOverlays();
    QWidget *buildPullFilesPage();
    void buildPullCommitsList();
    QWidget *buildPullChecksPage();
    QWidget *buildPullComposerBlock();
    QHBoxLayout *buildPullConversationActions();
    void buildPullConflictCards();
    void buildPullConversation();
    QHBoxLayout *buildPullSubTabs(QWidget *checksPage, QWidget *filesPage);
    PullStore pullStoreForCurrentRepo() const;
    void reloadPulls();
    void reloadPullsInBackground();
    void applyLoadedPulls(const PullStore &store, QList<PullRequest> pulls,
                          const QString &baseTip,
                          const QByteArray &snapshotDigest = QByteArray());
    void processPendingPullConflicts(quint64 gen);
    void queuePullConflictCheck(int number, const QString &fingerprint);
    QString pullBehindCacheKey(const PullRequest &pull) const;
    void queuePullBehindCheck(int number, const QString &cacheKey);
    void setPullConflictBadge(int number, bool conflict);
    void setPullViewedCount(int number, int viewed);
    // The comment that asks a pull request's author to resolve its merge
    // conflicts. It carries a hidden marker keyed to the patch, so any node can
    // tell this conflict was already raised and a new push earns a new notice.
    static QString pullConflictNoticeKey(const QString &patch);
    static bool pullHasConflictNotice(const PullRequest &pull);
    static QString pullConflictNoticeBody(const PullRequest &pull,
                                          const QStringList &conflictFiles);
    QString pullConflictNoticeSessionKey(const PullRequest &pull) const;
    // Posts that comment on pull `number` unless it is already there. Posted
    // automatically when the conflict probe sees a clean pull turn conflicted,
    // and by "Message author" on the Merge conflicts card otherwise. An empty
    // `fingerprint` skips the check that the patch is still the one probed.
    bool postPullConflictNotice(int number, const QString &fingerprint,
                                const QStringList &conflictFiles, bool automatic);
    // Reads how each open pull's head branch stands against its base and
    // whether it still has a checkout on disk, for the list's leading column.
    void refreshPullBranchState();
    void refreshPullList();
    void setPullListLoading(bool loading);
    void showPull(int number);
    void applyPullFileAuthorFilter();
    void renderPullDiff();
    bool handlePullDiffLinePointerEvent(QObject *obj, QEvent *event);
    void paintPullDiffLineSelection();
    bool branchDiffTakesBranchComments() const;
    const QList<PullEvent> &branchReviewEvents(const QString &branch);
    PullRequest branchReviewPull(const QString &patch);
    void reloadBranchReview();
    void onBranchReviewAnchorClicked(const QUrl &url);
    bool postBranchInlineComment(QString *error);
    bool finishBranchInlineComment(QString *error);
    void rerenderPullInlineDiff();
    void openPullInlineComposer();
    void positionPullInlineComposer();
    void cancelPullInlineComposer();
    void queuePullInlineComment();
    void submitPullInlineCommentNow();
    void submitPullInlineReviewBatch();
    void discardPullInlineReviewBatch();
    void updatePullInlineReviewBatchBar();
    bool postPullInlineComment(int number, const QString &path,
                               const QString &side, int lineStart,
                               int lineEnd, const QString &body,
                               const PullAnchor &anchor, QString *error);
    // The signed anchor (patchset + commented text) a new inline comment on
    // the pull currently shown should carry; empty parts when unknown.
    PullAnchor currentPullAnchor(int number, const QString &path,
                                 const QString &side, int lineStart,
                                 int lineEnd) const;
    // Fold/reopen one file of the pull diff in place; false when the document
    // cannot carry the swap and the caller has to re-render the whole diff.
    bool swapPullDiffFileInPlace(const QString &path);
    QString pullDiffSourceKey(const QSet<QString> &viewed,
                              const QSet<QString> &expanded) const;
    void scrollPullDiffToFile(const QString &filePath);
    void updatePullDiffScrollState();
    void selectPullFileInList(const QString &filePath);
    void layoutPullStickyHeader();
    void computePullFileTops();
    void applyAutoMarkViewedOnScroll();
    void adjustDiffFont(int delta);
    void registerDiffView(QTextEdit *view);
    void setDiffHtml(QTextEdit *view, const QString &html, bool endCap = true);
    void onDiffStreamFinished(QTextEdit *view);
    void pullSelectAdjacentChange(int delta);
    bool pullScrollToAdjacentHunk(int delta);
    void togglePullDiffSearch(bool show);
    void pullDiffSearchRecompute();
    void pullDiffSearchGoTo(int delta);
    void onPullDiffAnchorClicked(const QUrl &url);
    void submitPullThreadReply(const QString &threadId);
    void setPullThreadState(const QString &threadId, const QString &state);
    void renderPullThread(const PullRequest &pr);   // review/comment conversation
    void renderPullCommits(PullRequest pr);         // commits that make up the PR
    QList<PullActivityCommit> m_pullActivityCommits;
    int m_pullActivityCommitsNumber = 0; // PR the list above was collected for
    void refreshPullAgentActivity();
    QString pullAgentActivityDigest(const PullRequest &pr) const;
    QString m_pullAgentActivityDigest;
    // Commit and agent cards renderPullThread() added on top of the signed review
    // items, and the review-item count updatePullSubTabCounts() derived. Held so
    // the Conversation badge can be corrected after a live agent re-render
    // without repeating that function's git-backed tallies.
    int m_pullActivityExtraCards = 0;
    int m_pullActivityCardsNumber = 0; // PR the count above belongs to
    int m_pullConversationBaseCount = 0;
    bool m_pullActivityRefreshing = false;
    void renderPullChecks(PullRequest pr);          // action runs for the PR's commits
    void renderPullChecksSummary(PullRequest pr);   // inline conversation card
    void showPullCheckLog(int runId);               // load a run's log into the panel
    QStringList pullCommitShas(const PullRequest &pr) const; // base..head SHAs
    QList<int> runIdsForPull(PullRequest pr) const; // matching action runs
    // Resolve the signed historical head when it still exists, otherwise use
    // the durable canonical pr/<n> branch materialized for this PR. Takes a
    // value because the git probes pump the GUI event loop.
    QString resolvablePullHead(PullRequest pr) const;
    // The actual local branch a PR cleanup may delete. Cross-node heads are
    // signed as "node:branch" labels and must never be passed to `git branch`
    // verbatim; a head that was never materialized locally has nothing to
    // clean up and is not an error.
    QString localPullHeadForDeletion(PullRequest pr) const;
    // The PR record a delete works from. Falls back to a bare record carrying
    // just the number when the selection outlived its row in the loaded list,
    // so the teardown still reaches the folder and the canonical pr/<n> head.
    PullRequest pullRecordForDeletion(int number) const;
    struct PullRangeSnapshot {
        QString baseSha;
        QString headSha;
        QByteArray detailedLog; // renderPullCommits' rich per-commit format
        QStringList shas;       // %H list for pullCommitShas/runIdsForPull
        qint64 readAtMs = 0;
    };
    bool pullRangeSnapshot(const QString &base, const QString &head,
                           PullRangeSnapshot *out) const;
    mutable QHash<QString, PullRangeSnapshot> m_pullRangeCache;
    static PullRangeSnapshot readPullRangeSnapshot(const QString &dir,
                                                   const QString &base,
                                                   const QString &head);
    void loadPullRangeSnapshot(const QString &dir, const QString &base,
                               const QString &head);
    void rerenderOpenPullRange();
    QSet<QString> m_pullRangeLoading;
    QHash<QString, qint64> m_pullRangeMissAtMs;
    bool m_pullRangeRerenderQueued = false;
    void runChecksForCurrentPull();                 // enqueue workflows at PR head
    void approveChecksForCurrentPull();             // release a held first-PR run
    void buildAndPreviewCurrentPull();
    void previewPullAtCommit(const RepositoryRecord &repo, int number,
                             const QString &gitDir, const QString &commit);
    bool m_pullPreviewResolving = false;
    bool m_pullSuggestionApplying = false;
    void updatePullSubTabCounts(PullRequest pr);
    void refreshOpenPullChecks();                   // re-render checks for the open PR
    // Push and Release fire from the served mirror; PullRequest is the automatic
    // per-PR pass and matches `on: pull_request` only, so a deploy workflow
    // (`on: push`) never runs a contributor's proposed head. PullRequestManual
    // is the Checks tab's button, which keeps its long-standing meaning of "run
    // this repository's checks against the PR" and so also matches `on: push`.
    enum class WorkflowTrigger { Push, Release, PullRequest, PullRequestManual };
    // Who proposed the code a pull-request run is about to execute. Carried into
    // the ActionRun so the approval gate survives a restart.
    struct ActionPullContext {
        int number = 0;
        QString author;         // display name, or the signer key when unnamed
        bool firstTimeAuthor = false;
        // Branch the pull request targets. Whether a workflow runs on pull
        // requests is read from there as well as from the proposed head, so a
        // pull request opened before the repository adopted `on: pull_request`
        // is still checked — and a contributor cannot switch their own checks
        // off by editing the trigger on their branch.
        QString base;
    };
    // Two overloads rather than a defaulted `pull`: a nested type's default
    // member initializers cannot be used in a default argument of the class
    // that contains it.
    void queueWorkflowsForCommit(int repoIndex, const QString &owner,
                                 const QString &name, const QString &commit,
                                 const QString &ref,
                                 WorkflowTrigger trigger,
                                 ActionPullContext pull,
                                 const QString &before = QString());
    void queueWorkflowsForCommit(int repoIndex, const QString &owner,
                                 const QString &name, const QString &commit,
                                 const QString &ref,
                                 WorkflowTrigger trigger = WorkflowTrigger::Push);
    // Queue `on: pull_request` workflows for every open pull request of `repo`
    // whose head has not been checked yet, publishing each head into the served
    // mirror so the runner can check it out. Runs the git probes off the GUI
    // thread; safe to call on every pull reload.
    void queueChecksForOpenPulls(RepositoryRecord repo, QList<PullRequest> pulls);
    // One pull request, reduced to what the off-thread head publication needs,
    // plus the trust verdict (decided on the GUI thread from the pull list).
    struct PullCheckCandidate {
        int number = 0;
        // The pull itself, for the case where no ref carries its head yet: a
        // submission that arrived as a signed patch has no commit to check out
        // until PullStore replays it onto the base.
        PullRequest pull;
        QString head;   // signed head branch, "fork:branch" stripped
        QString base;   // branch the pull request targets
        QString author; // display name for the approval prompt
        bool firstTimeAuthor = false;
        // Only a branch-backed pull declares its named head authoritative. A
        // stored-patch pull keeps pr.head as provenance even after an edit
        // changed its content, so its head must come from refs/pr/<n>/head.
        bool branchBacked = false;
        // Replaying this pull onto the base has not already been tried and
        // failed. A submission that does not apply must not spend every pass's
        // `git am` budget and starve the pulls that would resolve.
        bool mayMaterialize = true;
    };
    struct PullCheckHead {
        int number = 0;
        QString commit;  // head as published into the served mirror
        QString ref;     // refs/heads/pr/<n>, the ref the runner sees
        QString base;    // branch the pull request targets
        QString author;
        bool firstTimeAuthor = false;
        bool published = false; // this pass pushed the head into the mirror
        QString error;   // why this head could not be published, if it could not
    };
    PullCheckCandidate pullCheckCandidate(const PullRequest &pr,
                                          const QList<PullRequest> &history,
                                          const RepositoryRecord &repo) const;
    // `force` is the maintainer pressing "Run checks against this PR": it skips
    // the already-checked dedupe and the per-pass cap, but never the approval
    // gate a first-time contributor's pull request carries.
    void resolvePullCheckHeads(RepositoryRecord repo,
                               QList<PullCheckCandidate> candidates,
                               WorkflowTrigger trigger, bool force);
    void applyPullCheckHeads(const QString &owner, const QString &name,
                             const QString &mirror, WorkflowTrigger trigger,
                             bool force, const QList<PullCheckHead> &heads);
    // Throttled per-repository catch-up, so a pull request that arrived while
    // this node was closed still gets its checks without the repo being opened.
    void sweepPullChecksFor(RepositoryRecord repo);
    QHash<QString, qint64> m_pullChecksSweepAtMs; // "owner/name" -> last sweep
    bool resolveActionApproval(const RepositoryRecord &repo,
                               const ActionRun &run);
    bool approveActionRun(int runId, QString *error = nullptr);
    int autoApproveAwaitingRuns(const RepositoryRecord &repo);
    struct WorkflowScanEntry {
        QString path;    // .forkmesh/<file>
        QString content; // the file as it is at the scanned commit
        // The base branch's copy of this same workflow declares `pull_request`.
        bool baseTriggersPullRequest = false;
        bool defaultBranchRetiresPush = false;
    };
    struct WorkflowScan {
        bool mirrorMissing = false;
        bool metadataOnly = false;
        bool snapshotOk = false;
        QString repositoryTree;
        QString executionDigest;
        QString snapshotError;
        QStringList changedPaths;  // push: changed paths for optional on.push.paths filter
        QString pushBefore;
        QList<WorkflowScanEntry> workflows;
    };
    // A Git worker can finish finding workflows while the Actions-history
    // worker owns ActionStore's cache mutex. Keep its parsed result until that
    // first cache scan has published, rather than letting createRun() stall the
    // GUI while it waits for the scan.
    struct DeferredActionWorkflowScan {
        QString owner;
        QString name;
        QString commit;
        QString ref;
        WorkflowTrigger trigger = WorkflowTrigger::Push;
        ActionPullContext pull;
        WorkflowScan scan;
    };
    void applyWorkflowScan(const QString &owner, const QString &name,
                           const QString &commit, const QString &ref,
                           WorkflowTrigger trigger,
                           const ActionPullContext &pull,
                           const WorkflowScan &scan);
    void drainDeferredActionWorkflowScans();
    void submitPullComment();                       // post a comment on the PR
    void sendPullRevisionToAgent();                 // re-queue the linked agent with revision feedback
    void submitPullReview(const QString &state);    // approve / request changes
    bool currentAccountOwnsPullRepository();
    void promptNewPull();
    void importPatchAsPull(); // read a .patch/.diff file and open it as a PR
    void promptNewPullFromDirectory();
    struct NewPullSourceRead {
        QString origin;
        QStringList branches;
        bool haveCurrentBranch = false;
        QString currentBranch;
        bool haveSubject = false;
        QString subject;
    };
    struct NewPullDiffRead {
        bool ok = false;
        QByteArray diff;
        QString error;
        QByteArray mbox;
    };
    void showNewPullDialog(const QString &sourceDir, const QString &preferredBase,
                           const QString &preferredHead, const QString &dir,
                           const NewPullSourceRead &read);
    void finishNewPull(PullRequest pr, bool fromRange, const QString &targetOwner,
                       const NewPullDiffRead &read);
    bool m_newPullSourceReading = false;
    void promptNewPullFromSource(const QString &sourceDir,
                                 const QString &preferredBase = QString(),
                                 const QString &preferredHead = QString());
    void switchToPullTab(int pullNumber);
    void openPullFromNotification(int pullNumber);
    void openPullDiffInGitView(int pullNumber);
    QHash<QString, QString> buildPullLineNotes(const PullRequest &pr);
    void updateCurrentPullBranch();
    void mergeCurrentPull();
    void afterPullMerged(PullRequest pr, bool ownerReviewOverride = false);
    void resolveCurrentPullConflicts(); // open the per-conflict merge editor
    bool runMergeConflictEditor(const QString &title, const QString &introHtml,
                                const QString &workTree,
                                const QStringList &conflictedFiles,
                                const QString &commitButtonText,
                                const std::function<bool(QString *)> &commitFn,
                                bool commitOffThread = false);
    void fixCurrentPullConflictsWithAi(const QString &provider);
    // "Fix" on a conflicted pull request: hand the work to an agent that runs
    // in a checkout of the pull request's own branch. The model beside the
    // button says who does it; pullFixAgentPrompt() is the task they get.
    void fixCurrentPullWithAgent();
    QString pullFixAgentPrompt(const PullRequest &pull,
                               const QString &checkoutBranch = QString()) const;
    // The local branch of the repository on screen that carries `pull`'s code:
    // its signed head when that branch is here, otherwise the durable `pr/<n>`
    // branch a relayed contribution is materialized under. Empty when neither
    // is, which is the caller's cue to materialize the pull's head ref first.
    QString pullAgentBranch(const PullRequest &pull) const;
    void refreshPullFixModelCombo();
    QString pullFixProvider() const; // agent provider chosen in that combo
    QString pullFixModel() const;    // model id chosen in that combo
    void fixCurrentPullConflictsWithOriginatingAgent();
    void editCurrentPullFile();         // edit the selected file on the PR's branch
    void deleteCurrentPullFile();       // delete the selected file on the PR's branch
    void closeIssuesLinkedFromPull(const PullRequest &pr);
    QList<int> closeIssuesForMerge(const QList<int> &numbers,
                                   const QString &comment, const QString &via);
    QList<int> closeIssuesForMerge(IssueStore store, const QList<int> &numbers,
                                   const QString &comment, const QString &via);
    void fundBountiesForMergedPull(const PullRequest &pr);
    void autoBountyForMergedPull(const PullRequest &pr);
    void pollBountyPayout(const RepositoryRecord &repo, int number, double amount,
                          const QString &kind = QString());
    QList<int> issuesLinkedFromPull(const PullRequest &pr) const;
    QList<int> pullsLinkedToIssue(int issueNumber) const;
    void linkPullToIssueFromIssuePage(); // issue page: pick a PR to link
    void linkIssueToPullFromPullPage();  // PR page: pick an issue to link
    void postIssueLinkComment(int issueNumber, const QString &body);
    void postPullLinkComment(int pullNumber, const QString &body);
    void linkAgentPullToIssue(const AgentSession &session, int prNumber);
    void closeCurrentPull();
    bool currentPullAuthoredHere() const;
    void requestCurrentPullLifecycle(const QString &type);
    void reopenCurrentPull();
    void restoreCurrentArchivedPull();
    void sendCurrentPullToSource();
    void deleteCurrentPull();
    void deleteCurrentPullAndBranch();
    void mergeAndDeleteCurrentPull();
    void deleteAllClosedAndMergedPullsAndBranches();
    void deletePullAndBranchAsync(int number, const QString &head, bool haveBranch,
                                  bool rewriteHistory, bool propagate,
                                  std::function<void()> onDone = {});
    void setPullDeleteButtonsEnabled(bool enabled);
    bool confirmPullDeletion(const QString &prompt, bool *rewriteHistory);
    void syncPullsInbox();
    void refreshPullsFromMesh();
    void submitPullToInbox(const PullRequest &pr);
    void submitPullToInbox(const PullRequest &pr, const RepositoryRecord &targetRepo,
                           bool quiet = false);
    void restorePendingPullSubmissions();
    void savePendingPullSubmissions() const;
    void schedulePendingPullSubmissionRetry(int delayMs = -1);
    void dispatchPendingPullSubmissions();
    void deliverPendingPullSubmission(const QString &submissionKey);
    // Submit a signed PR conversation event (comment/review) to the relay inbox
    // for repos this node can't write directly.
    void submitPullEventToInbox(int number, const PullEvent &ev);
    void submitPullEventToInbox(int number, const PullEvent &ev, int repoIndex);
    void updatePullActionState();
    void updatePullConflictCard(const PullRequest *selectedPull, bool conflicted,
                                const QStringList &conflictFiles);
    void updatePullMergeBlockers(bool have, bool open, bool writable, bool mergeable,
                                 bool conflictPending, bool conflicted,
                                 bool reviewBlocks, bool independentChangesRequested,
                                 const QStringList &changesRequestedBy,
                                 const QStringList &conflictFiles,
                                 const QString &base, bool behind, int behindCount);
    QUrl pullsApiUrl(const RepositoryRecord &repo) const;

    QWidget *buildMergeQueuePanel();   // the editable queue in the Pulls pane
    void refreshMergeQueuePanel();     // repaint it from the open repo's queue
    bool mergeQueueEnabledForOpenRepo() const;
    void setRepoMergeQueueEnabled(bool on);   // repository Settings toggle
    void setMergeQueuePaused(bool paused);
    void addPullToMergeQueue(int number);
    void removePullFromMergeQueue(int number);
    void toggleCurrentPullInMergeQueue(); // the PR header's "Queue" tile
    void moveMergeQueueSelection(int delta);
    void removeMergeQueueSelection();
    void clearMergeQueue();
    void queueBranchForMerge(const QString &branch);
    void scheduleMergeQueueRun(int delayMs);
    void processMergeQueue();
    MergeQueue openRepoMergeQueue() const;
    MergeQueue mergeQueueFor(const QString &owner, const QString &name) const;
    void saveMergeQueue(const QString &owner, const QString &name,
                        const MergeQueue &queue);
    void setMergeQueueEntryState(const QString &owner, const QString &name,
                                 int number, const QString &state,
                                 const QString &detail);
    void dropMergeQueueEntry(const QString &owner, const QString &name,
                             int number);
    enum class AgentListSort {
        Priority,
        NewestAdded,
        OldestAdded,
        RecentActivity,
        Status,
        Title,
        Provider,
        IssueNumber,
        TokenUsage,
        Cost,
        Runtime,
        FilesChanged,
        Conflicts,
    };
    // The Teammates control next to Sort cycles through three states, so the
    // filter is a mode rather than a checkbox.
    enum class AgentTeammateFilter {
        All,
        TeammatesOnly,
        WithoutTeammates,
    };
    using AgentListView = forkmesh::ui::ListViewMode;
    QWidget *buildAgentsTab();
    QWidget *buildAgentListToolBar();
    void openAgentProviderTerminal(const QString &program,
                                   const QString &providerName,
                                   const QString &dialogObjectName,
                                   const QString &terminalObjectName);
    void buildAgentDetailActionButtons();
    void buildAgentOutputViews(QWidget *detailPane);
    void buildAgentFilesPanel();
    void initAgents();
    void reloadAgents(AgentReloadScan scan = AgentReloadScan::Rescan);
    // Repaints the Agents list after deleteStoredAgentSession() dropped rows,
    // without reloadAgents()' re-read and rescan of every other session.
    void refreshAgentListAfterRemoval();
    void refreshAgentTable();
    void setAgentListView(AgentListView view);
    void applyAgentRowChips(int row, const AgentSession &session,
                            const AgentDiffStat &stat);
    void startAgentDiffStatsRefresh(const QList<AgentSession> &sessions,
                                    const QString &owner, const QString &name,
                                    const QString &agentGitDir);
    void sortAgentTableRows(QList<const AgentSession *> &visible,
                            const QString &query);
    bool agentTableRowsMatch(const QStringList &identities) const;
    AgentFolders m_agentFolders;
    QString agentFolderRepo() const;
    void applyAgentFolderRowCells(int row, const AgentFolder &folder,
                                  const QList<const AgentSession *> &members,
                                  bool filtering);
    void saveAgentFolders();
    QString createAgentFolder(const QString &name, const QList<int> &sessionIds);
    void promptNewAgentFolder(const QList<int> &sessionIds);
    void promptRenameAgentFolder(const QString &folderId);
    void deleteAgentFolder(const QString &folderId);
    void toggleAgentFolder(const QString &folderId);
    void setAllAgentFoldersCollapsed(bool collapsed);
    void moveAgentFolderToEdge(const QString &folderId, bool last);
    void moveAgentFolderStep(const QString &folderId, bool down);
    bool moveAgentSessionsToFolder(const QList<int> &sessionIds,
                                   const QString &folderId);
    void selectAgentFolderSessions(const QString &folderId);
    void dropAgentSessions(const QList<int> &sessionIds, int targetSessionId,
                           const QString &folderId, bool after);
    void dropAgentFolder(const QString &folderId, const QString &targetFolderId,
                         bool after);
    QMenu *buildAgentFolderContextMenu(const QString &folderId, QWidget *parent);
    QMenu *buildAgentListBlankContextMenu(QWidget *parent);
    void addAgentFolderMenu(QMenu *menu, int sessionId);
    void setAgentListSort(AgentListSort sort);
    void setAgentTeammateFilter(AgentTeammateFilter filter);
    QList<int> selectedAgentSessionIds() const;
    void deleteSelectedAgentSessions();
    void refreshAgentTaskLinkUi(int sessionId);
    void reprioritizeAgentSessions(int sourceSessionId, int targetSessionId,
                                   bool placeAfterTarget);
    void moveAgentSessionToPriorityEdge(int sessionId, bool bottom);
    void applyAgentRowCells(int row, const AgentSession &session,
                            const QString &agentGitDir, const QString &agentBase);
    QPixmap agentSessionPullAvatar(const AgentSession &session);
    // Same portrait, unpacked to a file the OS notifier can read.
    QString agentNotificationIconPath(int sessionId);
    void scheduleAgentLiveDiffRefresh(int sessionId);
    void armAgentDiffPassTimer(int delayMs);
    qint64 agentDiffCooldownRemainingMs() const;
    AgentDiffStat agentDiffStat(const AgentSession &session, const QString &gitDir,
                                const QString &base);
    void updateAgentTokenCell(int sessionId);  // live Tokens-column update
    void updateAgentCostCell(int sessionId);   // in-place Cost-column update
    void updateAgentRunSummaryCells(int sessionId); // in-place Turns/Time update
    void updateAgentStatusCell(int sessionId); // in-place Status-column update
    void applyAgentDiffStatResult(int generation, int repoIndex, int sessionId,
                                  const AgentDiffStat &stat,
                                  const QString &signature);
    void refreshAgentStatusPill(int sessionId); // in-place detail-header pill update
    void showAgentMetaPopup(); // open the detail metadata from hover or click
    void toggleAgentMetaPopup(); // click handler for the detail metadata popup
    void hideAgentMetaPopupIfPointerAway(); // preserve the popup while entering it
    void animateRunningAgentIcons();           // spins running rows' Status glyph
    void advanceAgentSpinPhases();   // one frame of every running fireball's orbit
    qreal agentSpinAngle(const AgentSession &session) const; // current head angle
    double agentActivityMeter(const AgentSession &session) const; // 0..1 live rate
    void noteAgentActivity(int sessionId, int bytes = 0);
    void onScannerTick();
    void refreshAgentTranscriptTheme(bool dark);
    void showAgentSession(int sessionId);
    void syncQuickAddControlsToAgentSession(const AgentSession &session);
    void refreshAgentDetailMeta(int sessionId,
                                bool forceProcessRefresh = false);
    void refreshAgentTaskGradeControl(const AgentSession *session);
    void scheduleAgentProcessMetaRefresh(qint64 processId, bool forceRefresh);
    qint64 agentSessionProcessId(int sessionId) const;
    void completeAgentSessionWhenSubprocessesExit(int sessionId);
    // The GUI-thread half of the poll above, once the process scan is back.
    void applyAgentSubprocessCompletionScan(
        int sessionId,
        const QList<SystemStats::DescendantProcess> &subprocesses);
    // Mirror m_agentBackgroundWork onto the live session rows so the list, the
    // header pill and the transcript footer all read the same state. Called
    // again after reloadAgents(), which rebuilds m_agentSessions from disk and
    // would otherwise drop the (deliberately unpersisted) marker.
    void applyAgentBackgroundWork();
    // One plain-text line naming what is still running, or empty when the
    // session has nothing outstanding.
    QString agentBackgroundWorkNote(int sessionId) const;
    void setAgentLogText(int sessionId, const QString &text);
    qint64 sessionTokenTotal(const AgentSession &session) const;
    void seedSessionTokens();
    void setAgentUsageLabel(const AgentSession &session);
    struct AgentLogScan {
        QString log;
        int requests = 0, responses = 0, errors = 0;
        qint64 inTokens = 0, outTokens = 0;
    };
    static AgentLogScan scanAgentLog(QString log);
    bool m_agentNetScanInFlight = false; // one live [net] rescan at a time
    void applyAgentNetworkPanel(const AgentLogScan &scan, const QString &status);
    AgentSession *findAgentSession(int sessionId);
    const AgentSession *latestAgentSessionForIssue(int issueNumber) const;
    const AgentSession *agentSessionForPull(int prNumber,
                                            const QString &headBranch = QString()) const;
    bool bindAgentSessionsToPull(int prNumber, const QString &headBranch);
    bool markAgentSessionsMerged(int prNumber, const QString &branch,
                                 bool mergeVerified);
    bool markRepoAgentSessionsMerged(const QString &owner, const QString &name,
                                     int prNumber, const QString &branch,
                                     bool mergeVerified);
    void refreshAgentMergeState();
    void updateAgentMergeCandidates(const QHash<int, QString> &heads);
    void markAgentSessionsLanded(const QList<int> &sessionIds, bool refreshUi,
                                 bool attested = false);
    void withdrawAgentSessionMerges(const QList<int> &sessionIds);
    void assignIssueToAgent(const QString &provider, const QString &model = QString());
    int startAgentForIssue(const Issue &issue, const QString &provider, bool createPr,
                           bool quiet = false, const QString &model = QString(),
                           const RepositoryRecord *repoHint = nullptr);
    void toggleIssueLooper();
    void looperStartNext();
    void looperOnSessionFinished(int sessionId);
    // stamp this node onto an issue's assignees the moment the looper
    // takes it, so the claim syncs to every node and no second looper (here or on
    // another mirror) starts the same task. The host appends the node and commits
    // (which syncs to mirrors); a mirror with no write access files the signed
    // assignees event to the owner's inbox, which merges and syncs it back.
    void looperClaimIssue(int number, const QStringList &existingAssignees);
    QString nodeAssigneeTag() const;
    void updateIssueLooperButton();
    void persistLooperState();
    void maybeRestoreIssueLooper();
    void continueSelectedAgentSession();
    void continueAgentSessionInteractive(int sessionId);
    void continueAgentSession(int sessionId, bool deferRefresh = false);
    void answerAgentQuestion(int sessionId, const QString &toolUseId,
                             const QString &answer, bool sensitive,
                             bool deferRefresh = false);
    void fixAgentConflictsWithAgent(int sessionId);
    void pullBaseIntoAgentBranch(int sessionId);
    void refreshAgentSyncControls(int sessionId);
    bool agentSessionHasLiveTransport(int sessionId) const;
    bool handOffAgentSessionToProvider(int sessionId, const QString &provider);
    void applyComposerSelectionToAgentSession(int sessionId);
    void sendPromptToSelectedAgent(const QString &prompt);

    void sendPromptToAgentSession(int sessionId, const QString &prompt);
    bool deliverPromptToLiveAgentTransport(int sessionId, const QString &prompt,
                                           bool recordPrompt = true);
    // Puts a running Claude Code transport into the session's stored permission
    // mode. False means the switch needs a restart to take effect.
    bool syncLivePermissionMode(int sessionId, ClaudeStreamSession *claude);
    void queueAgentSteerMessage(int sessionId, const QString &prompt);
    QString takeAgentSteerMessageForLaunch(int sessionId);
    void restoreAgentSteerMessageAfterFailedLaunch(int sessionId);
    void acknowledgeAgentSteerMessage(int sessionId);
    bool discardWedgedAgentTransport(int sessionId);
    void noteAgentSessionNotice(int sessionId, const QString &text,
                                bool error = false);
    void failQueuedAgentSession(AgentSession &session, const QString &reason);
    QString issueContextPrompt(const Issue &issue) const;
    void sendIssueContextToSelectedAgent();
    void deleteSelectedAgentSession();
    void deleteAgentSessionEntry(int sessionId);
    // Full teardown of one session — PR, worktree, branch — behind the rail's
    // Delete button and the trash at the end of each agent row.
    void deleteAgentSessionAndCleanup(
        int sessionId,
        std::shared_ptr<forkmesh::localcontrol::agentops::DeleteTrace> trace = {});
    void deleteAgentSessionOrgTask(
        int sessionId, const QString &taskId, bool force,
        std::shared_ptr<forkmesh::localcontrol::agentops::DeleteTrace> trace = {});
    // Continues an explicit agent delete after its linked organization task,
    // when present, has been removed from the board.
    void deleteAgentSessionAndCleanupAfterTask(
        int sessionId,
        std::shared_ptr<forkmesh::localcontrol::agentops::DeleteTrace> trace = {});
    void deleteAgentSessionFromRow(int sessionId); // confirms, then the above
    void createLinkedIssueForSelectedSession();
    bool deleteStoredAgentSession(int sessionId, bool cleanupWorktree = true,
                                  bool allowAssociationOnly = false);
    // Sweeps recycle-bin entries past the retention window (issue #578). Safe
    // to call at any time: it only removes bins that have already expired.
    void purgeExpiredAgentRecycleBin();
    // The strip over the Agents list naming the open repository's deleted
    // agents, with Show (the bin dialog), Restore all and Delete all. Hidden
    // while that repository's bin is empty.
    QWidget *buildAgentRecycleBinBar();
    void reloadAgentRecycleBin(); // re-read the bin from disk, then update
    void updateAgentRecycleBinBar();
    QList<AgentSession> agentRecycledInOpenRepo() const;
    void restoreAllRecycledAgentsInOpenRepo();
    void deleteAllRecycledAgentsInOpenRepo();
    void deleteWorktreeBranchAndAgentInBackground(
        const QString &worktreePath, const QString &branch,
        std::function<void(bool)> onDone = {});
    void deleteWorktreeBranchAndAgent(const QString &worktreePath,
                                      const QString &branch, bool confirm = true,
                                      bool deferRefresh = false,
                                      std::function<void()> onDone = {});
    void deleteMergedAgentBranchesInTurn(const QString &repoPath,
                                         QStringList branches, int sessionCount);
    void deleteAllMergedAgentSessions();
    void testOpenAiAgentKey();
    void refreshClaudeSpend();
    void refreshClaudeCodeUsage(bool fromHover = false);
    void applyClaudeUsageResponse(const QJsonObject &response);
    void applyClaudeUsageCredits(const QJsonValue &extraUsage);
    void showAgentAccountMenu(const QString &provider,
                              const QPoint &globalPosition);
    void probeClaudeAgentAccountIdentity(const QString &configDir,
                                         QAction *accountAction,
                                         const QString &baseLabel);
    void selectAgentAccount(const QString &provider, const QString &accountId);
    void addAgentAccount(const QString &provider);
    QString createAgentAccount(const QString &provider, const QString &label,
                               const QString &configDir = QString());
    void startClaudeAccountLogin(const QString &requestedId,
                                 const QString &label);
    bool finishClaudeAccountLogin(const QString &stagingDir);
    void watchClaudeAccountLogin(const QString &stagingDir);
    void reconcileClaudeAccountLogins();
    struct PendingClaudeLogin {
        QString requestedId;
        QString label;
    };
    QHash<QString, PendingClaudeLogin> m_pendingClaudeLogins;
    QFileSystemWatcher *m_claudeLoginWatcher = nullptr;
    bool m_reconcilingClaudeAccounts = false;
    void editAgentAccount(const QString &provider, const QString &accountId);
    bool renameAgentAccount(const QString &provider, const QString &accountId,
                            const QString &label);
    bool deleteAgentAccount(const QString &provider, const QString &accountId);
    void confirmDeleteAgentAccount(const QString &provider,
                                   const QString &accountId);
    QAction *addAgentUsagePageAction(QMenu *menu);
    void refreshAgentAccountUsageMenu(const QString &provider);
    // The open account menu per provider, so a usage response does not have to
    // findChild() its way through the whole window to find one that is usually
    // not there. Both are WA_DeleteOnClose, so these clear themselves.
    QPointer<QMenu> m_codexAccountUsageMenu;
    QPointer<QMenu> m_claudeAccountUsageMenu;
    void launchAgentSystemTerminal(const QString &provider,
                                   const QString &mode = QStringLiteral("agent"),
                                   const QString &accountId = QString());
    bool launchSystemTerminal(const QString &program, const QStringList &commandArgs,
                              const QString &cwd,
                              const QMap<QString, QString> &extraEnv = {});
    QStringList agentAccountUsageLines(const QString &provider,
                                       const QString &accountId) const;
    void flashUsageChart(QWidget *chart, bool ok);
    void refreshClaudeModelCombo();
    void applyLiveClaudeModelsToCombos();
    void refreshCodexUsageRemaining();
    void applyCodexRateLimits(
        const QJsonObject &rateLimits,
        const QJsonObject &rateLimitsByLimitId = QJsonObject());
    // A live run's own rate_limit_info: the percentage for one window and, when
    // the CLI says so, the time it reopens.
    void applyClaudeRateLimitEvent(const QJsonObject &rateLimitInfo);
    void applyClaudeUsage(bool weekly, int percent);
    int m_claudeUsageLastWeekPct = -1;
    int m_claudeUsageLast5hPct = -1;
    void applyClaudeReset(bool weekly, qint64 resetMs);
    void applyClaudeFableUsage(int percent);
    void applyClaudeFableReset(qint64 resetMs);
    void maybeEmailCreditsRefilled(bool weekly);
    void scheduleUsageLimitReminder(const QString &providerKey,
                                    const QString &windowKey,
                                    const QString &providerName,
                                    const QString &windowName,
                                    qint64 resetMs);
    void restoreUsageLimitReminders();
    void clearUsageLimitReminders();
    void notifyUsageLimitReady(const QString &providerKey,
                               const QString &windowKey,
                               const QString &providerName,
                               const QString &windowName,
                               qint64 resetMs);
    void holdAgentForUsageLimit(int sessionId);
    void armUsageLimitAgentHold(int sessionId, qint64 resumeAtMs);
    void restoreUsageLimitAgentHolds();
    void continueUsageLimitHeldAgents(const QList<int> &sessionIds);
    void continueUsageLimitHeldAgentsFor(const QString &providerKey);
    void cacheSpendLabel(const QString &textKey, const QString &tsKey,
                         const QString &text);
    void applyCachedSpendLabels();
    void markAgentLimitWindow(const QString &provider);
    void refreshAgentLimitLabel();
    void openAgentSessionFromIssue();
    void switchToAgentsTab(int sessionId);
    void switchToAgentBranch(int sessionId);
    void openAgentBranchInGitView(int repoIndex, const QString &branch,
                                  const QString &recordedBase, int sessionId);
    void openAgentsOverview();
    void openRepoAgentsList();
    void updateAgentsNavBadge();
    void refreshAgentDotMatrix();
    void refreshActionRunStrip();
    QString actionWorkflowIconName(const ActionRun &run);
    void processAgentQueue();
    void scheduleAgentQueuePump();
    void setAgentConcurrencyLimit(int limit);
    QList<int> agentQueueOrder();
    bool moveQueuedAgentSession(int sessionId, int to);
    QJsonObject agentQueuePayload();
    void refreshAgentQueueControls();
    void showAgentQueueLimitMenu();          // 1..N concurrent-agent picker
    void refreshAgentToolbarAccountButtons();
    void showAgentUsageMenu(const QPoint &globalPosition);
    int runningAgentCount() const;
    QList<int> runningAgentSessionIds() const; // oldest start first
    int requeueAgentsOverConcurrencyLimit();
    bool parkRunningAgentSession(int sessionId);
    QList<int> stoppableAgentSessionIds() const;
    void stopAllRunningAgents();
    bool stopAgentSessionById(int sessionId);
    int stopRepositoryAgentsForGuardrailChange(const QString &owner,
                                                const QString &name);
    // Save the open repository's agent allowlist, stopping its running agents
    // when the change narrows an enabled policy, and refresh Settings.
    void setRepoAgentAllowedDirectories(const QStringList &directories);
    bool isStoppableAgentSession(int sessionId) const;
    bool agentSessionWorkInFlight(int sessionId) const;
    qint64 agentSessionLastLiveMs(int sessionId) const;
    static constexpr qint64 kAgentSilentStaleMs = 90'000;
    QList<int> startableAgentSessionIds() const;
    void startAllStoppedAgents();
    // The hover control on an agent row: AgentRunToggleNone/Start/Stop.
    int agentRunToggleMode(const AgentSession &session) const;
    void toggleAgentSessionRun(int sessionId);
    QList<int> updatableAgentSessionIds() const;
    void updateAllAgentWorktreesFromMain();
    struct AgentWorktreeUpdateJob {
        QString repoPath;
        QString branch;
        QString base;
    };
    struct AgentWorktreeUpdateSummary {
        int updated = 0;
        int current = 0;
        int busy = 0;
        int conflicted = 0;
        int skipped = 0;
        QStringList logs;
    };
    static AgentWorktreeUpdateSummary updateAgentWorktreesFromBase(
        const QList<AgentWorktreeUpdateJob> &jobs, int unresolved);
    void applyAgentWorktreesUpdate(int total,
                                   const AgentWorktreeUpdateSummary &summary);
    bool m_updateAllAgentWorktreesRunning = false;
    AgentRunner *runnerForSession(int sessionId) const;
    AgentRunner *acquireAgentRunner();
    bool anyAgentRunning() const;
    int activeAgentSessionId() const;
    QStringList runningAgentBlockers() const;
    void onAgentLog(int sessionId, const QString &text);
    void appendAgentRawLog(const QString &text);
    void showAgentRawOutput();
    void showAgentRawLogEvent(const QJsonObject &event);
    void onAgentStatusChanged(int sessionId, const QString &status);
    void onAgentFinished(int sessionId, bool ok);
    void onAgentNeedsAttention(int sessionId, const QString &message);
    void updateAgentActionState();
    void updateQuickAddEnterTarget();
    bool quickAddShouldFollowUpAgent() const;
    int quickAddFollowUpAgentSessionId() const;
    // The real agent behind the PR open in its detail pane, plus its head
    // branch when requested. Provenance-only association records cannot take
    // a follow-up prompt, so they intentionally do not count here.
    const AgentSession *quickAddPullDetailAgent(QString *branch = nullptr) const;
    void updateQuickAddTargetAgentLabel();
    void updateIssueAgentUi(const Issue &issue);
    void refreshIssueFilesPanel(const Issue &issue);
    void renderIssueDiff(int issueNumber, const QByteArray &patch,
                         const QString &dir, const QString &base);
    void populateIssueFilesCell(int row, const Issue &issue);
    // Absolute path of the first attached image, for the list row preview.
    QString issueAttachmentPreviewPath(const Issue &issue,
                                       const QStringList &attachments) const;
    QIcon issueAssigneeAvatar(const QString &name);
    bool isLocalAuthorName(const QString &name) const;
    // Face for a timeline entry: follows the printed name, not the key
    // that signed the event, so proxied reports keep the reporter's face.
    QPixmap issueTimelineAvatar(const QString &authorId,
                                const QString &authorName, int side);
    // Re-resolve every face the issue list and the open issue already show.
    // The user directory carrying the real pictures lands after those are
    // painted, and both surfaces cache what they drew (issue #699).
    void refreshIssueAvatars();
    QHash<QString, QIcon> m_assigneeAvatarCache; // assignee name -> avatar icon
    AgentRunner::Config agentConfigForProvider(
        const QString &provider, const QString &model = QString(),
        const RepositoryRecord *repository = nullptr);
    void startClaudeCodeTerminal(AgentSession &session, const Issue &issue,
                                 const QString &repoPath);
    QString agentProviderName(const QString &provider) const;
    QStringList agentDoneFacts(int sessionId, bool qualifyRepo);
    QString agentDoneNotificationBody(int sessionId, const QString &summary);
    QWidget *buildRepoActionsTab();
    QWidget *buildActionsWorkflowPane();
    QWidget *buildActionsRunsPane();
    void refreshRepoActions();           // workflows column + runs for the open repo
    void refreshWorkflowProgress();
    QList<ActionWorkflow> availableWorkflowsForRepo(
        const RepositoryRecord &repo) const;
    void reloadWorkflowCountInBackground();
    void updateManualRunBar();
    void runSelectedWorkflowManually();
    void startManualWorkflowRun(int repoIndex, const QString &path, const QString &branch,
                                std::function<void(int runId, const QString &error)> done);
    void rerunSelectedRun();
    void stopSelectedRun();
    void skipSelectedRun();
    // The same actions for any open run, so a parallel column drives its own
    // run rather than whichever one the primary pane happens to show.
    int rerunActionRun(int runId);
    void stopActionRun(int runId);
    void skipActionRun(int runId);
    void copyActionRunLog(int runId);
    void copyActionRunLogPath(int runId);
    void sendActionRunToAgent(int runId, const QString &provider,
                              const QString &model);
    // Hands the selected run to an agent: where its log lives on disk (the whole
    // file, not a tail that stopped before the failing line), and the ask to fix
    // both this run and the recent CI test failures alongside it.
    void sendSelectedRunToAgent(const QString &provider, const QString &model);
    QString actionRunAgentPrompt(const ActionRun &run) const;
    // The run log's path on this machine, in native separators, for the places
    // that show it or hand it to an agent. Empty before the store exists.
    QString actionRunLogLocation(const ActionRun &run) const;
    void maybeAutoFixFailedRun(const ActionRun &run);
    void clearActionRuns();
    // Deletes finished runs (and their logs) and refreshes every run view.
    void removeActionRuns(const QList<ActionRun> &doomed);
    // Cancelled runs are normally transient noise (for example, superseded
    // push runs). When the user keeps the default cleanup preference, remove
    // their persisted metadata and logs together, then refresh every surface
    // that could still be showing one.
    bool deleteCancelledRunsAutomatically(const QList<ActionRun> &runs);
    void initActions();                  // store/runner/watcher, load history, hooks
    // initActions() splits setup from history loading: the store and runners
    // stay in the constructor so m_actionStore is never null, while the disk
    // scan/migration runs on a worker after the first event-loop turn.
    void initActionStore();
    void restoreActionRuns();
    bool m_actionRunsRestored = false;
    bool m_actionRunsRestoreInFlight = false;
#ifdef FORKMESH_WINDOW_TESTS
    std::function<void()> m_testActionRunsRestoreWorkerHook;
#endif
    void ensurePushHook(const RepositoryRecord &repo) const;
    // Working-copy post-commit/post-merge hooks: spool a *.commit event the
    // instant a commit lands on the source of truth (terminal/IDE/agent), so
    // scanActionSpool can sync the mirror and signal peers over the websocket
    // immediately instead of at the next heartbeat.
    void ensureCommitSignalHook(const RepositoryRecord &repo) const;
    void removePushHook(const RepositoryRecord &repo) const;
    void installAllPushHooks() const;
    void scanActionSpool();              // read *.push/*.commit events, enqueue runs
    bool m_actionSpoolSweepPending = false;
    void scanServedMirrorHeads(const QHash<int, QString> &pushedCommits);
    bool m_servedMirrorHeadScanInFlight = false;
    bool m_servedMirrorHeadScanPending = false;
    quint64 m_servedMirrorHeadScanGeneration = 0;
    QHash<int, QSet<QString>> m_servedMirrorHeadPushedCommits;
    void finishServedMirrorHeadScan(bool queued);
    void syncMirrorActionsConfiguration();
    void scanExternalActionsSources();
    void updateMirrorActionsRuntimeState();
    // Atomically publish a bounded, redacted Actions run summary for the
    // gateway. Live log lines are coalesced; lifecycle changes publish on the
    // next event-loop turn and the lease is refreshed periodically.
    void scheduleMirrorActionsSummary(int delayMs = 0);
    void writeMirrorActionsSummary();
    void enqueuePushEvent(const QString &owner, const QString &name,
                          const QString &commit, const QString &ref,
                          const QString &before = QString());
    QStringList actionNodeLabels() const;
    QString workflowNodePin(const RepositoryRecord &repo,
                            const QString &path) const;
    QStringList workflowRunsOnLabels(const RepositoryRecord &repo,
                                     const ActionWorkflow &workflow) const;
    bool workflowRunsOnThisNode(const RepositoryRecord &repo,
                                const ActionWorkflow &workflow) const;
    QString workflowDedicationLabel(const RepositoryRecord &repo,
                                    const ActionWorkflow &workflow) const;
    QStringList actionNodeCandidates() const;
    void setWorkflowNode(const QString &node);
    void refreshWorkflowNodeCombo();
    void updateWorkflowListItem(QListWidgetItem *item,
                                const ActionWorkflow &workflow,
                                const RepositoryRecord &repo);
    struct ActionQueueVerification;
    void processActionQueue();
    void finishActionQueueVerification(ActionQueueVerification result);
    int reservedActionRunnerCount() const;
    void requeueOrphanedActionRuns();
    void refreshQueuedRunReasons();
    // An encrypted repository is served out of a temporary materialization whose
    // directory is recreated by every sealing pass and deleted as soon as the
    // replacement is installed. A run checks out of, clones from, and lands
    // release artifacts into that directory for its whole lifetime, so a routine
    // re-seal (publishing a release triggers one) can delete the mirror out from
    // under an in-flight build: `git -C <mirror> worktree add` then fails with
    // "cannot change to '/tmp/ForkMesh-XXXXXX/repository.git'".
    // Returns the live materialization for this repository — writing its current
    // path into `mirrorPath` when the record lagged behind a re-seal — and keeps
    // that directory alive for as long as the caller holds the returned handle.
    // Null for a plain durable mirror, which needs no pinning.
    std::shared_ptr<void> pinActionMirror(const RepositoryRecord &repo,
                                          QString *mirrorPath) const;
    void releaseActionMirrorPin(int runId);
    ActionRunner *runnerForRun(int runId) const;
    void cancelSupersededRuns(const ActionRun &newRun);
    void onRunLog(int runId, const QString &text);
    void onRunStepChanged(int runId, int stepIndex, const QString &status);
    // Files the run's Ping and pop-up, each gated per event (adhoc #384).
    void notifyActionEvent(forkmesh::ui::ActionPingEvent event,
                           const QString &title, const QString &body,
                           bool warning, int runId = -1);
    struct AppNotification;
    void addNotification(const QString &title, const QString &body,
                         bool warning = false, int runId = -1);
    void addNotification(const QString &title, const QString &body, bool warning,
                         const NotificationLink &link);
    void addNotification(const QString &title, const QString &body, bool warning,
                         const NotificationLink &link, const QString &kind,
                         const QString &actor);
    qint64 recordNotification(AppNotification item);
    QString forwardPingToPhones(const AppNotification &item);
    void deleteForwardedPings(const QStringList &pingIds);
    void handleLocalControlPingsRequest(int connectionId, int requestId,
                                        const QString &method, const QJsonObject &params);
    forkmesh::pingfixes::Context pingFixContext(const AppNotification &item);
    const AppNotification *findPingForFix(const QJsonObject &params) const;
    QJsonObject pingFixesReply(const AppNotification &item);
    bool applyPingFix(const AppNotification &item, const QString &fixId, bool automatic,
                      QString *message, QString *error);
    void notePingForFixes(const AppNotification &item);
    void notePingFixRunFinished(const ActionRun &run);
    void addPingFixMenuActions(QMenu *menu, qint64 localId);
    void announceDesktopOnline(qint64 nowMs);
    void startPowerAlertWatch();
    void startAgentParityWatch();
    void checkAgentParity();
    void raisePowerAlert(const forkmesh::pingforward::Source &source);
    forkmesh::poweralert::Reading currentPowerReading();
    void wakeLocalMeshPhones(const QStringList &tokens);
    bool preparePingForwardRequest(const forkmesh::pingforward::Ping &ping,
                                   QNetworkRequest *request, QByteArray *body);
    bool signPingForwardRequest(const QString &proof, const QString &resource,
                                QNetworkRequest *request);
    bool phoneLinkedForPings() const;
    forkmesh::PingSync initialPingSync(const AppNotification &item,
                                       QString *reasonOut) const;
    void trimLocalPings();
    void setPingSync(qint64 pingId, forkmesh::PingSync state,
                     const QString &reason = QString());
    QString notificationJournalPath() const;
    void loadNotificationJournal();
    void saveNotificationJournal();
    void scheduleNotificationJournalSave();
    void flashNotification(const AppNotification &item);
    void acknowledgePing(qint64 pingId);
    QPixmap chatActorAvatar(const QString &senderId, const QString &senderName,
                            int side) const;
    QString chatNotificationIcon(const QString &senderId,
                                 const QString &senderName) const;
    // Unrounded picture for a chat sender: the sender id first, then the
    // account the name (or the sending node's owner) belongs to. Website
    // avatars are cached under user:<account>, never under a node id.
    QPixmap chatSenderAvatarSource(const QString &senderId,
                                   const QString &senderName) const;
    void refreshVisibleRowAvatars();
    void flashErrorBorder();
    void flashCelebrationBorder();
    void destroyTransientOverlays();
    void startRestartCautionFlash();
    void stopRestartCautionFlash();
    void openNotificationLink(const NotificationLink &link);
    void showNotifications();
    void refreshNotificationsTable();
    void deleteSelectedNotifications();
    void deleteWebAlert(const QString &alertId);
    void openNotificationRow(int row);
    void openNotificationRepository(int row);
    QString notificationRowKey(int row) const;
    int notificationRowForKey(const QString &key) const;
    void showPingDetail(int row);
    void renderPingDetail(int row);
    void refreshPingDetail();
    void hidePingDetail();
    void markWebAlertRead(const QString &alertId);
    // Drop one ping into the footer prompt composer, ready to send to an agent.
    void sendPingToPrompt(const QString &text);
    static QString notificationLinkLabel(const NotificationLink &link);
    void updateNotificationButton();
    // Pause Ping notifications until `untilMs` (0 resumes). Pings continue to
    // be filed; this persists the deadline, refreshes every surface that
    // renders it, and hands the new value to the relay on the next signed
    // heartbeat so the browser agrees.
    void setPingPause(qint64 untilMs);
    // Menu of the offered pauses — an hour, a day, next week, a custom
    // deadline — plus Resume once one is running. Shared by the Pings page
    // button and the Settings tab so both offer exactly the same choices.
    void popupPingPauseMenu(QWidget *anchor);
    void refreshPingPauseSurfaces();
    void updateWebAlertPager();
    void setWebAlertsPage(int page);
    void refreshWebAlerts(bool force = false);
    void celebrateFreshWebAlerts();
    void clearWebAlerts();
    void updateNavRebuildButton();
    void applyRailCaptionPreference();
    void applyRailOrder();
    void recordRailClick(const QString &key);
    RailFolders::State &railFolderState();
    bool isInRail(QWidget *widget) const;
    void saveRailFolderState();
    void hookRailWidget(QWidget *widget);
    bool handleRailEvent(QObject *watched, QEvent *event);
    void showRailItemMenu(QWidget *widget, const QPoint &globalPos);
    void showRailFolderMenu(RailFolderWidget *folder, const QPoint &globalPos);
    void moveRailItemToFolder(const QString &key, const QString &folderId);
    void promptNewRailFolder(const QString &key);
    void renameRailFolder(const QString &folderId);
    void deleteRailFolder(const QString &folderId);
    void refreshRailFolders();
    void saveRailOrder(const QStringList &keys);
    RailGroups::SortMode railSortMode() const;
    void setRailSortMode(RailGroups::SortMode mode);
    bool railGrouped() const;
    void setRailGrouped(bool grouped);
    RailGroups::State &railGroupState();
    void saveRailGroupState();
    QPushButton *buildRailSortButton();
    void refreshRailSortButton();
    QMenu *buildRailSortMenu(QWidget *parent);
    void showRailSortMenu();
    void addRailGroupActions(QMenu *menu, const QString &key);
    void showRailGroupMenu(RailGroupSeparator *separator, const QPoint &globalPos);
    void promptNewRailGroup(const QString &key);
    void renameRailGroup(const QString &groupId);
    void deleteRailGroup(const QString &groupId);
    void moveRailGroup(const QString &groupId, int step);
    void assignRailItemToGroup(const QString &key, const QString &groupId);
    void adoptRailGroupsFromLayout();
    void refreshRailGroupSeparators();
    void scheduleRailCountSort();
    bool canPopOutRailItem(QWidget *item) const;
    void popOutRailItemLater(QWidget *item, QWidget *spinner = nullptr);
    QWidget *popOutRailItem(QWidget *item);
    void popOutStackPage(QStackedWidget *stack, int index, const QString &key,
                         const QString &title);
    void attachPageDragOut(QAbstractButton *button, std::function<void()> popOut);
    void popOutLogPage();
    void catchUpLogPage();
    void dockPoppedOutPage(int at);
    void dockPoppedOutPages();
    void raisePoppedOutPages();
    QList<QPair<QString, QWidget *>> railHomeButtons() const;
    void placeMovableButtons();
    void applyRepoTabRowOrder();
    void applyRepoCodeBarOrder();
    QBoxLayout *buttonPanelLayout(const QString &panel) const;
    void moveButtonToPanel(const QString &key, const QString &panel);
    void resetButtonPlacement();
    void attachButtonPlacementMenu(QWidget *button);
    void addButtonPlacementActions(QMenu *menu, QWidget *button);
    RailShortcuts::State &railShortcutState();
    void saveRailShortcutState();
    void syncRailShortcutTiles();
    void activateRailShortcut(const QString &id);
    void removeRailShortcut(const QString &id);
    bool handlePageButtonEvent(QObject *watched, QEvent *event);
    bool isPageButtonCandidate(QAbstractButton *button) const;
    bool isDraggablePageButton(QAbstractButton *button);
    QStringList &knownPageButtons();
    void clickPageButton(QAbstractButton *button);
    void notePageButtonNavigated();
    QString pageButtonPath(QWidget *button) const;
    QAbstractButton *pageButtonAtPath(const QString &path) const;
    QAbstractButton *pageButtonForShortcut(const RailShortcuts::Shortcut &shortcut) const;
    bool handleRailPageButtonDrag(QEvent *event);
    QString dropPageButtonOnRail(QAbstractButton *button, int y);
    int railInsertIndexAt(int y) const;
    void showRailDropIndicator(int y);
    void hideRailDropIndicator();
    void applyWindowBackdrop();
    void applyWindowOpacity(int percent);
    void updateSignInButton();
    void showSignInMenu();
    void positionFloatingLogButton();
    void positionFooterLogPauseButton();
    void updateFooterLogPauseButton();
    int pendingActionCount() const;
    // Local pings filed since the Pings section was last opened. Synced ones
    // are left out: the relay files those into the website inbox, which
    // m_webAlertsUnread already counts.
    int unseenLocalPingCount();
    void markLocalPingsSeen();
    void openActionRunFromNotification(int runId);
    // `dismissOnly` asks the OS for a resident card too: one that stays in the
    // notification centre until it is clicked away, matching the in-app ping it
    // accompanies. Servers that ignore the hint simply show a normal card.
    // `fileLocally` false raises only the OS card, for an event whose Pings row
    // the relay files instead (a timer ping the phone is pushed too).
    void postNotification(const QString &title, const QString &body,
                          bool warning = false, const QString &icon = QString(),
                          const QString &activateHref = QString(),
                          bool dismissOnly = false, bool fileLocally = true);
    // Where a desktop notification takes you when it is clicked. The href is the
    // same "fm:agent:N" scheme the in-app notices already link with.
    NotificationLink notificationLinkForHref(const QString &href);
    void activateDesktopNotification(const QString &href);
    void onRunStatusChanged(int runId, const QString &status);
    void onRunFinished(int runId, bool ok);
    void onRunInterrupted(int runId);
    void onReleaseMetadataPrepared(int runId);
    void onReleaseMetadataLanded(int runId);
    void deployPendingReleaseRollout(
        const QString &owner, const QString &name,
        const PublishedReleaseDeployment &publishedRelease,
        bool managedRefsReady);
    void refreshActionsTable();
    void updateActionsSpinTimer();
    void animateRunningActionIcons();
    void showLatestVisibleActionRun();
    void showRun(int runId);
    // One extra run pane: its own title, meta line and streaming log, so the
    // run keeps updating while another one holds the primary detail pane.
    struct ParallelRunColumn {
        int runId = -1;
        QWidget *pane = nullptr;
        ElidingStatusLabel *title = nullptr;
        BusySpinner *spinner = nullptr;
        ElidingStatusLabel *meta = nullptr;
        ElidingStatusLabel *logPath = nullptr;
        QPlainTextEdit *log = nullptr;
        QPushButton *stopButton = nullptr;
        QPushButton *skipButton = nullptr;
        QPushButton *copyLogButton = nullptr;
        QPushButton *copyLogPathButton = nullptr;
        QPushButton *sendToAgentButton = nullptr;
        QPushButton *rerunButton = nullptr;
    };
    // Runs the user asked to follow beside the primary one. The runs table's
    // selection is the set: the current row drives the full detail pane, every
    // other selected row streams in its own column, so a whole fan-out of
    // queued and running workflows can be watched at once.
    void setParallelRunIds(QList<int> runIds);
    void closeParallelRun(int runId);
    void watchAllLiveRuns();
    QList<int> parallelRunIds() const;
    QList<int> openActionRunIds() const;
    ParallelRunColumn makeParallelRunColumn(int runId);
    void refreshParallelRunColumn(int runId, bool reloadLog = false);
    QPlainTextEdit *parallelRunLog(int runId) const;
    void selectActionRunRows(const QList<int> &runIds);
    void approveSelectedRun();
    void rejectSelectedRun();
    bool rejectActionRun(int runId);
    ActionRun *findRun(int runId);
    ActionNeeds::State actionRunNeedsState(int runId, QString *detail);
    QString actionRunQueueReason(int runId);
    QString actionRunMetaText(const ActionRun &run,
                              const QString &statusOverride = QString());
    QString actionRunLogPlaceholder(const ActionRun &run);
    void noteActionRunWaiting(int runId, const QString &detail);
    int repoIndexFor(const QString &owner, const QString &name) const;
    // Settings: global variables/secrets editor.
    void reloadVariablesList();
    void addOrEditVariable(const QString &name = QString());
    void deleteVariable(const QString &name);
    void exportVariables();
    void importVariables();
    void toggleVariablesRevealed();
    void persistVariablesFromTable();

    // landingTab: the repo-detail tab the load shows from its first paint
    // (-1 = Code). Opening on Code and clicking another tab afterwards
    // flashes the file list while the load pumps the event loop.
    void openRepoDetail(int repoIndex, int landingTab = -1);
    bool bindRepoDetailToRepo(int repoIndex);
    void openRepoDetailDeferred(int repoIndex, bool preservePage = false);
    void clearRepoDetail();
    void openRepositoryWebsite(); // open the current repo's page in the browser
    void forkCurrentRepo();       // clone the open repo into your own node
    bool configureForkSource(int index, const QString &sourceUrl,
                             bool notifyOnError = true);
    // Copy borrowed objects back into every local fork that reads this mirror
    // through git alternates, so removing the mirror cannot strand them.
    void detachForksBorrowingMirror(const QString &mirrorPath);
    void downloadCurrentRepoZip();
    // kind labels the resulting notice/ping; pass kLocalOperationNoticeKind for a
    // failure that is this machine's own business (ClientErrorReports.h).
    void setRepoDetailNotice(const QString &message, bool error = false,
                             const QString &kind = QString());
    void refreshOpenRepoDetail(); // re-read the open repo after its mirror changes
    void scheduleOpenRepoDetailRefresh();
    void updateRepoCodeSize();
    void updateRepoIssueCount();
    void updateRepoDiscussionCount();
    // Discussions rarely close, so an open-thread tally would pin the tab
    // badge at the total forever. The badge instead counts unread threads:
    // ones with activity newer than this machine's per-discussion read stamp
    // (QSettings "discussions/readStamps", stamped when a thread is shown).
    static QVariantMap discussionReadStamps();
    static QString discussionReadStampKey(const QString &repoKey, int number);
    void recordDiscussionRead(const QString &repoKey,
                              const Discussion &discussion);
    void updateRepoPullCount();
    void refreshRepoTabCounts();
    void loadRepoOverview(const QString &path);
    struct OverviewRead;
    struct OverviewLoad;
    static OverviewLoad readRepoOverview(const OverviewRead &request);
    void applyRepoOverview(quint64 generation, const QString &request,
                           const QString &path, const QString &dir,
                           const QString &keyPrefix, OverviewLoad load);
    QString overviewRequestFor(const QString &path) const;
    void fillOverviewEntryCommits(const QString &dir, const QString &ref,
                                  const QString &treeKey,
                                  const QString &loadedKey,
                                  const QStringList &paths);
    void showRepoOverview();
    void showRepoCoveExplorer();
    void showOverviewCommits();
    void showOverviewFiles();
    void showOverviewBranches();
    void showOverviewWorktrees();
    void updateRepoActivityRail();
    void setGitPromptOverlay(bool enabled);
    void positionGitPromptOverlay();
    void positionGlobalFooterOverlays();
    void setPromptOverlayCollapsed(bool collapsed);
    void setPromptOverlayDetached(bool detached);
    void resetPromptOverlayPlacement();
    void loadPromptOverlayPlacement();
    void savePromptOverlayPlacement();
    bool handlePromptPlacementEvent(QObject *object, QEvent *event);
    bool promptDragLeavesWindow(const QPoint &globalCursor,
                                const QRect &wanted) const;
    void tearOffPromptOverlay(const QPoint &globalCursor);
    void clampPromptOverlayIntoHost();
    void setLogOverlayExpanded(bool expanded);
    void setDebugLogTailVisible(bool visible);
    struct BrowsedRefRecovery {
        QString dir;
        QString gone;
        QString base;
        bool goneResolves = true;
        bool baseResolves = false;
    };
    static BrowsedRefRecovery readBrowsedRefRecovery(const QString &dir,
                                                     const QString &gone,
                                                     const QString &base);
    bool applyBrowsedRefRecovery(const BrowsedRefRecovery &recovery);
    QString browsedRefRecoveryBase() const;
    void seedDebugLogTail();
    void loadRepoFileTree();
    struct RepoFileTreeLoad {
        QString ref;
        QByteArray tree;
        QString treeError;
        BrowsedRefRecovery recovery;
        bool othersOk = false;
        QByteArray others;
    };
    static RepoFileTreeLoad readRepoFileTree(const QString &dir, const QString &ref,
                                             const QString &recoverBase,
                                             bool workingTree);
    void applyRepoFileTree(const QString &dir, const RepoFileTreeLoad &load);
    quint64 m_repoFileTreeGen = 0;
    bool m_repoFileTreeLoading = false;
    bool m_repoFileTreeReloadPending = false;
    void loadCoveExplorer();
    void refreshCoveExplorerTree();
    bool coveEditorIsReadOnly(const QWidget *editor) const;
    bool coveEditorIsModified(QWidget *editor) const;
    QString coveEditorBody(QWidget *editor) const;
    void setCoveEditorModified(QWidget *editor, bool modified);
    void openCoveExplorerDocument(const QString &path);
    void saveCurrentCoveExplorerDocument();
    bool persistCoveExplorerChange(Cove &current, const Cove &updated,
                                   QString *error = nullptr);
    void createCoveExplorerCove();
    void inviteUserToCurrentCove();
    void createCoveExplorerDocument();
    void deleteCurrentCoveExplorerDocument();
    QString coveAccountName();
    bool coveInviteAccountValid(const QString &accountName, QString *error);
    void showRepoFileTreeMenu(const QPoint &pos);
    void newRepoFileEntry(const QString &parentDir, bool folder);
    void renameRepoFileEntry(const QString &path, bool isDir);
    void deleteRepoFileEntry(const QString &path, bool isDir);
    void closeRepoFileTabsUnder(const QString &path, bool isDir);
    struct RepoFileOpRequest {
        enum Kind { Create, Rename, Delete };
        Kind kind = Create;
        QString path;
        QString dest;
        QString trackRel;
        bool isDir = false;
    };
    QString repoFileOpBranch();
    static std::pair<bool, QString> runRepoFileOp(const QString &dir,
                                                  const QString &branch,
                                                  const RepoFileOpRequest &op);
    void startRepoFileOp(const QString &branch, const RepoFileOpRequest &op);
    bool m_repoFileOpRunning = false;
    void finishRepoFileOp(const QString &base);
    void openRepoFile(const QString &path, std::function<void()> opened = {});
    void showRepoFileContent(const QString &dir, const QString &path,
                             const forkmesh::ui::GitCaptureResult &read);
    QHash<QString, QList<std::function<void()>>> m_repoFileOpenWaiters;
    void openWorkingTreeFile(const QString &dir, const QString &relPath);
    bool saveWorkingTreeFileEdit(const QString &absPath, const QString &content);
    void openRepoReadme(); // open the repo's README in a file tab (default view)
    void updateRepoFileSaveActions();
    void saveCurrentRepoFile(bool createPull);
    void toggleRepoFileMarkdownPreview();
    // Markdown file tabs render like the overview README and open on that
    // preview; their links (and the README's) open repository paths in place.
    void configureRepoMarkdownPreview(QPlainTextEdit *editor, const QString &path);
    // By value: the git lookup pumps the event loop, which can close the tab
    // that owns the link's view and its captured path.
    void openRepoMarkdownLink(QTextBrowser *view, QString basePath, QUrl url);
    void openRepoMarkdownTarget(const QString &dir,
                                const MarkdownRender::RepoLink &link,
                                bool directory);
    void showRepoFileHistory(const QString &path);
    void showRepoFileHistoryDialog(const QString &dir, const QString &path,
                                   const forkmesh::ui::GitCaptureResult &read);
    void saveRepoFileEdit(const QString &path, const QString &content, bool createPull,
                          std::function<void(bool)> done = {});
    void continueRepoFileEdit(const QString &dir, const QString &cleanPath,
                              const QString &content, bool createPull,
                              std::function<void(bool)> finish);
    struct RepoFileEditCommit {
        enum State { Failed, NoChanges, Committed };
        State state = Failed;
        QString message;
        QByteArray diff;
        QByteArray mbox;
        int pullNumber = -1;
        QString pullError;
    };
    static RepoFileEditCommit commitRepoFileEdit(const QString &dir, const QString &base,
                                                 const QString &cleanPath,
                                                 const QString &content, bool createPull,
                                                 const QString &branch,
                                                 const QString &title);
    bool m_repoFileSaveRunning = false;
    // True when the open repo can receive a proposed change as a pull request even
    // without a local working tree: a node mirroring someone else's repo builds the
    // commit in a throwaway worktree off its bare mirror and sends the signed patch
    // to the owner's inbox. False for preview-only or empty repos.
    bool repoCanProposePull() const;
    // No-working-tree path for "Save as PR": commit the edit in a temporary worktree
    // off the bare mirror, then submit the resulting signed patch to the owner's inbox.
    bool proposePullFromMirrorEdit(const QString &cleanPath, const QString &content,
                                   std::function<void(bool)> done = {});
    static RepoFileEditCommit buildMirrorEditPull(const QString &mirror,
                                                  const QString &base,
                                                  const QString &cleanPath,
                                                  const QString &content,
                                                  const QString &title,
                                                  const QString &authorName,
                                                  const QString &authorEmail);
    bool sendMirrorEditPull(const RepositoryRecord &repo, PullStore store, bool current,
                            const QString &cleanPath,
                            const QString &base, const QString &branch,
                            const QString &title, const QString &description,
                            const RepoFileEditCommit &commit);
    void loadRepoInfo();
    void applyRepoInfoJson(const QByteArray &raw);
    void refreshRepoInfoViews();
    quint64 m_repoInfoLoadGen = 0;
    void loadBranchesAndTags();
    struct BranchesTagsSnapshot {
        QString dir;               // repoGitDir()
        QString localPath;         // working copy for the worktree count
        QString configuredDefault; // m_repoInfo.defaultBranch
        QString checkedOut;        // m_repoBranch at issue time
        QString base;          // default branch (chooseDefaultBranch)
        QString head;          // checked-out branch (empty = detached/none)
        bool checkedOutGone = false; // browsed ref resolves to nothing any more
        QString branchRefsSig;
        BrowsedRefRecovery recovery;
        QStringList branches;  // local heads, committerdate-sorted
        int worktreeCount = 0; // linked worktrees, minus forkmesh/pulls
        struct Remote {
            QString name;
            QString fetchUrl;
            QString pushUrl;
        };
        QList<Remote> remotes;
        int tagCount = 0;
    };
    static BranchesTagsSnapshot readBranchesTagsGit(BranchesTagsSnapshot snap);
    void applyBranchesTags(const BranchesTagsSnapshot &snap);
    bool m_branchesTagsLoading = false;
    bool m_branchesTagsReloadQueued = false;
    QStringList repoBranches() const;
    static QStringList listRepoBranches(const QString &dir);
    struct BranchRefFiles {
        bool readable = false;
        QStringList branches;
        QString signature;
    };
    static BranchRefFiles readBranchRefFiles(const QString &dir);
    static QStringList orderBranchesLike(const QStringList &onDisk,
                                         const QStringList &gitOrder);
    struct BranchOrderLoad {
        QString dir;
        QString signature;
        QStringList branches;
    };
    void requestRepoBranchOrder(const QString &dir) const;
    void applyRepoBranchOrder(const BranchOrderLoad &load);
    mutable QStringList m_branchesGitOrder;
    mutable QString m_branchesGitOrderDir;
    mutable QString m_branchesGitOrderSig;
    mutable bool m_branchOrderLoading = false;
    mutable QString m_branchOrderPendingDir;
    static QString chooseDefaultBranch(const QStringList &branches,
                                       const QString &configured,
                                       const QString &dir,
                                       const QString &checkedOut,
                                       bool askGit = true);
    QString repoDefaultBranch(const QStringList &branches) const;
    QString repoDefaultBranchFast() const;
    QWidget *buildBranchesTab();
    struct BranchesPanelData {
        QString dir;
        QString configuredDefault; // m_repoInfo.defaultBranch, for the base pick
        QString checkedOut;        // m_repoBranch
        QString base;              // default branch (picked by the worker)
        QString selected;          // branch the repo view is parked on
        // m_branchDiffSeq when the load was asked for: a read of the viewed
        // branch started since then is as fresh as the refs this load reads.
        quint64 diffReadSeq = 0;
        bool writable = false;
        QStringList branches;       // local heads, default branch first
        QStringList remoteBranches; // refs/remotes/* (read-only rows)
        QHash<QString, qint64> times;
        QHash<QString, QString> shortShas;
        QHash<QString, QString> subjects;
        QHash<QString, QString> authors;
        QHash<QString, QPair<int, int>> localAheadBehind;
        QHash<QString, QPair<int, int>> remoteAheadBehind;
        QHash<QString, QString> worktrees; // branch -> linked worktree path
        QHash<QString, BranchRefHistory> histories;
    };
    struct BranchChangeStat {
        int files = -1;
        int added = -1;
        int removed = -1;
    };
    struct BranchDiffCacheEntry {
        QByteArray patch;
        QString emptyMessage;
    };
    static BranchesPanelData readBranchesPanelGit(BranchesPanelData data);
    void renderBranchesPanel(const BranchesPanelData &data);
    void refreshBranchLinks();
    void renderRemoteBranchRows(const BranchesPanelData &data, int &nextRow,
                                int &actionWidth, int &actionButtonMinWidth);
    void startBranchChangeStats(const BranchesPanelData &data);
    void loadBranchesPanel();
    QWidget *buildWorktreesTab();
    void loadWorktreesPanel();

    enum UiRefreshKind : unsigned {
        UiRefreshSourceControl = 1u << 0,
        UiRefreshSourceControlForce = 1u << 1,
        UiRefreshWorktrees = 1u << 2,
        UiRefreshBranches = 1u << 3,
        UiRefreshAgents = 1u << 4,
        UiRefreshIssues = 1u << 5,
        UiRefreshIssueReload = 1u << 6,
        UiRefreshAgentState = 1u << 7,
    };
    int m_uiRefreshBatchDepth = 0;
    unsigned m_uiRefreshPending = 0;
    bool deferUiRefresh(unsigned kind);
    void runPendingUiRefreshes();
    class UiRefreshBatch {
    public:
        explicit UiRefreshBatch(MainWindow *window) : m_window(window)
        {
            if (m_window)
                ++m_window->m_uiRefreshBatchDepth;
        }
        ~UiRefreshBatch()
        {
            if (!m_window)
                return;
            if (--m_window->m_uiRefreshBatchDepth == 0)
                m_window->runPendingUiRefreshes();
        }
        UiRefreshBatch(const UiRefreshBatch &) = delete;
        UiRefreshBatch &operator=(const UiRefreshBatch &) = delete;

    private:
        MainWindow *m_window = nullptr;
    };
    void focusRepoDetailTable(int id);
    void runGitDetached(const QString &dir, const QStringList &args,
                        std::function<void(bool, const QByteArray &)> onDone,
                        const forkmesh::LogSource &source =
                            forkmesh::logSourceHere(),
                        bool mergeStderr = false);
    // runOffThread() posts through a raw MainWindow receiver. Keep every such
    // thread accounted for until it has stopped, so restart teardown cannot
    // leave a late result addressing the destroyed QObject/widget tree.
    QSet<QThread *> m_offThreadWorkers;
    // The apply functor is GUI state too: callers may capture QPixmap/QIcon or
    // another GUI-affine value in it. Keep a GUI-thread owner until the worker
    // is reaped so a discarded teardown result cannot destroy that state from
    // inside QThread::create's worker callable.
    QHash<QThread *, std::shared_ptr<void>> m_offThreadApplyHolders;
    std::atomic_bool m_acceptOffThreadResults{true};
    void drainOffThreadWorkers();
    template <typename T>
    void runOffThread(std::function<T()> work, std::function<void(T)> apply)
    {
        if (!m_acceptOffThreadResults.load(std::memory_order_acquire))
            return;
        auto applyHolder =
            std::make_shared<std::function<void(T)>>(std::move(apply));
        QThread *worker = QThread::create(
            [this, work = std::move(work), applyHolder,
             routine = forkmesh::BackgroundRoutine::current()]() mutable {
                // The routine that asked is named on the caller's thread;
                // name it here too so tickets the work opens carry it.
                const forkmesh::BackgroundRoutine scope(routine);
                T result = work();
                if (!m_acceptOffThreadResults.load(std::memory_order_acquire))
                    return;
                QMetaObject::invokeMethod(
                    this,
                    [this, applyHolder,
                     result = std::move(result)]() mutable {
                        deliverOffThreadResult(std::move(*applyHolder),
                                               std::move(result));
                    },
                    Qt::QueuedConnection);
            });
        worker->setParent(this);
        m_offThreadWorkers.insert(worker);
        m_offThreadApplyHolders.insert(worker, applyHolder);
        connect(worker, &QThread::finished, this, [this, worker] {
            m_offThreadWorkers.remove(worker);
            m_offThreadApplyHolders.remove(worker);
            worker->deleteLater();
        });
        worker->start();
    }
    template <typename T>
    void deliverOffThreadResult(std::function<void(T)> apply, T result)
    {
        if (forkmesh::ui::inKeepAlivePump()) {
            QPointer<MainWindow> guard(this);
            forkmesh::ui::deferUntilAfterKeepAlivePump(
                [guard, apply = std::move(apply),
                 result = std::move(result)]() mutable {
                    if (guard)
                        guard->deliverOffThreadResult(std::move(apply),
                                                      std::move(result));
                });
            return;
        }
        apply(std::move(result));
    }
    bool deferredOutOfKeepAlivePump(bool &pending, std::function<void()> work)
    {
        if (!forkmesh::ui::inKeepAlivePump())
            return false;
        if (pending)
            return true; // already queued for the next clean turn
        pending = true;
        QPointer<MainWindow> guard(this);
        forkmesh::ui::deferUntilAfterKeepAlivePump(
            [guard, &pending, work = std::move(work)]() mutable {
                if (!guard)
                    return;
                pending = false;
                work();
            });
        return true;
    }
    int m_worktreeStatusGen = 0;
    void switchToWorktree(const QString &branch);
    void switchToBranch(const QString &branch, int agentSessionId = -1);
    int branchDiffAgentSessionId() const;
    void refreshBranchAgentButton();
    void refreshBranchConflictButton();
    QString m_branchCompareBase;
    QString branchCompareBase() const;
    void setBranchCompareBase(const QString &base);
    void closeBranchCompareView();
    bool selectWorktreeRow(const QString &branch);
    bool selectBranchRow(const QString &branch);
    void updateWorktreeSelection(const QString &branch,
                                 const QString &worktreePath);
    quint64 m_worktreeConflictProbeGen = 0;
    void requestMergeWorktreeIntoMain(const QString &branch,
                                      const QString &worktreePath = QString(),
                                      bool cleanUp = false,
                                      bool findWorktree = false);
    void mergeWorktreeIntoMain(const QString &branch,
                               const QString &worktreePath = QString(),
                               bool cleanUp = false,
                               std::function<void(bool)> done = {});
    struct WorktreeMergeFlow {
        QString dir;
        QString branch;
        QString base;
        QString worktreePath;
        bool cleanUp = false;
        QString owner;
        QString name;
        QString localPath;
        QString mirrorPath;
        const ForkMeshIdentity *identity = nullptr;
        QString userName;
        int idleAgentId = 0;
        bool agentWorking = false;
    };
    struct WorktreeMergeStep {
        enum Kind { Refused, NeedsEditor, EditorUnresolved, Attempted };
        Kind kind = Refused;
        QString message;
        QString noticeKind;
        bool error = true;
        bool reloadPanels = false;
        bool merged = false;
        bool hasConflicts = false;
        bool branchInBase = false;
        QString mergeError;
        QString mergeCommit;
        QStringList logs;
    };
    static WorktreeMergeStep prepareWorktreeMerge(const WorktreeMergeFlow &flow);
    static WorktreeMergeStep mergeBranchIntoBase(const WorktreeMergeFlow &flow,
                                                 WorktreeMergeStep step);
    void applyWorktreeMergeStep(const WorktreeMergeFlow &flow,
                                WorktreeMergeStep step,
                                std::function<void(bool)> done);
    void finishWorktreeMerge(const WorktreeMergeFlow &flow,
                             const WorktreeMergeStep &step, bool removed,
                             bool branchDeleted, std::function<void(bool)> done);
    void deletePullsOfMergedBranch(const WorktreeMergeFlow &flow,
                                   const QList<int> &sessionPulls,
                                   std::function<void(QList<int>, QString)> onDone);
    void finishMergedBranchCleanup(const WorktreeMergeFlow &flow, bool success,
                                   const QList<int> &deletedPulls,
                                   const QString &pullError);
    bool m_worktreeMergeRunning = false;
    struct QueuedWorktreeMerge {
        QString dir;
        QString owner;
        QString name;
        QString branch;
        QString worktreePath;
        bool cleanUp = false;
        std::function<void(bool)> done;
    };
    QList<QueuedWorktreeMerge> m_queuedWorktreeMerges;
    void runNextQueuedWorktreeMerge();
    bool mergeAgentBranchIntoBase(int repoIndex, const QString &branch,
                                  bool cleanUp,
                                  std::function<void(bool)> done = {});
    void updateWorktreeFromMain(const QString &worktreePath, const QString &branch,
                                const QString &baseArg = QString(),
                                std::function<void()> done = {});
    void editWorktreeConflicts(const QString &worktreePath, const QString &branch,
                               const QString &base, std::function<void(bool)> done);
    struct WorktreeUpdateRead {
        bool dirty = false;
        bool merged = false;
    };
    QSet<QString> m_worktreeGitBusy;
    void resolveWorktreeConflicts();
    void commitWorktreeChanges(const QString &worktreePath, const QString &branch);
    void removeWorktree(const QString &worktreePath, const QString &branch,
                        bool confirm, bool alsoDeleteBranch = true,
                        std::function<void()> onDone = {});
    struct WorktreeRemoval {
        bool refused = false;
        bool branchAttempted = false;
        bool branchDeleted = false;
        QString branchError;
        QStringList logs;
    };
    static WorktreeRemoval removeWorktreeAndBranch(const QString &repoPath,
                                                   const QString &worktreePath,
                                                   const QString &branch,
                                                   bool deleteBranch);
    bool repoDetailShows(const QString &localPath) const;
    void noticeForRepo(bool here, const QString &message, bool error = false,
                       const QString &kind = QString());
    // A worktree whose directory has been removed is still listed by git until
    // someone prunes it, and running a read in it only yields "cannot change to
    // '<dir>'". Lookups default to live worktrees; cleanup paths that exist to
    // remove the leftover admin entry ask for Any.
    enum class WorktreeLookup { Live, Any };
    QString worktreePathForBranch(QString repoPath, QString branch,
                                  WorktreeLookup lookup = WorktreeLookup::Live) const;
    static QString readWorktreePathForBranch(
        const QString &repoPath, const QString &branch,
        WorktreeLookup lookup = WorktreeLookup::Live);
    static QString worktreePathInListing(const QByteArray &listing,
                                         const QString &repoPath,
                                         const QString &branch,
                                         WorktreeLookup lookup);
    bool localBranchExists(const QString &repoPath, const QString &branch) const;
    void showBranchDiff(const QString &branch, int agentSessionId = -1);
    void showBranchDiffBranchGone(QString branch, QString base);
    QList<RepoSecurityFinding> addSecretIgnoreMarkers(
        const QString &workDir, const QList<RepoSecurityFinding> &findings);
    void ignoreBranchDiffSecrets(const QList<RepoSecurityFinding> &findings);
    void ignoreScmDiffSecrets(const QList<RepoSecurityFinding> &findings);
    void ignoreAgentDiffSecrets(const QList<RepoSecurityFinding> &findings);
    void applyBranchDetailActions(const QString &branch, const QString &base,
                                  int behind, int ahead, bool hasConflict,
                                  bool worktreeConflict = false,
                                  bool conflictCheckPending = false);
    int m_branchDetailActionsGen = 0;
    // A leftover .git lock stops every write in the branch's checkout, and no
    // amount of reading the diff explains why. Re-resolve which git dir the
    // browsed branch writes through, watch it, and offer to remove the lock.
    void refreshBranchLockState();
    void updateBranchLockButton(); // re-stat m_branchLockDir; no git process
    void clearBranchGitLock();
    void renderBranchScopeDiff();
    QString branchWorkDir(const QString &branch) const;
    // Renders a small patch on the GUI thread and a wide one on a worker that
    // streams it in (docs/desktop/branch-diffs.md). `done` runs after the page
    // is applied, or never if a newer render or another branch superseded it.
    void renderBranchDiffPatch(
        const QString &patch, const QString &emptyMessage,
        const QString &viewedContext, std::function<void()> done = {},
        std::optional<QHash<QString, QString>> fingerprints = std::nullopt);
    // The same on the GUI thread, for the cached repaint of a branch already
    // reviewed: the pane promises it back on the very next turn of the event
    // loop (adhoc #227). A large cached patch goes through the worker render.
    void renderBranchDiffPatchNow(const QString &patch, const QString &emptyMessage,
                                  const QString &viewedContext);
    void renderBranchDiffPatchOn(
        const QString &patch, const QString &emptyMessage,
        const QString &viewedContext, std::function<void()> done, bool guiThread,
        std::optional<QHash<QString, QString>> fingerprints = std::nullopt);
    void applyBranchDiffChunk(int gen, const QString &chunk, int filesDone,
                              int filesTotal);
    void applyBranchDiffRender(const forkmesh::ui::BranchDiffRender &render,
                               const QString &patch, const QString &message,
                               const QString &viewedContext, bool comments,
                               const QString &renderKey, bool progressive);
    void cancelBranchDiffRender();
    void setBranchDiffHtml(const QString &html, bool endCap = false);
    void syncBranchDiffAgentStat(int agentSessionId);
    int m_branchDiffRenderGen = 0; // supersedes a worker render still in flight
    std::shared_ptr<std::atomic<bool>> m_branchDiffRenderCancel;
    QString m_branchDiffRenderKey;
    QString m_branchDiffShownIdentity;
    // The merge-health inputs as one string, so a report scored on a worker
    // can be told apart from one the inputs have since moved on from.
    QString branchDiffHealthInputKey() const;
    std::shared_ptr<forkmesh::ui::DiffHealthReport> m_branchDiffHealthReport;
    QString m_branchDiffHealthReportKey;   // inputs the stored report describes
    QString m_branchDiffHealthReportPatch; // patch the stored report describes

    // The activity log above the range pane's diff: every step of reading,
    // rendering and laying out the branch's changes, as it happens, so a long
    // load says what it is doing instead of sitting on one placeholder line.
    // Progress lines replace the previous Progress line in place (a byte
    // counter ticking); ProgressDone replaces it too but then stays.
    enum class BranchDiffLogKind {
        Step, Info, Git, Progress, Ok, Warn, Fail, ProgressDone
    };
    QWidget *m_branchDiffLogPanel = nullptr;
    QLabel *m_branchDiffLogTitle = nullptr;
    QTextEdit *m_branchDiffLog = nullptr;
    QPushButton *m_branchDiffLogToggle = nullptr;
    QPushButton *m_branchDiffLogButton = nullptr;
    bool m_branchDiffLogCollapsed = true;
    qint64 m_branchDiffLogTookMs = -1;
    bool m_branchDiffLogFollow = true; // tail the log unless the reader scrolled up
    bool m_branchDiffLogLastWasProgress = false;
    bool m_branchDiffLogAwaitingLayout = false;
    int m_branchDiffLogGen = 0;       // lines from an older load are dropped
    QString m_branchDiffLogRoutine;   // BackgroundRoutine name of the live load
    QElapsedTimer m_branchDiffLogClock;
    QWidget *buildBranchDiffLogPanel();
    void beginBranchDiffLog(const QString &title);
    void appendBranchDiffLog(const QString &line,
                             BranchDiffLogKind kind = BranchDiffLogKind::Info);
    // Safe from any thread: queues the line to the GUI thread, where it is
    // dropped if the load it belongs to is no longer the one on screen.
    void postBranchDiffLog(int logGen, const QString &line, BranchDiffLogKind kind);
    std::function<void(const QString &, int)> branchDiffLogSink(int logGen);
    void setBranchDiffLogCollapsed(bool collapsed);
    void updateBranchDiffLogButton(const QString &line = QString());
    void noteBranchDiffTicket(const forkmesh::BackgroundTicket &ticket,
                              bool started);
    void rebuildBranchDiffSpans();
    void updateBranchDiffSummary(const QList<forkmesh::ui::DiffFileEntry> &files,
                                 const QSet<QString> &viewed,
                                 const QString &patch,
                                 const QString &viewedContext);
    void updateBranchDiffHealth(const QList<forkmesh::ui::DiffFileEntry> &files,
                                const QString &patch);
    // Fold/reopen one file of the branch diff in place; false when the document
    // cannot carry the swap and the caller has to re-render the whole diff.
    bool swapBranchDiffFileInPlace(const QString &path, const QString &context);
    void updateRangeViewedLabel(const QStringList &paths);
    QWidget *buildBranchRangePane();
    void buildBranchDiffSummaryBar();
    void buildBranchDiffSearchBar();
    void computeBranchFileTops();
    void applyBranchAutoMarkViewedOnScroll();
    bool branchScrollToAdjacentHunk(int delta);
    void toggleBranchDiffSearch(bool show);
    void branchDiffSearchRecompute();
    void branchDiffSearchGoTo(int delta);
    void updateBranchDetailActions(const QString &branch);
    void createPullFromBranch(const QString &branch,
                              std::function<void(int)> done = {});
    void onBranchDiffAnchorClicked(const QUrl &url);
    void updateBranchDiffSticky();
    QString diffViewedScope(const QString &context) const;
    QSet<QString> loadDiffViewed(const QString &context) const;
    void setDiffViewed(const QString &context, const QString &path, bool viewed);
    // Viewed is a statement about the file as it stood when it was ticked, so
    // every render re-reads the patch and drops the mark from any file whose
    // diff has moved since. Returns the viewed set to render with — call this
    // rather than loadDiffViewed() wherever the patch is in hand. Also the one
    // place that learns the current fingerprints, which setDiffViewed() stamps
    // onto the marks it writes.
    QSet<QString> refreshDiffViewedForPatch(const QString &context,
                                            const QString &patch);
    // The same, for fingerprints a worker already computed off the patch.
    QSet<QString> refreshDiffViewedForFingerprints(
        const QString &context, const QHash<QString, QString> &fingerprints);
    // path -> the fingerprint stored with its Viewed mark, empty for a mark
    // written before fingerprints (or off a patch this window never rendered).
    QHash<QString, QString> loadDiffViewedStamps(const QString &context) const;
    bool diffViewedMarksCurrent(const QString &context,
                                const QStringList &paths) const;
    void saveDiffViewedStamps(const QString &context,
                              const QHash<QString, QString> &stamps);
    // Files marked viewed that the reviewer has reopened anyway. Deliberately
    // not persisted like the viewed set: reopening one is a "let me look at
    // that again" for the sitting, not a standing preference, so a fresh visit
    // to the diff starts folded the way the review state says it should.
    QSet<QString> diffExpandedViewed(const QString &context) const;
    void setDiffExpandedViewed(const QString &context, const QString &path,
                               bool expanded);
    // Routes an "expand:" anchor from a diff row or its sticky header. True
    // when the URL was one, so callers can stop looking at it.
    bool toggleDiffExpandedFromAnchor(const QUrl &url, const QString &context);
    struct WorktreeMergeReport
    {
        enum Status {
            Merged,     // base is in; any local edits were restored on top
            Busy,       // a merge or unresolved files were already in the way
            Conflicted, // refused or rolled back; the branch is untouched
            Failed,     // couldn't be attempted at all
        };
        Status status = Failed;
        QString message; // ready to show, naming the branch and the base
        QStringList logs;
    };
    static WorktreeMergeReport mergeBaseIntoLinkedWorktree(const QString &worktree,
                                                           const QString &branch,
                                                           const QString &base);
    void updateBranchFromBase(const QString &branch);
    void openBranchMergeEditor(const QString &branch,
                               std::function<void()> done = {});
    struct BranchGitFlow {
        QString dir;
        QString branch;
        QString base;
        QString linkedWorktree;
        QString mergeDir;
        QString currentBranch;
        bool isCurrent = false;
        bool stashed = false;
    };
    struct BranchGitStep {
        enum Kind { Done, NeedsStashChoice, NeedsEditor, LocalConflicts };
        Kind kind = Done;
        BranchGitFlow flow;
        QString message;
        bool error = false;
        QString noticeKind;
        bool clearBranchesCache = false;
        bool showBranch = false;
        bool reloadWorktrees = false;
        bool reloadBranchesAndTags = false;
        QStringList conflicted;
        QStringList dirtyFiles;
        QStringList logs;
    };
    static BranchGitStep readBranchUpdate(BranchGitFlow flow);
    static BranchGitStep mergeBaseIntoBranchCheckout(BranchGitFlow flow,
                                                     bool stash);
    static BranchGitStep finishBranchUpdateEditor(BranchGitFlow flow,
                                                  bool committed);
    static BranchGitStep startBranchMergeEditor(BranchGitFlow flow);
    static BranchGitStep finishBranchMergeEditor(BranchGitFlow flow,
                                                 bool committed);
    static BranchGitStep keepResolvedLocalEdits(BranchGitFlow flow, int count);
    static QString putCheckoutBack(const BranchGitFlow &flow, QStringList *logs);
    static QString popBranchUpdateStash(BranchGitFlow *flow);
    void showBranchGitStep(const BranchGitStep &step);
    void applyBranchUpdateStep(BranchGitStep step);
    void applyBranchEditorStep(BranchGitStep step, std::function<void()> done);
    bool m_branchMergeFlowBusy = false;
    void pullBaseIntoAllBranches();
    struct PullAllBranchesResult {
        bool attempted = false;
        int updated = 0;
        QStringList failed;
        QStringList skipped;
    };
    static PullAllBranchesResult fastForwardInactiveBranches(
        const QString &dir, const QStringList &branches, const QString &base);
    bool m_pullAllBranchesRunning = false;
    struct BranchFixPrep {
        enum Kind { Refused, Merged, Conflicted };
        Kind kind = Refused;
        QString message;
        bool error = true;
        QString mergeDir;
        QString restoreBranch;
        QStringList conflicted;
        QStringList logs;
    };
    static BranchFixPrep prepareBranchConflictFix(const QString &dir,
                                                  const QString &branch,
                                                  const QString &base);
    void startBranchConflictFix(int repoIndex, const QString &owner,
                                const QString &name, const QString &branch,
                                const QString &base, const QString &provider,
                                const QString &model, const QString &apiKey,
                                bool claudeCode, const BranchFixPrep &prep);
    bool m_aiFixStarting = false;
    void fixBranchConflictsWithAgent(const QString &branch, const QString &provider,
                                     const QString &model = QString());
    void promptNewBranch();
    void deleteBranch(const QString &branch);
    void deleteReviewedBranchAndCleanUp(const QString &branch);
    // Branch management requested on the website (adhoc #2098): the
    // relay queues it for this node, /api/sync delivers it, and this
    // applies it to the checkout the mesh is published from.
    void applyWebBranchOps(const RepositoryRecord &repo,
                           const QJsonArray &ops);
    void drainWebBranchOpsFor(const RepositoryRecord &repo);
    bool webMergeBlockedByAgent(const QString &owner, const QString &name,
                                const QString &branch) const;
    void recordWebBranchMerges(
        const QString &owner, const QString &name, const QString &path,
        const QList<forkmesh::branchops::Outcome> &outcomes);
    void finishWebBranchOps(
        const QString &owner, const QString &name, const QString &path,
        const QString &base,
        const QList<forkmesh::branchops::Outcome> &outcomes);
    void reportWebBranchOpResults(const QString &owner, const QString &name,
                                  const QJsonArray &results);
    void deleteRemoteBranch(const QString &branch);
    void confirmDeleteRemoteBranch(const QString &dir, const QString &branch,
                                   const QString &remote, const QString &ref);
    void deleteMeshTrackingBranch(const QString &remote, const QString &ref);
    QString neighbourBranchInList(const QString &branch) const;
    int branchRowInList(const QString &branch) const;
    void flashMergedBranchRow(const QString &branch);
    void clearMergedBranchFlash();
    void startMergedBranchCelebration(const QString &branch,
                                      const QString &base,
                                      const QString &dir);
    void requestMergedBranchCelebrationHtml(const QString &branch,
                                            const QString &base,
                                            const QString &dir,
                                            std::function<void(QString)> onReady);
    QString branchNoChangesPaneHtml(const QString &branch, const QString &base,
                                    const QString &dir,
                                    const QString &emptyMessage,
                                    bool remember = true,
                                    std::function<void(QString)> onReady = {});
    QString renderBranchNoChangesPane(const QString &branch, const QString &base,
                                      const QString &dir,
                                      const QString &emptyMessage, int range,
                                      const struct MergeCelebrationRead &read);
    void showMergeRange(const QString &key);
    void deleteMergedBranches();
    void confirmDeleteMergedBranches(const RepositoryRecord &repo,
                                     const QString &dir, const QString &base,
                                     const QStringList &merged,
                                     const QStringList &mergedRemote);
    void deleteSelectedBranches();
    void refreshBranchesFromRemotes();
    QWidget *buildReleasesTab();
    void loadReleasesPanel();
    void promptNewRelease();
    void rememberReleaseRollout(const QString &owner, const QString &name,
                                const QString &tag);
    void rememberReleasePublication(const QString &owner,
                                    const QString &name,
                                    const QString &tag);
    void discardSupersededReleaseIntents(const QString &owner,
                                         const QString &name,
                                         const QString &tag);
    bool releasePublicationPending(const QString &owner,
                                   const QString &name,
                                   const QString &tag) const;
    bool releaseRolloutPending(const QString &owner, const QString &name,
                               const QString &tag) const;
    void clearReleaseRollout(const QString &owner, const QString &name,
                             const QString &tag);
    bool releasePublicationPendingForRepository(
        const QString &owner, const QString &name) const;
    void clearReleasePublication(const QString &owner, const QString &name,
                                 const QString &tag);
    void rememberReleaseArtifactPrune(const QString &owner,
                                      const QString &name,
                                      const QString &tag);
    bool releaseArtifactPrunePending(const QString &owner,
                                     const QString &name,
                                     const QString &tag) const;
    void clearReleaseArtifactPrune(const QString &owner,
                                   const QString &name,
                                   const QString &tag);
    bool publishedReleaseDeployment(
        int index, const QString &tag, const QString &buildCommit,
        PublishedReleaseDeployment *deployment, QString *error) const;
    void generateReleaseNotesWithAgent(const QString &dir, const QString &prevTag,
                                       const QString &newTag,
                                       const QString &targetRef,
                                       const QString &provider,
                                       const QString &modelChoice,
                                       QPlainTextEdit *notesEdit,
                                       QPushButton *button);
    void pruneReleaseArtifacts(int index, const QString &releaseTag,
                               const QStringList &keepHashes = QStringList());
    void pruneReleaseTagsForRepo(int repoIndex, const QString &dir,
                                 const QString &keepTag);
    // Saved managed mirrors are durable fleet targets, even when the working
    // copy has no ssh:// remote configured for them. Every source update uses
    // this direct authenticated controller channel; releases additionally use
    // the completion callback to sequence artifact deployment after the refs.
    int pushRefsToManagedHosts(
        int index, bool wakeIfUnchanged = true,
        const QString &releaseTag = QString(),
        const QStringList &releaseArtifactHashes = QStringList(),
        std::function<void(int succeeded, int failed)> onFinished = {},
        bool reconcileArtifacts = true, bool pruneRefs = false);
    int pushReleaseToManagedHosts(
        int index, const QString &tag,
        const QStringList &artifactHashes,
        std::function<void(int succeeded, int failed)> onFinished = {});
    // pushRefsToManagedHosts, split by phase (MainWindowMirrorPush.cpp).
    struct ManagedPushTarget;
    bool collectManagedPushArtifacts(
        const RepositoryRecord &repo, const QString &releaseTag,
        const QStringList &releaseArtifactHashes, bool releasePush,
        bool reconcileArtifacts,
        QList<forkmesh::ui::MirrorReleaseBlob> *releaseArtifacts);
    QList<ManagedPushTarget> managedPushTargets(
        const QList<forkmesh::ui::MirrorReleaseBlob> &releaseArtifacts,
        bool startArtifactReconciliation, int *unavailableTargets);
    void startManagedArtifactReconciliation(
        const QString &managedRepoKey, const QList<ManagedPushTarget> &targets,
        int unavailableTargets,
        const QList<forkmesh::ui::MirrorReleaseBlob> &releaseArtifacts);
    int seedManagedReleaseArtifacts(
        int index, bool wakeIfUnchanged, const QString &releaseTag,
        const std::function<void(int succeeded, int failed)> &onFinished,
        const QList<ManagedPushTarget> &targets, int unavailableTargets,
        const QList<forkmesh::ui::MirrorReleaseBlob> &releaseArtifacts);
    int pushManagedRefsToHosts(
        const RepositoryRecord &repo, const QString &managedRepoKey,
        const QStringList &targetHosts, QList<ManagedPushTarget> targets,
        int unavailableTargets, bool wakeIfUnchanged, bool pruneRefs,
        const QString &releaseTag,
        const std::function<void(int succeeded, int failed)> &onFinished);
    // Push a just-published release to the repo's fediverse followers via the
    // relay's owner-signed POST /ap-publish (releases never pass through the
    // signed inboxes, so the relay can't federate them on its own).
    void announceReleaseOnFediverse(int repoIndex, const QString &tag,
                                    const QString &title, const QString &notes);
    // Open a Mastodon composer prefilled with a just-published release, on the
    // instance from the account's Mastodon handle when one is set.
    void postReleaseToMastodon(int repoIndex, const QString &tag,
                               const QString &title, const QString &notes);
    void showReleaseDetail(const QString &tag);
    QWidget *buildArtifactsTab();
    void loadArtifactsPanel();
    QString artifactsMirrorRequest() const;
    void fillArtifactsTable(
        const QList<forkmesh::ui::MirrorReleaseBlob> &blobs,
        const QHash<QString, QString> &nameByHash,
        const QHash<QString, QString> &tagByHash, bool writable);
    quint64 m_artifactsLoadGen = 0;
    bool m_artifactsLoading = false;
    bool m_artifactsReloadPending = false;
    QString m_artifactsLoadRequest;
    QString m_artifactsShownRequest;
    QString releasesLoadRequest() const;
    void applyReleasesPanel(const QString &dir, bool tagsRead,
                            const QByteArray &tagsOut);
    quint64 m_releasesLoadGen = 0;
    bool m_releasesLoading = false;
    bool m_releasesReloadPending = false;
    QString m_releasesLoadRequest;
    QString m_releasesShownRequest;
    void promptNewReleaseThen(std::function<void(bool)> done);
    void draftNewRelease(const QString &dir, const QStringList &branches,
                         const QString &target, const QString &commandTag,
                         const QByteArray &latestTag,
                         std::function<void(bool)> done);
    bool m_newReleaseRunning = false;
    static bool releaseRolloutAllowed(const RepositoryRecord &repo);
    void cutRelease(const forkmesh::releasedraft::Cut &cut,
                    std::function<void(bool, const QString &)> done);
    void createReleaseRemotely(const forkmesh::agentintake::ReleaseRequest &request,
                               std::function<void(const QJsonObject &)> reply);
    void listReleasesRemotely(const forkmesh::agentintake::ReleaseRequest &request,
                              std::function<void(const QJsonObject &)> reply);
    void publishReleaseRemotely(const forkmesh::agentintake::ReleaseRequest &request,
                                std::function<void(const QJsonObject &)> reply);
    void handleAgentReleaseRequest(int sessionId,
                                   const forkmesh::agentintake::Request &request,
                                   std::function<void(const QJsonObject &)> reply);
    forkmesh::agentintake::RateLimiter m_agentReleaseLimiter{
        forkmesh::agentintake::kReleaseRateLimitCount, forkmesh::agentintake::kRateWindowMs};
    void sendReleaseNotesRequest(const QByteArray &log, const QString &prevTag,
                                 const QString &newTag,
                                 const QString &targetRef,
                                 const QString &provider,
                                 const QString &modelChoice,
                                 QPlainTextEdit *notesEdit, QPushButton *button);
    void continueReleasePushToMirrors(const QString &tag, const QString &owner,
                                      const QString &name,
                                      const QString &localPath,
                                      const QString &commit);
    QSet<QString> m_releaseMirrorPushResolving;
    void openReleaseDetailDialog(const QString &tag, const QString &when,
                                 const QString &tagger, const QString &subject,
                                 const QString &body, const QString &prevTag,
                                 const QString &diffHtml);
    quint64 m_releaseDetailGen = 0;
    QString m_releaseDetailLoading;
    QSet<QString> m_releaseTagDeletes;
    QSet<QString> m_releaseTagPrunes;
    void finishReleaseTagPrune(const QString &keepTag, int deleted, int failed,
                               bool current, int index);
    void deleteArtifact(const QString &hash, const QString &label);
    QWidget *buildMirrorNodesSection();
    QWidget *buildShortcutsTab();
    // Renders shortcuts and skills into one list, most-launched first. Skills
    // have no loader of their own — every skill entry point calls this.
    void loadShortcutsPanel();
    QString shortcutsDirPath() const; // <working tree>/.forkmesh/shortcuts, "" without one
    // Both take their path by value: they refresh the panel (which deleteLater()s
    // the very card whose closure owns the caller's string) and can pump the
    // event loop before they are done reading it.
    bool runShortcut(QString filePath, int repoIndex = -1,
                     QMap<QString, QString> environmentOverrides = {});
    bool shortcutRunning(const QString &filePath) const;
    QProcess *shortcutProcess(const QString &filePath) const;
    forkmesh::localcontrol::insights::ShortcutRun *shortcutRunById(int id);
    void appendShortcutOutput(int runId, const QString &chunk);
    void showShortcutRun(int runId);
    void refreshShortcutRunPicker();
    QWidget *shortcutProgressWidget(qint64 startedAtMs, qint64 expectedMs,
                                    int agentSessionId, bool compact);
    QWidget *activeShortcutProgress(const QString &filePath, const QString &scope,
                                    bool script, bool dedicated, qint64 lastMs,
                                    bool compact);
    void tickShortcutProgress();
    void draftShortcutPrompt(QString filePath,
                             QString scope = QStringLiteral("shortcut"));
    void stopShortcut();
    void stopShortcutRun(int runId);
    void openShortcutEditor(const QString &filePath);
    void deleteShortcut(const QString &filePath);
    // Launch tallies behind the "most used first" ordering. The scope is
    // "shortcut" for .forkmesh/shortcuts entries and the skill scope
    // ("repository", "app", "organization") for skills.
    QString shortcutUsageKey(const QString &filePath, const QString &scope,
                             int repoIndex = -1) const;
    int shortcutUsageCount(const QString &usageKey) const;
    // Counts a launch and stamps its time; returns the usage key ("" when the
    // entry has none) so the caller can report the outcome later.
    QString recordShortcutUsage(const QString &filePath, const QString &scope,
                                int repoIndex = -1);
    void forgetShortcutUsage(const QString &filePath, const QString &scope);
    // What each card's run line reads: when it last launched, how many
    // finished runs succeeded, and how long they took. Drafting a prompt only
    // stamps lastRunAtMs; scripts and dedicated agents also report an outcome.
    struct ShortcutRunStats
    {
        qint64 lastRunAtMs = 0;
        int finished = 0;
        int succeeded = 0;
        int timed = 0; // finished runs with a measured duration
        qint64 totalMs = 0;
        qint64 lastMs = -1;
        bool lastOk = false;
        QString lastOutput;
    };
    ShortcutRunStats shortcutRunStats(const QString &usageKey) const;
    QVariantMap shortcutRunStatsSettingValue() const; // kShortcutRunStatsSetting
    // durationMs < 0 means no measurement (for example bash never started).
    void recordShortcutRunResult(const QString &usageKey, bool ok,
                                 qint64 durationMs);
    void trackShortcutAgentRun(int sessionId, const QString &usageKey);
    // Called for every finished agent; a no-op unless a shortcut started it.
    void recordShortcutAgentFinished(int sessionId, bool ok, qint64 durationMs);
    QString skillsDirPath(const QString &scope, int repoIndex = -1) const;
    void recordAgentShortcutUse(int sessionId, const QString &filePath,
                                const QString &scope, int repoIndex);
    void recordAgentToolShortcutUses(int sessionId, const QJsonObject &toolUse);
    void recordTeammateShortcutUses(
        int sessionId, const QList<forkmesh::teammates::Config> &teammates,
        int repoIndex);
    void openSkillEditor(const QString &filePath = QString());
    void deleteSkill(const QString &filePath);
    QWidget *buildTeamSection();
    QWidget *buildTeamSectionHeader(QWidget *page);
    void addTeamSectionHeaderActions(HeaderWrapLayout *header, QWidget *page);
    QWidget *buildTeamStatsPage();
    void loadTeammatesPanel();
    // Roster card pieces for loadTeammatesPanel (MainWindowTeammates.cpp).
    struct TeammateRosterEntry;
    QPushButton *buildTeammateRosterCard(const TeammateRosterEntry &entry,
                                         const QString &latestOutput,
                                         const QString &handle);
    void fillTeammateCardStatus(QVBoxLayout *sectionLayout,
                                const TeammateRosterEntry &entry,
                                bool tableView, bool oneTime);
    void fillTeammateCardDuties(
        QVBoxLayout *sectionLayout, const forkmesh::teammates::Config &config,
        const std::function<void(const QString &)> &openDetailSection);
    void fillTeammateCardSkills(
        QVBoxLayout *sectionLayout, const forkmesh::teammates::Config &config,
        const std::function<void(const QString &)> &openDetailSection);
    void fillTeammateCardOutput(QVBoxLayout *sectionLayout,
                                const TeammateRosterEntry &entry,
                                const QString &latestOutput, bool tableView);
    void fillTeammateCardAssignment(QVBoxLayout *sectionLayout,
                                    const TeammateRosterEntry &entry,
                                    const QString &latestOutput,
                                    const QString &handle, bool oneTime);
    void appendTeammateCardBriefingActions(QVBoxLayout *sectionLayout,
                                           const TeammateRosterEntry &entry,
                                           const QString &latestOutput,
                                           const QString &handle);
    QFrame *buildTeammateRosterEmptyState(const QString &directory,
                                          bool noEntries);
    // Briefing is the Team section's daily read model: the full-team page
    // combines every teammate's contribution, while selecting a contributor
    // reveals the source assignment, output and working notes behind it.
    void loadTeammateBriefing();
    struct TeammateBriefingEntry;
    void wireTeammateBriefingReferenceLinks(QLabel *label);
    void addTeammateBriefingAvatar(QBoxLayout *layout,
                                   const forkmesh::teammates::Config &config,
                                   int side, const QString &objectName);
    void addTeammateBriefingSessionLinks(QVBoxLayout *layout,
                                         const AgentSession *session);
    QFrame *buildTeammateBriefingContribution(const TeammateBriefingEntry &entry);
    void appendTeammateBriefingDetail(QVBoxLayout *content, QFrame *hero,
                                      QVBoxLayout *heroLayout,
                                      const TeammateBriefingEntry &entry);
    void showTeammateBriefing(const QString &teammateId = QString());
    // Continues the team's shared agent entry with every available teammate's
    // role-scoped update (creating the entry only on its first run); when a
    // teammate is named, only that individual's lane is refreshed.
    void generateTeammateBriefing(const QString &teammateId = QString());
    // Continues every available prompt-based teammate's configured work in one
    // shared agent entry. The same primitive powers daily briefing runs.
    void runTeammateTeamNow();
    void runTeammatesInOneSessionNow();
    QWidget *buildTeammateRail();
    QWidget *buildTeammateRailItem(const forkmesh::teammates::Config &config,
                                   const QString &status, bool inFlight,
                                   bool doneUnseen,
                                   const AgentSession *session,
                                   const forkmesh::teammates::Mood &mood);
    QPushButton *buildTeammateVideoTile(const forkmesh::teammates::Config &config,
                                        const QString &status, bool inFlight,
                                        bool doneUnseen,
                                        const AgentSession *session,
                                        const forkmesh::teammates::Mood &mood,
                                        const QString &caption);
    QList<QPushButton *> addTeammateRailHud(QWidget *host,
                                            const forkmesh::teammates::Config &config,
                                            bool inFlight, int sessionId,
                                            const forkmesh::teammates::Mood &mood,
                                            const QString &namePrefix,
                                            int hudSide, int moodSide);
    void markTeammateDoneUnseen(int sessionId);
    void clearTeammateDoneUnseen(int sessionId);
    void loadAgentReadState();
    QString agentReadKey(const AgentSession &session) const;
    bool agentSessionUnread(const AgentSession &session) const;
    bool agentSessionWatched(int sessionId) const;
    bool noteAgentSessionSeen(const AgentSession &session);
    void markAgentSessionRead(int sessionId);
    QHash<QString, forkmesh::teammates::Mood>
    assessTeammateMoods(const QList<forkmesh::teammates::Config> &teammates,
                        qint64 nowMs);
    void refreshTeammateRail();
    void scheduleTeammateRailRefresh();
    bool teammateRailEnabled() const;
    void setTeammateRailEnabled(bool enabled);
    void applyTeammateRailVisibility();
    void showTeammateRailMenu(const QString &id, QWidget *anchor);
    QWidget *buildPackageRail();
    void refreshPackageRail();
    void schedulePackageRailRefresh();
    void preparePackageRails();
    void preparePackagePage(forkmesh::market::NativePackageWidget *widget);
    void preparePackagePages();
    bool packageRailEnabled() const;
    void setPackageRailEnabled(bool enabled);
    void applyPackageRailVisibility();
    bool packageRailReminderEnabled() const;
    void setPackageRailReminderEnabled(bool enabled);
    void checkPackageRailReminder();
    NotificationLink packageRailLinkForHref(const QString &href) const;
    bool openPackageRailLink(const NotificationLink &link);
    void openPackageRailTile(const QString &packageId, const QString &tileId);
    void showPackageRailMenu(const QString &packageId, const QString &tileId,
                             QWidget *anchor);
    void runTeammateFromRail(const QString &id);
    void processTeammatePayoutsFromRail(const QString &id);
    void openTeamSectionForTeammate(const QString &id, bool edit = false,
                                    bool focusSkills = false);
    void setTeammateScheduleActive(const QString &id, bool active);
    void setTeammateAvailable(const QString &id, bool available);
    QString teammateRosterStatus(const forkmesh::teammates::Config &config,
                                 const AgentSession *session) const;
    forkmesh::teammates::AgentChoice recentlyUsedAgentChoice() const;
    forkmesh::teammates::AgentChoice promptAreaAgentChoice() const;
    void createNeededTeammate();
    QString buildTeammateCreatorPrompt(int repoIndex) const;
    int activeTeammateCreatorSession() const;
    void finishTeammateCreatorRun(int sessionId);
    bool adoptCreatedTeammate(int repoIndex, const QString &output,
                              QString *name = nullptr);
    void updateTeammateAnalytics(
        const QList<forkmesh::teammates::Config> &teammates);
    // The chrome search is the Team filter while this section is visible. It
    // has no second page-local text box to drift out of sync.
    void syncTeammatePageSearch();
    // The chrome search is likewise the filter of whichever installed package
    // page is visible, through NativePackageWidget::setHostSearchQuery.
    void syncNativePackagePageSearch();
    void syncOrganizationTaskPageSearch();
    // The top search box lists rows from every installed package: pages load
    // on first use, and redraw the dropdown when their records arrive.
    void prepareNativePackageSearch();
    void nativePackageSearchIndexChanged();
    void syncNativePackageBadge(const QString &id);
    QString nativePackageAgentName() const;
    forkmesh::market::NativePackageRepository nativePackageRepository(int index) const;
    QList<forkmesh::market::NativePackageRepository> nativePackageRepositories() const;
    forkmesh::market::NativePackageRepository currentNativePackageRepository() const;
    void openNativePackageRepositoryFile(
        const forkmesh::market::NativePackageRepository &repository,
        const QString &path, int line);
    void notifyNativePackagesRepositoryChanged();
    void askNativePackageAgent(
        const QString &prompt,
        forkmesh::market::NativePackageAgentReplyHandler done);
    QJsonArray nativePackageSpecialAgents();
    void runNativePackageSpecialAgentAction(
        const QString &agentId, const QString &action, const QString &text,
        forkmesh::market::NativePackageSpecialAgentActionHandler done);
    // Packages the desktop hosts itself (Notes, Tasks, Calendar, Timers, and
    // the launcher-only ones) answer the same box; see MainWindowSearch.cpp.
    struct BuiltinPackageSearchHit {
        QString packageId;
        QString group; // dropdown heading
        QString icon;
        QString label;
        QString hitId; // handed back to activateBuiltinPackageSearchHit
    };
    bool builtinPackageSearchable(const QString &packageId) const;
    QList<BuiltinPackageSearchHit> builtinPackageSearchHits(const QString &query,
                                                           int limitPerPackage) const;
    void activateBuiltinPackageSearchHit(const QString &packageId,
                                         const QString &hitId);
    void prepareBuiltinPackageSearch();
    void openNoteFromSearch(const QString &id, const QString &storage);
    bool m_builtinPackageSearchPrepared = false;
    QString teammatesDirPath(int repoIndex = -1) const;
    // Which repository the Team section is pointed at. Teammates live in
    // <repo>/.forkmesh/teammates, so every teammate path resolves through
    // here; it falls back to the open repository before one is chosen.
    int activeTeammateRepoIndex() const;
    void setActiveTeammateRepoIndex(int repoIndex);
    // Repositories a teammate can be assigned to: local working trees only,
    // because a read-only mirror has nowhere to store the config.
    QList<int> teammateRepoChoices() const;
    QString teammateRepoLabel(int repoIndex) const;
    void refreshTeammateRepoChooser();
    void showTeammateDetail(const QString &id, bool focusSkills = false);
    void openTeammateHome(const QString &id);
    void showTeammateHome(const QString &id);
    QWidget *buildTeammateHomePage();
    bool teammateHomeVisible() const;
    void scheduleTeammateHomeRefresh();
    void refreshTeammateHome(bool force = false);
    void leaveTeammateHome();
    QList<const AgentSession *> teammateHomeRuns(const QString &id) const;
    QString teammateHomeRunOutput(const AgentSession &session);
    void fillTeammateHomeHeader(const forkmesh::teammates::Config &teammate,
                                const AgentSession *session,
                                const QString &status, bool inFlight);
    void fillTeammateHomeChat(const forkmesh::teammates::Config &teammate,
                              const QList<const AgentSession *> &runs,
                              const forkmesh::teammates::Workspace &workspace);
    void fillTeammateHomeWidgets(const forkmesh::teammates::Config &teammate,
                                 const QList<const AgentSession *> &runs,
                                 const forkmesh::teammates::Workspace &workspace);
    void fillTeammateHomeInfo(const forkmesh::teammates::Config &teammate,
                              const QList<const AgentSession *> &runs);
    QWidget *buildTeammateHomeWidgetCard(
        const forkmesh::teammates::Config &teammate,
        const forkmesh::teammates::Widget &widget);
    void sendTeammateHomeMessage(const QString &text,
                                 const QString &assignment = QString());
    void addTeammateHomeWidget(const QString &kind);
    void editTeammateHomeWidget(const QString &widgetId);
    void askTeammateToBuildWidget(const QString &widgetId = QString());
    void updateTeammateWorkspace(
        const QString &id,
        const std::function<void(forkmesh::teammates::Workspace &)> &change,
        bool rebuild = true);
    void saveTeammateHomeNotes();
    QString teammateRepoDigest(int repoIndex) const;
    QString teammateWorkspaceRoot(int repoIndex = -1) const;
    forkmesh::teammates::Workspace loadTeammateWorkspace(const QString &id,
                                                         int repoIndex = -1) const;
    bool saveTeammateWorkspace(const QString &id,
                               const forkmesh::teammates::Workspace &workspace,
                               int repoIndex = -1) const;
    QPushButton *makeTeammatePlayButton(const forkmesh::teammates::Config &config,
                                        bool inFlight, int sessionId, int side,
                                        QWidget *parent);
    void appendTeammateProfileHero(QVBoxLayout *contentLayout,
                                   const forkmesh::teammates::Config &teammate,
                                   const AgentSession *session,
                                   const QString &state, bool inFlight,
                                   bool oneTime, const QString &handle);
    void appendTeammateProfileWork(QVBoxLayout *contentLayout,
                                   const forkmesh::teammates::Config &teammate,
                                   const AgentSession *session);
    void appendTeammateProfileSettings(
        QVBoxLayout *contentLayout, const forkmesh::teammates::Config &teammate,
        const QString &state, bool oneTime, const QString &handle);
    // Dashboard is the default Team landing page. It keeps the compact roster
    // controls and folds each teammate's daily briefing into the same card.
    void showTeammateDashboard();
    void showTeammateGrid();
    void openTeammateEditor(const QString &id = QString(),
                            bool focusSkills = false);
    // Appends the editable teammate form to an existing column, so the profile
    // and its editor read as one panel instead of two competing views.
    void appendTeammateEditorForm(QVBoxLayout *column, QScrollArea *scroll,
                                  forkmesh::teammates::Config teammate,
                                  bool editing, const QString &directory,
                                  QPushButton *saveButton, bool focusSkills);
    // Section builders and Save wiring for appendTeammateEditorForm.
    struct TeammateEditorFields;
    int teammateRepoIndexForDirectory(const QString &path) const;
    void appendTeammateIdentityFields(QVBoxLayout *form,
                                      const forkmesh::teammates::Config &teammate,
                                      const QString &directory,
                                      TeammateEditorFields &fields);
    void appendTeammateAgentFields(QVBoxLayout *form);
    void appendTeammateSkillFields(QVBoxLayout *form, QWidget *formHost,
                                   const forkmesh::teammates::Config &teammate,
                                   TeammateEditorFields &fields);
    void appendTeammateScheduleFields(QVBoxLayout *form, QWidget *formHost,
                                      const forkmesh::teammates::Config &teammate,
                                      TeammateEditorFields &fields);
    void appendTeammateCollaborationFields(
        QVBoxLayout *form, QWidget *formHost,
        const forkmesh::teammates::Config &teammate,
        TeammateEditorFields &fields);
    void connectTeammateEditorSave(QPushButton *saveButton, QWidget *formHost,
                                   forkmesh::teammates::Config teammate,
                                   bool editing, const QString &directory,
                                   const forkmesh::teammates::LoadResult &loaded,
                                   const TeammateEditorFields &fields);
    void deleteTeammate(const QString &id);
    void stopTeammate(const QString &id);
    // Card-level prompt box. A teammate that is already working takes the text
    // as a steering message on its live session (runTeammate would refuse to
    // start a second one); an idle teammate starts a run with the text as its
    // concrete assignment. Takes the prompt by value: both paths pump the event
    // loop, which deletes the card that owns the editor it came from.
    void sendTeammatePrompt(const QString &id, QString prompt);
    int runTeammate(
        const QString &id,
        const QString &trigger = QStringLiteral("On demand"),
        const QString &taskType = QStringLiteral("on-demand"),
        int repoIndex = -1, const QString &assignment = QString(),
        // A teammate run updates its Team card in place. The user can opt in
        // to the agent detail through that card's explicit Agent link.
        bool switchToTab = false);
    // A teammate owns one durable agent entry. Once its initial entry exists,
    // every later assignment is another turn in that session rather than a
    // new row in Agents.
    bool continueTeammateSession(
        forkmesh::teammates::Config teammate, const QString &trigger,
        const QString &prompt, int repoIndex = -1);
    AgentSession *ownTeammateSession(
        const forkmesh::teammates::Config &teammate);
    int ownTeammateSessionId(const forkmesh::teammates::Config &teammate,
                             int repoIndex) const;
    int startTeammateSession(forkmesh::teammates::Config teammate,
                             const QString &trigger, const QString &taskType,
                             int repoIndex, const QString &assignment,
                             bool switchToTab);
    bool teammateRunHasSubstantiveChanges(const AgentSession &session);
    QList<int> runTeammateTeam(
        const QList<forkmesh::teammates::Config> &teammates,
        const QString &trigger,
        const QHash<QString, QString> &assignments = {}, int repoIndex = -1);
    forkmesh::teammates::AgentChoice nextTeammateBackupModel(int sessionId) const;
    bool queueTeammateBackupModel(int sessionId, const QString &failure);
    // Record on the session itself which teammate started it and snapshot the
    // installed skills, so both the row badge and historical usage chart
    // survive later teammate edits.
    void markAgentSessionTeammate(int sessionId,
                                  const forkmesh::teammates::Config &teammate);
    void markAgentSessionTeam(
        int sessionId, const QList<forkmesh::teammates::Config> &teammates);
    bool fileTeammateSessionInFolder(const AgentSession &session);
    QString buildTeammateSharedPrompt(
        const QList<forkmesh::teammates::Config> &teammates,
        const QString &trigger, int repoIndex, bool mayEdit) const;
    // Persists a 1–5 operator assessment (or 0 to clear it) for a finished
    // teammate task. The task grade feeds Team performance, not agent status.
    bool setTeammateTaskGrade(int sessionId, int grade);
    void ensureTeammateScheduleTimer();
    void checkTeammateSchedules();
    void triggerTeammatesForEvent(const QString &eventName,
                                  const QString &detail,
                                  int repoIndex = -1,
                                  const QString &chatConversation = QString(),
                                  const QString &chatThreadRootId = QString());
    int deliverTeammateMessage(const QString &id, const QString &trigger,
                               const QString &message, int repoIndex = -1,
                               int relayDepth = 0,
                               const QString &chatConversation = QString(),
                               const QString &chatThreadRootId = QString());
    void bindTeammateChatThread(int sessionId,
                                const QString &chatConversation,
                                const QString &chatThreadRootId);
    void relayTeammateAssistantText(int sessionId, const QString &text);
    void handleTeammateFinished(int sessionId);
    void continueTeammateLoopAfterCycle(int sessionId, int repoIndex,
                                        const QString &teammateId);
    void enableTeammateSchedule(const QString &id);
    void disableAllTeammateSchedules();
    QString buildTeammatePrompt(
        const forkmesh::teammates::Config &teammate,
        const QString &trigger,
        const QString &taskType = QStringLiteral("on-demand"),
        const QString &assignment = QString(), int repoIndex = -1) const;
    QPixmap teammateAvatar(const forkmesh::teammates::Config &teammate,
                           int side) const;
    QString teammateRuntimePrefix(const QString &id, int repoIndex = -1) const;
    forkmesh::teammates::Config teammateWithRuntime(
        forkmesh::teammates::Config teammate, int repoIndex = -1) const;
    forkmesh::teammates::Config teammateWithRuntime(
        forkmesh::teammates::Config teammate,
        const QString &runtimePrefix) const;
    void saveTeammateRuntime(
        const forkmesh::teammates::Config &teammate,
        int repoIndex = -1) const;
    void saveTeammateRuntime(
        const forkmesh::teammates::Config &teammate,
        const QString &runtimePrefix) const;
    void clearTeammateRuntime(const QString &id, int repoIndex = -1) const;
    void recordTeammateActivity(const QString &id, const QString &activity,
                                const QString &trigger = QString(),
                                bool markRun = false);
    QString teammateLatestOutput(const AgentSession *session) const;
    QHash<QString, MemberInfo> teammateChatMentionProfiles() const;
    QWidget *buildRepoSettingsTab();
    void refreshRepoSettings(); // sync the Settings controls to the open repo

    // --- Coves: encrypted, password-shared file vaults inside a repo ----------
    // A cove is.forkmesh/coves/<slug>.cove (AES-256-GCM, see CoveStore). The team
    // shares one password out-of-band; entered in repo settings, it unlocks
    // matching legacy coves. Account-scoped coves live in the repository explorer.
    // Opening a cove logs it locally and (if the creator
    // asked) sends a best-effort live alert back to the creator's node.
    CoveStore coveStoreForRepo(int repoIndex) const;
    QString coveRepoSettingsPrefix(int repoIndex) const; // QSettings key prefix
    QString rememberedCovePassword(int repoIndex) const; // repo password
    bool coveAutoOpenEnabled(int repoIndex) const;       // repo toggle
    bool tryUnlockCove(Cove &cove, int repoIndex, QString *passwordOut) const;
    void logCoveAccessLocal(const Cove &cove);           // "log yourself" — local
    QStringList coveAccessLogLocal(const QString &coveId) const;
    void announceCoveOpened(const Cove &cove);           // best-effort live alert
    void openCove(const QString &relPath);               // unlock + show a viewer
    void openCoveViewer(int repoIndex, Cove cove, const QString &password);
    void promptCreateCove(int repoIndex);
    void rebuildRepoCovesList();                         // repo Settings list
    QWidget *buildCoveSection();                         // repo Settings "Coves"
    void applyCovePasswordFromSettings(int repoIndex, const QString &password,
                                       bool remember);
    static QByteArray coveOpenCanonical(const QString &coveId, const QString &creatorKey,
                                        const QString &openerKey, qint64 ts);
    void updateRepoSource();
    void applyRepositorySource(int index, const QString &source);
    void reloadRepoRemotesTable();
    void promptAddRepoRemote();
    void promptEditRepoRemote();
    void deleteSelectedRepoRemote();
    void promptSetRepoForkLocation();
    void setRepoGitIdentityFromForkMesh();
    void setRepoActionsEnabled(bool on);
    void setRepoActionsAutoApprove(bool on);
    void setRepoRequirePeerApproval(bool on);
    // Enable or disable secret-scanning push protection for the open repo.
    void setRepoSecretScanningEnabled(bool on);
    void setWorkflowDisabled(const QString &path, bool disabled);
    void setRepoWorkflowDisabled(int index, const QString &path, bool disabled);
    bool isWorkflowDisabled(const QString &path) const;
    void loadMirrorNodesPanel();
    int mirrorNodesRepoIndex() const;
    void refreshMirrorNodesRepoChooser();
    void showMirrorNodesOnNetwork(int repoIndex);
    void applyNodeRoleFilter();
    forkmesh::noderole::Role nodeRoleFor(const forkmesh::noderole::Facts &facts) const;
    static QTableWidgetItem *makeNodeRoleCell(forkmesh::noderole::Role role);
    QSet<QString> savedHostNodeNames() const;
    void fetchMirrorSectionCatalog(const QString &owner, const QString &repo);
    void draftMirrorNodeDiagnosticsPrompt(QTableWidgetItem *healthItem);
    QString mirrorSelfSnapshotKey(const RepositoryRecord &repo) const;
    void refreshMirrorSelfSnapshot(const RepositoryRecord &repo, const QString &key);
    QHash<QString, MirrorSelfSnapshot> m_mirrorSelfSnapshots; // keyed by mirrorPath
    QSet<QString> m_mirrorSelfSnapshotsInFlight;
    struct MirrorPanelRefs {
        QString stamp;
        QString routingHash;
        QString allRefsHash;
        QHash<QString, QString> heads;
    };
    struct MirrorPanelGitRequest {
        QString gitDir;
        QString stamp;
        bool wantRefs = false;
        QList<QPair<QString, QString>> relations;
        QList<QStringList> counts;
        QString identityDir;
        QStringList identities;
    };
    struct MirrorPanelGitFacts {
        QString gitDir;
        QString stamp;
        bool refsRead = false;
        MirrorPanelRefs refs;
        QHash<QString, int> relations;
        QHash<QString, int> counts;
        QHash<QString, CommitIdentity> identities;
    };
    static MirrorPanelGitFacts
    readMirrorPanelGitFacts(const MirrorPanelGitRequest &request);
    void requestMirrorPanelGitFacts(const MirrorPanelGitRequest &request);
    void applyMirrorPanelGitFacts(const MirrorPanelGitFacts &facts);
    QHash<QString, MirrorPanelRefs> m_mirrorPanelRefs;
    QHash<QString, int> m_mirrorRelationCache;
    QHash<QString, QString> m_mirrorRelationUnknownAt;
    QHash<QString, int> m_mirrorPanelCounts;
    QString m_mirrorPanelGitDir;
    bool m_mirrorPanelGitInFlight = false;
    bool m_mirrorPanelGitPending = false;
    void animateMirrorNodeLights();
    void updateMirrorNodeLightTimer();
    void requestMirrorNodesRefresh();
    void fetchCatalogMirrors(const QString &owner, const QString &repo,
                             const QString &source);
    void fetchSshMirrorMetadata(const QString &owner, const QString &repo,
                                const QString &source);
    void fetchReleaseDownloadCounts(const QString &owner, const QString &repo,
                                    const QString &source);
    void replicateReleaseArtifacts(int index);
    void downloadNextReleaseBlob(int index, const QString &mirrorPath,
                                 QMap<QString, QString> pending);
    void pushReleaseToMirrors(const QString &tag);
    void pushReleaseToMirrors(int index, const QString &tag);
    void deleteTag(const QString &tag);
    bool repoHasWorkingTree() const;
    void loadFileSearchIndex();
    // Opens the whole-codebase treemap / 3D view for the browsed ref
    // (MainWindowCodeMap.cpp).
    void openRepoCodeMap();
    QWidget *createGlobalSearchBox();          // build the box + results popup
    void rebuildGlobalSearchResults();         // (re)populate the dropdown (debounced)
    void startGlobalSearchCommitSplice(int searchGeneration, const QString &needle,
                                       const QString &suffix, const QString &ref);
    void startGlobalSearchCatalogSplice(int searchGeneration, const QString &needle,
                                        const QString &query);
    void finishGlobalSearchPopup(int total);
    void positionGlobalSearchPopup();          // anchor the dropdown under the box
    void moveGlobalSearchSelection(int delta); // keyboard up/down through results
    void activateGlobalSearchItem(QListWidgetItem *item); // navigate to a result
    void hideGlobalSearchPopup();
    void syncGitCommitFilter();
    void syncAgentPageSearch();
    // Empty every box that can be narrowing the list in front of you: the top
    // search box plus the per-page mirrors it feeds. Clearing only the top box
    // would leave a list the user filtered from its own box just as empty,
    // which is the dead end "(clear search)" exists to escape.
    void clearSearchFilters();
    QLabel *createSearchEmptyNotice();         // notice wired to clearSearchFilters
    // Swap a list for its "no results for <term>" notice, or back.
    void updateSearchEmptyNotice(QLabel *notice, QWidget *list,
                                 const QString &query, bool listIsEmpty);
    QWidget *createNavHistoryButtons();        // build the Back / Forward pair
    void scheduleNavRecord();                  // queue a debounced location capture
    void recordNavLocation();                  // snapshot the current place onto the trail
    void restoreNavEntry(int index);           // navigate to a recorded place
    struct NavPlace;
    void applyNavDetailTab(const NavPlace &place); // re-select a recorded repo tab
    void captureNavSubPlace(NavPlace &place) const;
    void applyNavSubPlace(const NavPlace &place);
    QString openRepoFilePath() const; // file the code editor is showing
    bool gitWorkspaceIsVisible() const;
    QString navPlaceLabel(const NavPlace &place) const; // human-readable trail destination
    void navigateBack();
    void navigateForward();
    void showNavHistoryMenu(bool forward);     // press-and-hold: the trail itself
    QMenu *buildNavHistoryMenu(bool forward);  // its rows, or null when empty
    QList<int> navHistoryMenuIndices(bool forward) const; // nearest place first
    void updateNavHistoryButtons();            // enable/disable per trail position
    QWidget *buildSearchResultsSection();
    void openSearchResultsPage(const QString &query);
    void openSavedSearchesPage(); // the search page with no query: just saved searches
    // Saved searches: kept from the results page, listed beside it, and
    // re-run in the background to ping about new occurrences.
    struct SearchPageInputs;
    std::shared_ptr<const SearchPageInputs> searchPageInputs(int repoIndex,
                                                             const QString &query) const;
    static QVector<GlobalSearchHit> searchPageKindHits(const SearchPageInputs &in,
                                                       const QString &kind);
    // The Git categories the page streams from live processes, collected
    // synchronously for a background re-run: code, commit messages, history.
    static QVector<GlobalSearchHit> scanSavedSearchGitHits(const QString &gitDir,
                                                           const QString &ref,
                                                           const QString &query,
                                                           int capPerKind);
    SavedSearchStore &savedSearchStore();
    QString searchScopeLabel(int repoIndex) const;
    void finishSearchPageRun();
    void showSaveSearchPrompt();
    void saveCurrentSearch();
    void dismissSaveSearchPrompt();
    void updateSaveSearchButton();
    void refreshSavedSearchList();
    void openSavedSearch(const QString &id);
    void setSavedSearchAlerts(const QString &id, bool alerts);
    void setSavedSearchSecurityPertinent(const QString &id, bool pertinent);
    void editSavedSearchExclusions(const QString &id);
    int scrubTargetRepoIndex(const QString &scope) const;
    QString scrubTermShortcutPath(int repoIndex) const;
    void scrubSearchTerm(QString term, QString scope);
    void editSearchHiddenTerms();
    void setSearchHiddenTerms(const QStringList &terms);
    void setSearchHidesVariables(bool hide);
    void refreshAllSavedSearches();
    void updateSavedSearchRefreshButton();
    void setSavedSearchExclusions(const QString &id, const QStringList &excluded);
    QWidget *buildSecurityAlarmBanner();
    void evaluateSecurityAlarms();
    void acknowledgeSecurityAlarms();
    void openSecurityAlarmResults();
    void playSecurityAlarmTone();
    void removeSavedSearch(const QString &id);
    void scheduleSavedSearchAlerts();
    void checkSavedSearchAlerts();
    void runNextSavedSearchAlertCheck();
    void startSearchProcess(const QString &dir, const QStringList &gitArgs,
                            QTreeWidgetItem *bucket, int cat);
    void onSearchResultActivated(QTreeWidgetItem *item, int column);
    bool isSearchResultLogRow(QTreeWidgetItem *item) const;
    void deleteSearchResultLogRow(QTreeWidgetItem *item);
    // Delete all on a Logs or Workflow runs group header: removes every row the
    // group lists, after a confirmation.
    void deleteSearchResultBucket(QTreeWidgetItem *bucket);
    void updateSearchBucketDeleteButton(QTreeWidgetItem *bucket);
    void stopSearch();
    void animateSearchBucketSpinners();
    void updateSearchStatus();
    void editRepoAbout();
    bool saveRepoAboutMetadata(const QString &about, const QString &websiteInput,
                               QString *error = nullptr);
    bool applyRepoAboutMetadataAt(int index, const QString &about,
                                  const QString &websiteInput,
                                  QString *error = nullptr);
    QUrl repoAboutApiUrl(const QString &owner, const QString &name) const;
    void saveRepoFediverseSettings(const QString &owner, const QString &name,
                                   bool federate, bool broadcastEvents,
                                   bool acceptComments);
    void loadCommits();
    struct CommitsLoad {
        bool ok = false;
        QByteArray log;
        QSet<QString> unpushed;
        QString mirrorTip;
    };
    static CommitsLoad readCommits(const QString &dir, const QString &localPath,
                                   const QString &mirrorPath,
                                   const QString &mirrorBranch,
                                   const QString &ref, int rowLimit,
                                   const QString &relayHost);
    void applyCommits(int loadGen, const CommitsLoad &load, int rowLimit);
    bool m_commitsLoading = false;
    int m_commitsLoadMoreScroll = 0;
    void fillCommitStats(int loadGen);
    void loadMoreCommits();
    void toggleCommitFilesRows(int row);
    void collapseAllCommitFileRows();
    // statusCode < 0 recomputes from m_actionRuns; the bulk loaders pass the
    // code they already looked up through a CommitStatusIndex.
    void updateCommitRowHover(int row, int statusCode = -1);
    void updateCommitsUnsyncedFilesPanel();
    void fetchCurrentRepo();
    void pullCurrentRepo();
    QWidget *buildSourceControlPanel();
    void refreshSourceControl();             // re-scan `git status` into the tree
    void refreshSourceControl(bool force);   // force refresh path bypassing cache short-circuit
    void showSourceControlLoading(const QString &branch);
    void showRangeFilesInSourceControl(const QStringList &paths,
                                       const QStringList &statuses);
    void clearRangeFilesInSourceControl();
    bool sourceControlShowsRange() const;
    QString sourceControlGitDir() const;
    QString repoChangeBadgeGitDir() const;
    void scrollBranchDiffToFile(const QString &path, bool moveFocus = true);
    bool stepScmTreeFile(int delta);
    void setScmCurrentItemSilently(QTreeWidgetItem *item);
    void updateBranchDiffActiveOutline();
    void updateScmDiffActiveOutline();
    void scheduleRepoChangeBadgeRefresh();
    void refreshRepoChangeBadge();
    void scmStagePath(const QString &path);
    void scmUnstagePath(const QString &path);
    void scmDiscardPath(const QString &path, bool untracked);
    void showScmFileMenu(const QPoint &pos);
    void confirmScmRevertPathToBase(const QString &path);
    bool scmRevertPathToBase(const QString &path, QString *summary);
    struct ScmRevertRequest {
        QString path;
        QString branch;
        QString dir;
        QString base;
    };
    struct ScmRevertResult {
        bool ok = false;
        QString summary;
        bool closeTabs = false;
        bool refresh = false;
        bool committed = false;
        QString logLine;
    };
    bool scmRevertRequestFor(const QString &path, ScmRevertRequest *request,
                             QString *error) const;
    static ScmRevertResult runScmRevertPathToBase(const ScmRevertRequest &request);
    void applyScmRevertPathToBase(const ScmRevertRequest &request,
                                  const ScmRevertResult &result);
    void scmIgnorePath(const QString &path);
    void scmDeletePath(const QString &path);
    bool deleteWorkingTreeFile(const QString &dir, const QString &path);
    bool handleDiffFileActionLink(QTextEdit *view, const QUrl &url);
    void showDiffFileMenu(QTextEdit *view, const forkmesh::ui::DiffFileMenuTarget &target);
    void deleteDiffFile(QTextEdit *view, const QString &dir, const QString &path);
    bool diffBranchDeleteRequestFor(const forkmesh::ui::DiffFileMenuTarget &target,
                                    ScmRevertRequest *request);
    void confirmDeleteDiffFileFromBranch(QTextEdit *view,
                                         const ScmRevertRequest &request);
    static ScmRevertResult runScmDeletePathFromBranch(const ScmRevertRequest &request);
    void refreshDiffAfterFileDelete(QTextEdit *view);
    void scmStageAll();
    void scmUnstageAll();
    void scmDiscardAll();
    void scmCommit();
    void scmCommitAndPush();
    void scmStageAllCommitAndPush();
    void performScmCommit(std::function<void(bool)> done = {});
    void commitWorkingTree(const QString &message,
                           std::function<void(bool)> done = {});
    struct ScmMutationResult {
        bool ok = false;
        QString error;
        QByteArray out;
        bool flag = false;
    };
    struct ScmMutationJob {
        QString key;
        std::function<ScmMutationResult()> work;
        std::function<void(const ScmMutationResult &)> apply;
    };
    bool queueScmMutation(const QString &key,
                          std::function<ScmMutationResult()> work,
                          std::function<void(const ScmMutationResult &)> apply);
    void pumpScmMutationQueue();
    static ScmMutationResult scmMutate(const QString &dir,
                                       const QStringList &args);
    int scmRepoIndexFor(int hint, const QString &localPath) const;
    QList<ScmMutationJob> m_scmMutationQueue;
    QString m_scmMutationRunningKey;
    bool m_scmMutationRunning = false;
    bool m_scmCommitInFlight = false;
    bool m_scmRevertInFlight = false;
    void showScmDiff(const QString &path, bool staged, bool untracked);
    void showScmDiffAll(bool staged);
    void renderScmCombinedDiff();
    // Fold/reopen one file of the working-tree diff in place; false when the
    // document cannot carry the swap and the caller has to re-render it whole.
    bool swapScmDiffFileInPlace(const QString &path);
    QString scmDiffSourceKey(const QSet<QString> &viewed,
                             const QSet<QString> &expanded) const;
    void setupScmDiffPane();
    void scrollScmDiffToFile(const QString &path, bool staged);
    bool pinScmDiffSectionToTop(int sectionIndex);
    void updateScmDiffScrollState();
    void layoutScmStickyHeader();
    void updateScmNextFileBar();
    void layoutScmNextFileBar();
    void computeScmFileTops();
    void applyScmAutoMarkViewedOnScroll();
    void selectScmFileInTree(const QString &path, bool staged);
    QTreeWidgetItem *scmFindItem(const QString &path, bool staged) const;
    void updateScmViewedCount();
    void onScmDiffAnchorClicked(const QUrl &url);
    static QString scmViewedContext();
    void scmSelectAdjacentChange(int delta);
    bool scmScrollToAdjacentHunk(int delta, bool fromEnd = false);
    void autoFillScmMessage();
    void generateScmMessage();
    void finishScmGeneration();
    void startScmAiRequest(const QString &diff, int modelIndex, bool isCommit,
                           quint64 requestGeneration,
                           quint64 patchGeneration, const QString &dir,
                           int duration);
    void setScmGenerateEnabled(bool enabled);
    void refreshCommitsUnlessCurrent();
    void refreshCommitMarkersIfStale();
    struct CommitMarkerProbe {
        QString localTip;
        QString mirrorTip;
    };
    static CommitMarkerProbe readCommitTips(const QString &dir,
                                            const QString &ref,
                                            const QString &mirrorPath,
                                            const QString &mirrorBranch);
    QString openRepoMirrorPath() const;
    // mtime+size fingerprint of the working copy's and the served mirror's ref
    // storage. Empty when no repo is open. Same trick as refreshMirrorAdverts()'s
    // input signature: asking git whether git has moved costs exactly what the
    // probe is trying to avoid.
    QString commitMarkerProbeSignature() const;
    QString m_commitMarkerProbeSig;
    qint64 m_commitMarkerProbedAtMs = 0;
    bool m_commitMarkerProbeInFlight = false;
    void applyCommitIssueClosures();
    struct CommitClosureScan {
        bool ok = false;
        bool unchanged = false;
        bool namesIssue = false;
        QString key;
        QByteArray log;
    };
    static CommitClosureScan readCommitClosureScan(const QString &dir,
                                                   const QString &ref,
                                                   const QString &issuesSignature,
                                                   const QString &previousKey);
    void closeIssuesNamedByCommits(IssueStore store, const QByteArray &log);
    bool m_commitClosureScanRunning = false;
    bool m_commitClosureScanPending = false;
    int commitStatusCode(const QString &sha) const;
    // One pass over m_actionRuns for the open repository, so a table of
    // thousands of commits (the search path loads 5000) does a hash lookup per
    // row instead of re-walking the run history for every row — twice.
    struct CommitStatusIndex {
        QHash<QString, int> flagsByCommit;        // 1 success, 2 failed, 4 running
        QList<QPair<QString, int>> abbreviated;   // commits shorter than a full sha
        bool built = false;
    };
    CommitStatusIndex buildCommitStatusIndex() const;
    int commitStatusCode(const QString &sha, const CommitStatusIndex &index) const;
    QString commitStatusGlyph(const QString &sha) const;
    void refreshCommitBarStatusGlyph();
    void refreshCommitTableStatusGlyphs();
    void updateActionsTabIndicator();
    qint64 estimatedRunDurationMs(const ActionRun &run) const;
    void updateAgentsTabIndicator();
    void refreshCommitStatusGlyphs();
    void refreshRepoSecurity();
    // Shared by refreshRepoSecurity() and runRepoDependencyScan(): gathers the
    // repo state RepoSecurity::scan() needs so it can be handed to a worker
    // thread without touching MainWindow/GUI state from there.
    RepoSecurityInput buildRepoSecurityInput(const RepositoryRecord &selected,
                                              const RepositoryRecord &writable) const;
    // Renders a computed snapshot into the Security tab's widgets (also caches
    // it in m_lastRepoSecuritySnapshot so a "Run scan" click can re-render the
    // busy state instantly, before the rescan itself has produced anything new).
    void applyRepoSecuritySnapshot(const RepoSecuritySnapshot &snapshot,
                                   const QString &localBase);
    void runRepoDependencyScan(const QString &manifestPath = QString());
    void refreshRepoQuality();
    // Open a repo file in the editor and highlight/centre the given 1-based
    // line (used by the Security and Quality findings tables).
    void openRepoFileAtLine(const QString &path, int line);
    void loadRepoInsights();
    struct InsightsGitRead {
        QString dir;
        QString ref;
        int windowDays = 0;
        bool treeOk = false;
        QByteArray treeOut;
        QString treeErr;
        bool logOk = false;
        QByteArray logOut;
        QString logErr;
    };
    static InsightsGitRead readRepoInsightsGit(InsightsGitRead read);
    void renderRepoInsights(const InsightsGitRead &git);
    quint64 m_insightsLoadGen = 0;
    bool m_uncommitRunning = false;
    QSet<QString> m_commitFilesLoading;
    quint64 m_commitsUnsyncedFilesGen = 0;
    bool m_commitRewriteRunning = false;
    static std::pair<bool, QString> removeCommitFromHistory(const QString &dir,
                                                            const QString &hash,
                                                            const QString &branch);
    static std::pair<bool, QString> revertCommitInHistory(const QString &dir,
                                                          const QString &hash);
    bool m_createBranchRunning = false;
    bool m_pullCurrentRepoRunning = false;
    bool m_commitPatchRunning = false;
    void saveCommitPatch(const QString &hash, const forkmesh::ui::GitCaptureResult &read);
    bool m_insightsLoading = false;
    bool m_insightsReloadPending = false;
    void showInsightsContributorMenu(const QPoint &pos);
    void fitInsightsContributorsHeight();
    void reassignContributorIdentity(const QString &oldName);
    struct ContributorIdentityRead {
        bool dirty = false;
        QStringList identities;
        QString primaryEmail;
    };
    static ContributorIdentityRead readContributorIdentities(const QString &dir,
                                                             const QString &oldName);
    void continueReassignContributorIdentity(const QString &dir, const QString &oldName,
                                             const QStringList &identities,
                                             const QString &primaryEmail);
    static std::pair<bool, QString> rewriteContributorIdentity(const QString &dir,
                                                               const QString &script);
    bool m_reassignIdentityRunning = false;
    void openCommitsForContributor(const QString &author);
    void setRepoBranch(const QString &branch, bool loadContent = true);
    QString repoHeadBranch() const;          // the checked-out branch (HEAD)
    void updateCommitsBranchButtonLabel();   // branch + current worktree identity
    void refreshCommitsBranchButton();       // commits-page branch indicator/menu
    void createAndCheckoutBranch();          // "Create new branch…"
    QString currentRef() const;
    QString repoGitDir() const;
    QString iconsDir() const;
    QIcon iconForFile(const QString &fileName) const;
    mutable QHash<QString, QIcon> m_fileTypeIcons; // icon name -> loaded icon
    QIcon iconForDir(bool opened) const;
    void startRefreshSpin();
    void stopRefreshSpin();
    void startCommitDiffSpin();
    void stopCommitDiffSpin();
    void spinRefreshButton(QPushButton *button);
    void addRefreshSpin(QPushButton *button);
    void startButtonSpin(QPushButton *button);
    void stopButtonSpin(QPushButton *button);
    enum class RestartKind { Rebuild, Update };
    void startRestartSpin(QPushButton *button,
                          RestartKind kind = RestartKind::Rebuild);
    void stopRestartSpin();
    void setRestartSpinProgress(int percent);
    void setRestartSpinHourglass(bool hourglass);
    QString restartTimingHistoryKey(RestartKind kind) const;
    QList<forkmesh::restarttiming::RestartRun>
    restartRunHistory(RestartKind kind) const;
    void appendRestartRun(RestartKind kind,
                          const forkmesh::restarttiming::RestartRun &run);
    void recordRestartDuration();
    void finishPendingRestartTiming();
    void addRestartCountdown(QPushButton *button);
    void updateRestartCountdown();
    QString restartCountdownText() const;
    void setRestartPhase(const QString &phase, int buildPass = 0);
    void noteRestartStepOutput(const QString &chunk);
    void startNodeSwitchSpin();
    void stopNodeSwitchSpin();
    void startRepoSwitchSpin();
    void stopRepoSwitchSpin();
    void nodeSwitchStep(const QString &what);
    void finishLoadStepTiming();
    void showLoadStatus(const QString &what);

    const RepositoryRecord &writableRecordFor(const RepositoryRecord &repo) const;

    QString repoAgentGitDir(const RepositoryRecord &repo) const;

    int issuesRepoIndex() const;                 // selected repo, or -1
    IssueStore issueStoreForCurrentRepo() const; // build a store for that repo
    void refreshIssuesRepoCombo();
    void reloadIssues();        // load issues + label/milestone filters from the store
    void reloadIssuesInBackground();
    void applyLoadedIssues(const QString &signature, QList<Issue> issues,
                           QList<IssueLabel> labels,
                           QList<IssueMilestone> milestones);
    void appendCreatedIssue(const IssueStore &store, const Issue &issue);
    void removeIssueFromCurrentList(int number);
    void refreshIssueList();    // apply filters into the list widget
    void resetIssueFilters();   // clear status/label/milestone/search filters
    void selectIssueListTab(int id);
    void refreshIssueMilestones();
    void refreshIssueLabels();
    QWidget *buildIssueBoard();
    void refreshIssueBoard();    // rebuild the columns/cards from m_currentIssues
    QStringList boardColumns() const;             // configured column names
    void setBoardColumns(const QStringList &cols); // persist + rebuild
    void editBoardColumns();                      // prompt to edit the column list
    QString issueBoardColumn(const Issue &issue) const;
    void moveIssueToColumn(int number, const QString &column);
    void editIssueLabelDefinition(int row);
    void showIssue(int number); // render the selected issue's thread
    void renderIssueThread(const Issue &issue);
    struct IssueThreadCardContext; // defined in MainWindowIssues.cpp
    void renderNoIssueSelected();
    void renderIssueParentRefs(const Issue &issue);
    void renderIssueSidebarValues(const Issue &issue);
    void addIssueThreadCard(const Issue &issue, const IssueEvent &ev, bool isOpen,
                            const IssueThreadCardContext &context);
    void renderIssueCardBody(QWidget *bodyContainer, QVBoxLayout *bodyLayout,
                             int num, const QString &eid,
                             const QString &eventBody,
                             const QStringList &eventAttachments, bool isOpen,
                             bool writable, const QString &imageBase,
                             bool haveLocalFiles);
    QWidget *buildIssueChecklist(QWidget *bodyContainer, int num,
                                 const QString &eid, const QString &eventBody,
                                 const QStringList &eventAttachments,
                                 const QString &displayBody,
                                 const QList<IssueChecklistTask> &tasks,
                                 bool writable);
    QMenu *buildIssueCardMenu(QWidget *card, int num, const QString &eid,
                              const QString &eventBody, bool writable,
                              bool isOpen);
    void addIssueCardSubIssuesButton(QWidget *headerBox, QHBoxLayout *headerRow,
                                     int num, const QString &eventBody,
                                     bool writable, bool isOpen);
    void addIssueCardReactions(QWidget *card, QVBoxLayout *cardLayout,
                               const Issue &issue, int num, const QString &eid,
                               bool writable, const QString &myKey,
                               const QString &myName);
    void addIssueActivityRow(const IssueEvent &ev, const QString &html);
    void addIssueSubIssuesSection(const Issue &issue, bool writable);
    void addIssueRemoteThread(const Issue &issue);
    void showIssueBurnupChart();
    void showIssueComposePage(QWidget *page);
    void removeIssueComposePage();
    void setIssueInlineNotice(const QString &message, bool error = false,
                              const QString &kind = QString());
    void promptEditIssueTitle();
    void saveIssueTitleEdit();
    void cancelIssueTitleEdit();
    void promptNewIssue();
    void composeNewIssue(const QString &prefillTitle, const QString &prefillBody);
    void promptIssueFromChatMessage(const QString &text, const QString &senderName,
                                    qint64 timestampMs);
    bool createIssueFromChatCommand(const QString &argument);
    void quickAddIssue();
    void publishQuickAddToActivityPub(const QString &prompt);
    QString quickAddAgentBaseBranchForGitView() const;
    // Images and video both; the queue and everything downstream of it takes
    // either (see promptAttachmentLine).
    void attachQuickAddMedia();
    // Plays a queued video attachment, in-app where this build has a decoder.
    void showAttachedVideoDetail(const QString &path);
    bool tryPasteImageIntoQuickAdd();
    void queueQuickAddImage(const QString &path);
    void removeQuickAddImage(const QString &path);
    void clearQuickAddImages();
    void updateQuickAddImageButton();
    void rebuildQuickAddAttachChips();
    void showQuickAddImageDetail(const QString &path);
    void captureScreenRegion();
    void startVoiceCapture();
    void stopVoiceCapture();
    void startVoiceCaptureFor(QPlainTextEdit *target, QPushButton *button);
    void initializeWorldSpeechBridge();
    void createWorldSpeechPairing();
    void revokeWorldSpeechPairing();
    void cancelWorldVoiceCapture();
    void openVoiceSettings();
    // Settings > Pings, with `kind`'s row highlighted when one is named. The
    // Pings page's settings column and header button land here.
    void openPingSettings(const QString &kind = QString());
    QPushButton *makeVoiceButton(MarkdownEditor *composer);
    void startVoiceTranscription(bool finalPass);
    void applyVoiceTranscript(const QString &text, bool finalPass);
    void updateVoiceInputButton();
    void updateVoiceLevelMeter();
    void stopVoiceLevelMeter();
    void showVoiceTranscribeSpinner();
    void hideVoiceTranscribeSpinner();
    void toggleMicTest();
    void updateMicTestMeter();
    void stopMicTest();
    void installWhisperCpp();
    void installParakeet();
    void installVoiceEngine();
    void refreshWhisperStatus();
    void recordQuickAddHistory(const QString &text);
    bool navigateQuickAddHistory(int direction);
    // Write the composer's pending draft to disk now (no-op when unchanged).
    void saveQuickAddDraft();
    // Footer slash-actions menu: the "/" button left of the Agent
    // checkbox opens a filterable popup mirroring the Claude Code extension's
    // actions menu (Context/Model sections plus the CLI's own slash commands,
    // pulled live from `claude` via a control-protocol initialize probe).
    void openQuickAddSlashActions();
    void populateSlashActionsList();
    void moveSlashActionsSelection(int delta);
    void activateSlashActionRow(QWidget *row);
    void refreshClaudeSlashCommands();
    QStringList agentEffortLevels() const;
    void refreshQuickAddSpeedSelector();
    void refreshAgentModelSelectors();
    void refreshQuickAddAgentModelSelector();
    void refreshIssueAgentModelSelector();
    void fillAgentModelSelector(QComboBox *selector,
                                const QString &selectedProvider,
                                const QString &selectedModel,
                                bool includeManual,
                                const QString &fallbackTip);
    QList<ComposerModelChoice> composerModelCatalog() const;
    // The agent+model pairs with the most runs behind them on this node, most
    // used first, capped at `limit` — the short list a one-click action offers
    // instead of the whole catalog.
    QList<ComposerModelChoice> mostUsedAgentModels(int limit) const;
    void refreshComposerModelVisibilityList();
    void refreshCloudflareAiModels();
    void refreshClaudeEffortLevels();
    void mentionProjectFileInQuickAdd();
    void showTreasuryDonateDialog();
    void showTreasuryDonateDialogForPool(const QJsonObject &pool);
    void copyIssueToClipboard();
    void copyIssueThreadToClipboard();
    void askAiForCurrentIssue();
    // "@opus why does this hang?" / "@fable fix this" in an issue thread: the
    // host answers in the thread, or starts a coding agent when the mention
    // asks for work. Scanned after every inbox drain, sync and local comment.
    void answerIssueModelMentions(const RepositoryRecord &repo,
                                  const QList<Issue> &issues,
                                  const QList<PullRequest> &pulls);
    void runIssueModelMention(const RepositoryRecord &repo, const Issue &issue,
                              const forkmesh::issuemention::Mention &mention,
                              const QString &mentionEventId,
                              const QList<PullRequest> &pulls);
    // One @model mention's outbound request, kept whole so an answer that came
    // back empty can be asked again with a bigger output budget instead of
    // rebuilding the prompt from the thread.
    struct IssueMentionAsk {
        RepositoryRecord repo;
        int number = 0;
        QString botAuthor;
        QString label;       // "Sonnet 5"
        QString model;       // what is actually asked ("claude-sonnet-5")
        bool claude = false;
        QString oauthToken;  // Claude Code subscription token, when there is one
        QString apiKey;
        QString instructions;
        QString prompt;
        QString note;        // footer aside ("asked as gpt-4.1-nano")
    };
    void sendIssueMentionAsk(const IssueMentionAsk &ask, int maxTokens,
                             bool retry);
    void postIssueModelComment(const RepositoryRecord &repo, int number,
                               const QString &botAuthor, const QString &body);
    // The bot's "seen it" on the comment that mentioned it, written before the
    // model is asked so the thread shows the mention landed.
    void postIssueModelReaction(const RepositoryRecord &repo, int number,
                                const QString &eventId, const QString &botAuthor,
                                const QString &reaction);
    void toggleIssueReaction(int number, const QString &eventId,
                             const QString &reaction);
    void showIssueReactionPicker(int number, const QString &eventId,
                                 QWidget *anchor);
    // The issue thread as plain text for a model prompt: title, state and every
    // surviving open/comment body with its author, bounded to maxChars.
    QString issueThreadContext(const Issue &issue, int maxChars) const;
    // The repository around that thread: commit history, the commits and
    // branches naming this issue, its pull requests, and the current contents
    // of any file threadText points at. This is what lets a mentioned model
    // answer "was this fixed?" instead of asking for a paste.
    QString issueRepoContext(const RepositoryRecord &repo, const Issue &issue,
                             const QList<PullRequest> &pulls,
                             const QString &threadText, int maxChars) const;
    void scanCurrentRepoModelMentions();
    void showIssueAiTyping();
    void hideIssueAiTyping();
    int availableCredits() const;     // 1 voting credit per hour online
    void voteOnCurrentIssue();
    void voteOnIssue(int number);
    void submitIssueVoteToInbox(int number);
    void updateVoteUi();
    void addIssueComment();
    void closeIssueWithComment();
    void attachIssueImage();
    void queueIssueAttachment(const QString &path); // dedupe + reference + count
    void toggleIssueStatus();
    int nextVisibleIssueAfter(int number) const;
    // Right-click on an issue row: select it, then offer the detail pane's
    // actions (status, fields, vote, copy, delete) without leaving the list.
    void showIssueListContextMenu(const QPoint &viewportPos);
    QMenu *buildIssueListContextMenu(int number, QWidget *parent);
    void deleteCurrentIssue();
    void moveCurrentIssueToTask();
    void deleteCurrentIssueWithHistory(int number);
    void editIssueLabels();
    void editIssueMilestone();
    // Planned start/end dates: opens the inline two-QDateEdit
    // editor in the sidebar's "Dates" row; save writes a signed "dates" event.
    void editIssueDates();
    void saveIssueDatesInline();
    void editIssuePriority();
    void nudgeIssuePriority(int direction);
    void nudgeIssueProgress(int deltaPercent);
    void editIssueProgress();
    bool handleIssueProgressDrag(QMouseEvent *ev);
    void applyIssueProgressDragAt(const QPoint &pos);
    void commitIssueProgressDrag();
    int m_issueProgressDragRow = -1;
    // Label pills and the milestone chip in a summary row are click targets:
    // they rewrite the top search into the matching label:/milestone: query.
    bool handleIssueRowChipClick(QMouseEvent *ev);
    void applyIssueSearchQuery(const QString &query);
    void showIssueListForQuery(const QString &query);
    // The detail pane's tab bar only earns its height when the "Changes in Git"
    // shortcut is visible; otherwise the title starts at the top of the pane.
    void syncIssueDetailTabBar();
    void editIssueBounty();
    void bountyAllOpenIssues(double amountUsd);
    void reprioritizeBacklog();
    void prioritizeIssuesFromReadme();
    void analyzeIssueCompleteness();
    QString currentRepoReadme() const;
    int estimateIssueProgress(const Issue &issue,
                              const QSet<int> &mergedIssues) const;
    static double openAiEstimateUsd(const Issue &issue);
    void renderIssueAssigneesValue(QStringList assignees);
    void editIssueAssignees();
    void pickIssueAssignees();
    void refreshIssueAssigneeSuggestions();
    void renderIssueReporterValue(QString reporter);
    void editIssueReporter();
    void pickIssueReporter();
    void saveIssueLabelsInline();
    void saveIssueMilestoneInline();
    void saveIssuePriorityInline();
    void saveIssueAssigneesInline();
    void saveIssueReporterInline();
    void cancelIssueSidebarEditors();
    void updateIssueActionState();

    QWidget *buildProjectsSection();
    ProjectStore projectStoreForCurrentRepo() const; // build a store for that repo
    void reloadProjects();      // load projects (+ issues, for progress) from the store
    void refreshProjectList();  // apply the status filter into the table
    void refreshProjectGantt(); // rebuild the Gantt rows from the loaded data
    void showProject(int number); // render the selected project's detail pane
    void promptNewProject();
    void editProjectLinkedIssues(); // multi-select dialog over the repo's issues
    void setProjectInlineNotice(const QString &message, bool error = false);
    int projectProgressPercent(const Project &project) const;

    QUrl issuesApiUrl(const RepositoryRecord &repo) const;
    // Owner-encrypted agent relay: clear session content remains
    // on the desktop; the relay stores only recipient-sealed snapshots/prompts.
    QUrl agentsApiUrl(const RepositoryRecord &repo) const;
    void pushAgentSessionsSnapshot();
    void pushAgentSessionsForRepo(RepositoryRecord repo, QList<AgentSession> sessions);
    void pushAgentSessionsForRepoWithTranscripts(
        RepositoryRecord repo, QList<AgentSession> sessions,
        QHash<int, QString> transcripts);
    void ensureAgentE2EEControlPlane(
        RepositoryRecord repo, std::function<void(bool ok)> onDone);
    // Fetch the account's other enrolled owner-device public bundles (e.g.
    // the mobile node) from /api/security/owner-keys so session snapshots can
    // be sealed to every owner device, not only this desktop's vault key.
    void refreshCompanionOwnerBundles(const QString &ownerKeyId);
    QList<QJsonObject> companionOwnerBundles(const QString &ownerKeyId,
                                             qint64 nowMs);
    bool loadOwnerEncryptionIdentity(MirrorCrypto::Identity *identity,
                                     QString *error = nullptr) const;
    void scheduleAgentSessionsPush();
    // Coalesces meta.json rewrites driven by streaming token-usage updates
    // (several a second per running Codex turn) into one save per session
    // per ~second; flushed synchronously on close.
    void scheduleAgentSessionSave(int sessionId);
    void flushPendingAgentSessionSaves();
    void applyAgentMetaHtml(const QString &html);
    void drainAgentPrompts();
    void drainAgentPromptsFor(RepositoryRecord repo);
    // Organization-member Claude/Codex jobs are a distinct, encrypted-at-rest
    // channel; owner-only E2EE prompts above remain unchanged.
    void drainOrgAgentJobsFor(RepositoryRecord repo);
    void applyOrgAgentJobsPayload(const RepositoryRecord &repo,
                                  const QJsonArray &jobs);
    void startApprovedOrgAgentJob(const RepositoryRecord &repo,
                                  const QJsonObject &job,
                                  const QString &reason);
    void fetchOrgAgentJobAttachments(const RepositoryRecord &repo,
                                     QJsonObject job, QStringList saved = {});
    int startCloudHandoffJob(const RepositoryRecord &repo,
                             const QJsonObject &job);
    bool fetchServedBranch(const RepositoryRecord &local, const QString &branch,
                           bool update);
    void persistOrgAgentBinding(int localAgentId, const QJsonObject &job);
    void clearOrgAgentBinding(int localAgentId);
    void reportOrgAgentJob(const RepositoryRecord &repo,
                           const QJsonObject &job,
                           const QString &securityVerdict,
                           const QString &status,
                           int localAgentId = 0,
                           const QString &reason = QString(),
                           const QString &result = QString());
    void reportCompletedOrgAgentJobs();
    // Whether this machine could run a provider's agent at all — the binary
    // and the CLI login — in the words the relay is told. Asked before a
    // queued website run is claimed for this desktop.
    QJsonObject agentProviderAvailability(const QString &provider) const;
    void deliverQueuedAgentPrompt(int sessionId, const QString &text);
    void startWebNewAgentForRepo(const RepositoryRecord &repo, const QString &task,
                                 const QString &providerOverride = QString(),
                                 const QStringList &images = QStringList());
    QStringList saveWebAgentImages(const RepositoryRecord &repo,
                                   const QStringList &images);
    QUrl sharesApiUrl(const RepositoryRecord &repo) const;
    void shareRepoRequest(const RepositoryRecord &repo, const QString &grantee,
                          const QString &action);
    void requestPrivateRecipientBundles(
        const RepositoryRecord &repo, bool updateCollaboratorList,
        std::function<void(bool ok, QList<QJsonObject> bundles,
                           QStringList grantees, QString error)> onDone);
    void addRepoCollaborator(const QString &nameRaw);
    void removeRepoCollaborator(const QString &nameRaw);
    void refreshRepoCollaborators();
    void showBountyQrDialog(const RepositoryRecord &repo, int number,
                            const QString &uri, const QString &address,
                            double amountUsd, const QString &amountSol,
                            const QString &kind = QString(),
                            const QString &payee = QString());
    void showBountyWalletDialog();
    void submitIssueCommentToInbox(const QString &body,
                                   const QStringList &attachmentSrcPaths = {},
                                   const QStringList &attachmentPlaceholders = {});
    // Mirror node path: file a signed "assignees" event to the source of truth's
    // inbox so the looper's claim on an issue reaches the owner and syncs back to
    // every mirror.
    void submitIssueAssigneesToInbox(int number, const QStringList &assignees);
    // Mirror node path: send a signed new-issue ("open") event to the source of
    // truth's inbox. Returns false only when there is no repo to target (checked
    // synchronously); the actual POST result is only known once it completes, so
    // callers get it via onDone rather than assuming success immediately (a
    // failed submission must not be reported as "sent").
    bool submitNewIssueToInbox(const QString &title, const QString &body,
                               const QStringList &labels, const QString &milestone,
                               int priority, const QStringList &assignees,
                               const QStringList &attachmentSrcPaths = {},
                               const QStringList &attachmentPlaceholders = {},
                               std::function<void(bool ok, const QString &error)> onDone = {});
    void syncIssuesInbox();
    // Fetch the signed contents of one collaboration inbox and present a
    // reviewable list with per-submission and sync-all actions. Counts stay on
    // the three toolbar buttons via the public, content-free /pending endpoint.
    void showPendingInbox(const RepositoryRecord &repo, const QString &kind);
    QString relayCooldownMessage(const QString &host) const;
    void showPendingInboxDialog(const RepositoryRecord &repo,
                                const QString &kind,
                                const QJsonArray &pending);
    void applyPendingInboxSelection(const RepositoryRecord &repo,
                                    const QString &kind,
                                    const QJsonArray &pending);
    void reviewPendingPull(RepositoryRecord repo, QJsonObject item);
    void showPullReviewDialog(RepositoryRecord repo, PullRequest pr,
                              PullReviewCheckout checkout);
    QString pullReviewCheckoutPath(const RepositoryRecord &repo,
                                   const QString &slug) const;
    void refreshPendingInboxBadges();
    void setPendingPullsTabBadge(int pending);
    void setPendingInboxCount(const RepositoryRecord &repo,
                              const QString &kind, int count);
	void drainIssuesInboxFor(RepositoryRecord repo, bool interactive,
							 bool force = false, int page = 0,
                             std::function<void()> onFinished = {});
	// Opening the Issues page: pull everything the source of truth is missing —
	// the relay inbox *and* whatever a mirror already materialized on its own
	// copy — instead of waiting for the periodic sweep.
	void syncIssuesFromMeshOnOpen(int index);
	// Backoff bookkeeping shared by the owner-side issue/pull/discussion
	// inbox drains. Managed mirror nodes run their native Go intake instead.
	void noteInboxDrainFailure(const QString &backoffKey, int status);
    void noteInboxDrainSuccess(const QString &backoffKey);
	void drainPullsInboxFor(RepositoryRecord repo, bool interactive,
                           bool force = false);
    // Owner-side pull ledger convergence: adopt mirror-drained pulls the
    // ledger never saw, then flip open pulls whose branch already landed.
    // Throttled per repo unless `force`; reloads the pulls UI on change.
    void reconcilePullLedgerFor(RepositoryRecord repo, bool force = false);
    void scanRepoMentionsFor(const RepositoryRecord &repo);
    void applyRepoMentions(const RepositoryRecord &repo,
                           const QList<Issue> &allIssues,
                           const QList<PullRequest> &allPulls);
    struct AutomaticIssueSyncRequest {
        RepositoryRecord repo;
        QString writablePath;
        QString account;
        QString relayOrigin;
        QJsonArray pending;
        bool applyPayload = false;
    };
    void pollOwnedInboxes();
    void queueAutomaticIssueSync(
        RepositoryRecord repo, QJsonArray pending = {},
        bool applyPayload = false, QString expectedAccount = {},
        QString expectedRelayOrigin = {});
    void launchAutomaticIssueSync(AutomaticIssueSyncRequest request);
    // Bounded relay sync: scheduleRelaySync() coalesces explicit refresh
	// requests into one signed GET /api/sync that returns every owned repo's
	// pending control-plane changes. Node events and reconnect catch-up drive it;
	// no per-repository socket or fallback polling timer is opened.
    void scheduleRelaySync();
    void performRelaySync();
    void applyIssuesInboxPayload(const RepositoryRecord &repo,
								 const QJsonArray &pending, bool interactive,
                                 std::function<void(bool)> onApplied = {});
    void applyPullsInboxPayload(const RepositoryRecord &repo,
								const QJsonArray &pending, bool interactive);
    void applyDiscussionsInboxPayload(const RepositoryRecord &repo,
                                      const QJsonArray &pending,
									  bool interactive);
    void applyAgentPromptsPayload(const RepositoryRecord &repo,
                                  const QJsonArray &prompts);
    void acknowledgeAgentPrompts(const RepositoryRecord &repo,
                                 const QList<qint64> &queueIds);
    QUrlQuery signedInboxQuery(const QString &owner) const;
    void backUpIdentityKey();
    void refreshIdentityBackupNag();
    // Explain provider-owned, per-device Claude login and the owner-sealed
    // shared-workspace boundary. No credential export/import controls exist.
    void showClaudeCodeDeviceSetup();
    void chooseAvatar();
    void setSettingsAvatar(const QByteArray &pngData);
    QByteArray effectiveAvatar();
    QByteArray effectiveUserAvatar();
    // Persist the local avatar to the account record (POST /api/accounts/profile)
    // so the web dashboard shows the same picture. No-op without an authenticated
    // session token or a local avatar to upload.
    void pushAccountAvatar();
    // Edit the signed-in account's public profile fields (bio, location, time
    // zone, Mastodon handle, privacy switches): the record the web dashboard's
    // Public profile form writes through POST /api/accounts/profile. Non-modal;
    // saves asynchronously with the session token, or the current password when
    // this desktop never minted one.
    void showEditAccountProfileDialog();
    // That dialog's Save button: POST the fields, update the cached account
    // record, then close the dialog or show the error through setHint.
    void submitAccountProfileSave(
        QDialog *dialog, const QString &account, QPushButton *saveButton,
        QLabel *passwordLabel, QLineEdit *passwordEdit,
        const std::function<void(const QString &, bool)> &setHint);
    // The editable profile fields as a POST /api/accounts/profile body, read
    // from the open editor. Empty when no editor is open.
    QJsonObject accountProfileEditBody() const;
    void adoptWebAccountAvatar(const QByteArray &png);
    void updateAvatarButton();
    void updateUserAvatarButton();
    void updateAdminCrownBadge();
    void refreshIssueComposerAvatar();
    void refreshPullComposerIdentity();
    QWidget *makeComposerIdentity(QLabel **outAvatar = nullptr,
                                  const QString &verb = QString());
    void logout();
    // Tell the relay to drop the browser-visible account session minted by the
    // last email/password login, so logging out here also logs out on the
    // website. No-op when this desktop only ever authenticated with its key.
    void revokeAccountSession();
    void revokeAccountSessionToken(const QString &token,
                                   const QString &serverUrl = QString());
    // Immediately after a logout, ask for the account password and sign back in
    // against the website. Password login (unlike the silent key-based auth) both
    // mints a website session and re-registers this desktop's public key with the
    // relay, so the site knows this device. Returns true once signed back in.
    bool promptRelogin(const QString &previousAccount);
    void loginToUserAccount();
    void uninstallForkMesh();
    void rebuildAndRelaunch();
    void showSection(int index);
    void updateHomeStats();
    QString selfNodeStats() const; // inline stats line for your own node
    void attachBackend(ChatBackend *backend);
    void leaveSession(const QString &reason = QString());

    void onMessage(const ChatMessage &message);
    bool canReactInConversation(const QString &conversation) const;
    void toggleChatReaction(const QString &conversation,
                            const QString &messageId, const QString &reaction);
    bool hasOwnChatReaction(const QString &conversation,
                            const QString &messageId,
                            const QString &reaction) const;
    void onReaction(const QString &conversation, const QString &messageId,
                    const QString &emoji, const QString &reactorName, bool added);
    void onDiscordReactionsLoaded(
        const QString &conversation, const QString &messageId,
        const QMap<QString, QStringList> &reactions);
    void onMessageEdited(const QString &conversation, const QString &messageId,
                         const QString &newText);
    void onMessageDeleted(const QString &conversation, const QString &messageId);
    void applyMessageRecipients(const QString &conversation,
                                const QString &messageId, int recipients,
                                const QStringList &names = {});
    forkmesh::recipients::ReceiptMark chatReceiptMark(
        const ChatMessage &message) const;
    void applyMessagesDelivered(const QString &conversation,
                                const QStringList &messageIds);
    void applyConversationRead(const QString &conversation);
    void refreshReceiptRow(const ChatMessage &message);
    void applyOfficeReaction(const QString &conversation,
                             const QString &messageId, const QString &emoji,
                             const QString &reactorName, bool added);
    void applyOfficeEdit(const QString &conversation, const QString &messageId,
                         const QString &senderId, const QString &newText);
    void applyOfficeDelete(const QString &conversation,
                           const QString &messageId, const QString &senderId);
    bool officeMutationTargetReady(const QString &conversation,
                                   const QString &messageId,
                                   const QJsonObject &mutation);
    void replayPendingOfficeMutations(const QString &messageId);
    void promptEditMessage(const QString &messageId, const QString &currentText);
    void confirmDeleteMessage(const QString &messageId);
    void openChatThread(const QString &rootMessageId);
    void closeChatThread();
    void rebuildChatThreadPane();
    void sendChatThreadReply();
    // How many replies a message has, and who wrote them: the summary row
    // under it shows their faces and names the newest replier.
    ThreadReplySummary chatThreadSummary(const QString &conversation,
                                         const QString &rootMessageId) const;
    // Admin moderation: delete any message (not just your own). The delete is
    // signed by this node's identity and broadcast; peers verify the signature
    // and the signer's admin status before applying.
    void confirmAdminDeleteMessage(const QString &messageId);
    void onAdminDeleteRequested(const QString &conversation, const QString &messageId,
                                const QString &adminId, const QString &adminName,
                                qint64 ts, const QString &sig);
    QString adminDeleteCanonical(const QString &conversation, const QString &messageId,
                                 const QString &adminPubkey, qint64 ts) const;
    void onAvatar(const QString &peerId, const QByteArray &pngData);
    void onTypingChanged(const QString &conversation, const QString &peerId,
                         const QString &peerName, bool active);
    void logSystem(const QString &text,
                   const char *sourceFile = FORKMESH_LOG_SOURCE_FILE,
                   int sourceLine = FORKMESH_LOG_SOURCE_LINE);
    void logSystemFrom(const QString &text, const QString &sourcePath,
                       int sourceLine,
                       forkmesh::LogSeverity severity =
                           forkmesh::LogSeverity::Automatic,
                       const QString &badgeOverride = QString());
    void observeErrorLogForTeammates(const QString &storedLine,
                                     qint64 observedAtMs);
    void resumeErrorLogMonitoringTeammates();
    void processErrorLogMonitoringTeammates(const QString &newErrorLine,
                                            qint64 observedAtMs,
                                            bool recoverReservedLaunches);
    bool reconcileLogMonitorRun(
        forkmesh::teammates::LogMonitorRuntime &runtime,
        bool recoverReservedLaunches, const QString &queueName);
    bool settleWorkedCrashRepair(
        forkmesh::teammates::LogMonitorRuntime &runtime);
    // Wake the monitors when a failed run's retry hold runs out, so a quiet
    // log does not leave the held batch waiting for the next ERROR line.
    void armLogMonitorRetryWake(
        const forkmesh::teammates::LogMonitorRuntime &runtime, qint64 nowMs);
    void startErrorLogMonitoringTeammate(
        int repoIndex, const forkmesh::teammates::Config &teammate,
        const forkmesh::teammates::LogMonitor &monitor,
        const QString &stateKey);
    QString errorLogRepairAssignment(
        const forkmesh::teammates::LogMonitorRuntime &runtime) const;
    // A crash never reaches the running app: the dying process writes it to
    // the log, so the next launch reads it back and hands it straight to the
    // ERROR teammate instead of waiting for the batch to fill.
    QList<forkmesh::crashlog::Record> unrepairedCrashRecords();
    QSet<QString> dispatchCrashRepairs(
        int repoIndex, const forkmesh::teammates::LoadResult &loaded,
        bool automaticRunsEnabled, bool recoverReservedLaunches,
        const QList<forkmesh::crashlog::Record> &newCrashes);
    void startCrashRepairTeammate(
        int repoIndex, const forkmesh::teammates::Config &teammate,
        const QString &taskType, const QString &stateKey);
    QString crashRepairAssignment(
        const forkmesh::teammates::LogMonitorRuntime &runtime) const;
    void completeSuccessfulLogMonitorRun(int sessionId);
    void noteErrorLogRepairHold(int busySessionId, int heldCount);
    // Live lines reach the Activity view in batches: logSystemFrom() queues
    // them and one ~50 ms coalescing timer inserts the whole batch in a single
    // document edit, so a burst of hundreds of lines costs a few relayouts and
    // repaints instead of one each.
    void queueNetworkLogLiveLine(const QString &storedLine);
    void armNetworkLogLiveFlush();
    void flushNetworkLogLiveLines();
    void appendNetworkLogLines(const QStringList &storedLines);
    QStringList m_logLivePending;
    QStringList m_logPopoutLivePending;
    QTimer *m_logLiveFlushTimer = nullptr;
    // Follow the tail: while on, every batch (and any relayout that grows the
    // document) keeps the view on the newest line. Scrolling up turns it off,
    // scrolling back to the bottom or the Live button turns it back on.
    void setNetworkLogFollowing(bool follow);
    void updateNetworkLogFollowButton();
    bool m_logFollowTail = true;
    bool m_logScrollingToTail = false;
    int m_logUnseenLines = 0; // lines appended while not following
    QPushButton *m_logFollowButton = nullptr;
    QString m_lastLogRenderDate; // date of the last line rendered (for dividers)
    bool m_networkLogViewStale = false;
    // The Logs page's chart summary / frequency ranking fell behind while the
    // page was off-screen; showSection(4) brings them current in one pass.
    bool m_logTimelineViewStale = false;
    QString logBadgeFor(const QString &storedLine) const; // category of a line
    QString logAccentFor(const QString &storedLine) const; // badge colour of a line
    // Classifying a stored line lowercases it and runs it past ~50 substring
    // rules. The category chips, the timeline chart and every filtered rebuild
    // each walk the whole retained log, so the same lines were re-classified
    // thousands of times per click — a 652 ms stall was sampled inside
    // QString::toLower() under refreshLogTimelineChart(). Lines are immutable
    // once stored, so the verdict is cached by line.
    struct LogLineStyle {
        QString message; // body with the "[source:line]" suffix stripped
        QString badge;
        QString accent;
    };
    const LogLineStyle &logStyleFor(const QString &storedLine) const;
    void forgetLogStyle(const QString &storedLine) const; // a line left the buffer
    void clearLogStyleCache() const;
    mutable QHash<QString, LogLineStyle> m_logStyleCache;
    bool logLineMatchesTextFilter(const QString &storedLine) const;
    bool logLineMatchesFilters(const QString &storedLine) const; // categories + text
    bool hasLogCategoryFilter() const { return !m_logFilters.isEmpty(); }
    QString logCategoryFilterSummary() const;
    // Per-category logging switch. A muted category is dropped in
    // logSystemFrom() before anything records it.
    void loadMutedLogCategories() const;
    bool logCategoryEnabled(const QString &badge) const
    {
        loadMutedLogCategories();
        return !m_mutedLogCategories.contains(badge);
    }
    void setLogCategoryEnabled(const QString &badge, bool enabled);
    bool loggingEnabled() const;
    void setLoggingEnabled(bool enabled);
    void updateLoggingOffButton();
    // Erases every retained line of one category: the buffer, the live file,
    // the rotated segments and the chart's digest. Returns the lines removed.
    int deleteLogCategory(const QString &badge);
    void confirmDeleteLogCategory(const QString &badge);
    void showLogCategoryMenu(const QString &badge, const QPoint &globalPos);
    void showLogTypesDialog();
    QVector<LogTimelineSeries> logTimelineSeries() const;
    void rebuildLogFilterButtons(); // (re)build the category chip row
    int logFilterChipCount(const QString &category) const;
    void updateLogFilterChipCounts(); // refresh the counts without rebuilding
    void updateCloudLogFilterChip();
    void refreshLogTimelineChart();
    void refreshLogFrequencyChart();
    void updateLogAggregationAvailability();
    void refreshLogUsageView();
    void fetchAdminUsageReport();
    void refreshLogD1UsageView();
    void ensureD1QuerySourceIndex();
    forkmesh::D1QuerySite d1LiveQuerySite(const QString &sql,
                                          const QString &originFile);
    QString reportedD1DatabaseId() const;
    void fetchAdminD1UsageReport();
    void setAdminD1ReadGuard(const QString &mode, int threshold);
    void refreshLogWorkerFootprintView();
    void fetchWorkerFootprintReport();
    void refreshLogCronView();
    void fetchAdminCronReport();
    void applyLogTimelineCategoryFilter();
    void appendLogTimelineEntry(const QString &storedLine);
    void setLogTimelinePresetMinutes(int minutes);
    void setLogTimelinePresetHours(int hours);
    void updateLogTimelineSummary();
    void chooseCustomLogTimelineRange();
    void showFormattedLogJson();
    void rebuildNetworkLogView();   // re-render the log honoring m_logFilters
    void renderNetworkLogSlice(quint64 generation);
    QString popoutLogLineHtml(const QString &storedLine, QString &runningDate);
    void renderNetworkLogPopoutSlice(quint64 generation);
    void appendNetworkLogPopoutLine(const QString &storedLine);
    void appendNetworkLogPopoutLines(const QStringList &storedLines);
    void updateNetworkLogPopoutStatus();
    QString networkLogPath() const; // on-disk path for the live log file
    void loadNetworkLog();          // restore log history at startup
    // Rewrites the live file from the in-memory buffer. This *shortens* the
    // record, so it is only for the Clear button - shutdown must not call it or
    // every session would throw away whatever no longer fits in the buffer.
    void saveNetworkLog();
    // Retention. The live file is append-only until it passes
    // netlog::kRotateLines, then it is rotated into network_log.d/ and segments
    // older than netlog::kRetentionDays are dropped.
    void rotateNetworkLogIfNeeded();
    void pruneNetworkLogHistory();
    void persistNetworkLogOnShutdown();
    // The 30-day chart cannot hold 30 days of lines, so every logged line is
    // also counted into a coarse on-disk digest that outlives the buffer.
    void loadNetworkLogDigest();
    void saveNetworkLogDigest();
    void flushNetworkLogDigestWrites();
    void noteNetworkLogDigestLine(qint64 timestampMs, const QString &badge);
    QVector<LogTimelineBucket> networkLogHistoryBuckets(qint64 fromMs,
                                                        qint64 toMs) const;
    // Every logged line used to be opened, appended and closed on the GUI
    // thread, and a busy stretch (an action running, a mirror publishing) logs
    // hundreds of them. Queue them instead and let one worker drain the queue in
    // submission order; a full rewrite supersedes whatever is still queued,
    // because it writes the same buffer those lines are already in.
    void queueNetworkLogAppend(const QString &line);
    // Delete one line from the buffer, the log files, the filter chip counts
    // and the chart digest. False when the buffer no longer holds it.
    bool deleteNetworkLogLine(const QString &line);
    // The same for many lines, with one filter/chart refresh and one queued
    // file pass. Returns how many the buffer still held and so removed.
    int deleteNetworkLogLines(const QStringList &lines);
    void flushNetworkLogWrites();   // block until the queue is on disk (shutdown)
    int m_logRenderFrom = 0;
    bool m_logViewMutating = false;
    bool m_logFilterEmptyNotice = false;
    quint64 m_logRenderGeneration = 0;
    QStringList m_logRenderLines;
    int m_logRenderCursor = 0;
    QString m_logRenderDate;
    void loadOlderNetworkLogSegment();
    void onNetworkLogScrolled(int value);
    void flashMessage(const QString &text, bool error = false,
                      const QString &clickHref = QString(),
                      const QString &kind = QString(), int actionRunId = -1,
                      const char *sourceFile = FORKMESH_LOG_SOURCE_FILE,
                      int sourceLine = FORKMESH_LOG_SOURCE_LINE);
    void flashPingMessage(const QString &text, bool error = false,
                          const QString &clickHref = QString(),
                          const QString &kind = QString(), int actionRunId = -1,
                          const char *sourceFile = FORKMESH_LOG_SOURCE_FILE,
                          int sourceLine = FORKMESH_LOG_SOURCE_LINE);
    void fileMessage(const QString &text, bool error = false,
                     const QString &kind = QString(), int actionRunId = -1,
                     const char *sourceFile = FORKMESH_LOG_SOURCE_FILE,
                     int sourceLine = FORKMESH_LOG_SOURCE_LINE);
    void showLocalNotice(QLabel *label, const QString &text, bool error = false,
                         const QString &kind = QString(),
                         const char *sourceFile = FORKMESH_LOG_SOURCE_FILE,
                         int sourceLine = FORKMESH_LOG_SOURCE_LINE);
    void showLocalNotice(forkmesh::ui::kit::InlineNotice *notice,
                         const QString &text, bool error = false,
                         const QString &kind = QString(),
                         const char *sourceFile = FORKMESH_LOG_SOURCE_FILE,
                         int sourceLine = FORKMESH_LOG_SOURCE_LINE);
    enum class NoticeRoute { Page, Local, Ping };
    void flashMessageImpl(const QString &text, bool error,
                          const QString &clickHref, const QString &kind,
                          int actionRunId, NoticeRoute route,
                          const char *sourceFile, int sourceLine);
    // Ping the mesh about the failures this app used to only ever show to itself
    // Every warning/critical modal (caught by eventFilter's
    // QEvent::Show branch, so no call site has to remember) and every error notice
    // is reported to the relay, which records it in the shared operational error
    // log and pings the administrators the first time a distinct failure appears
	// — the only way an error on a desktop nobody is watching is ever seen.
	// Signed by this node's account. Off unless QSettings
    // kReportUserVisibleErrorsSetting is true (Settings > General). See
    // ClientErrorReports.h for the redaction, dedupe and deferral bounds.
    // `pingId` is the Pings row this failure was filed as, so the report's fate
    // (sent / parked / refused) lands back on that row. 0 means
    // "not filed", e.g. a report raised before the page existed.
    void reportUserVisibleError(const QString &kind, const QString &title,
                                const QString &message,
                                const QString &surface = QString(),
                                qint64 pingId = 0);
    void sendUserVisibleErrorReport(
        const forkmesh::ClientErrorReports::Report &report);
    void flushDeferredErrorReports();
    void scheduleDeferredErrorReportFlush();
    forkmesh::ClientErrorReports m_errorReports;
    QTimer *m_errorReportFlushTimer = nullptr;
    struct PageNotice {
        quint64 id = 0;
        QString page;
        QString text;
        bool error = false;
        QString href;
        QString kind;
        int actionRunId = -1;
        qint64 pingId = 0;
    };
    struct NoticeDestination {
        NotificationLink link;
        QString href;
        QString label;
        QString icon;
        QString tooltip;
        bool isValid() const { return !label.isEmpty(); }
    };
    QWidget *buildPageNoticeArea();
    QString currentNoticePage() const;
    void showPageNotice(const QString &text, bool error,
                        const QString &clickHref = QString(),
                        const QString &kind = QString(), int actionRunId = -1,
                        qint64 pingId = 0);
    void clearPageNotice(quint64 id);
    void clearPageNotices();
    void renderPageNotices(bool force = false);
    NoticeDestination noticeDestination(const QString &href, int actionRunId,
                                        qint64 pingId, bool error);
    void openNoticeDestination(const NoticeDestination &destination);
    bool openNoticeHref(const QString &href);
    void showPingKindMenu(QWidget *anchor, const QString &kind);
    void turnOffPingKind(const QString &kind);
    void appendTextToPrompt(const QString &text);
    void alertOnLoggedError(const QString &message);
    void notePlannedRestartLine(const QString &plain, qint64 nowMs);
    bool quietPlannedRestartError(const QString &plain, qint64 nowMs);
    void closeExpiredPlannedRestarts(qint64 nowMs);
    void armPlannedRestartTimer();
    bool foldRepeatedLoggedError(const forkmesh::ErrorPingGroups::Group &group,
                                 const QString &message);
    void dismissLoadStatus();
    void flyPromptSparkleToAgents();
    void floatPromptIntoTranscript(const QString &prompt,
                                   const QStringList &images);
    void floatPromptOnto(QWidget *landing, const QString &prompt,
                         const QStringList &images);
    QWidget *quickAddFollowUpLandingView() const;
    MessageRow *addMessageRow(
        const ChatMessage &message,
        const QHash<QString, MemberInfo> *mentionProfiles = nullptr);
    MessageRow *createMessageRow(const ChatMessage &message,
                                 bool threadContext = false,
                                 const QHash<QString, MemberInfo> *mentionProfiles =
                                     nullptr);
    void renderConversationRows(); // rebuilds rows in place; caller handles scrolling
    void rebuildConversationView();
    // Coalesced renderConversationRows: several rebuild requests in one burst
    // (a replayed backlog) become a single repaint on the next event loop turn.
    void scheduleConversationRebuild(bool keepAtBottom);
    void scrollToBottom();
    void setChannels(const QStringList &channels);
    void setRoster(const QList<MemberInfo> &members);
    QString welcomeChannelForIdentity() const;
    void maybeAnnounceWelcome();
    void removeChatMember(const QString &id, const QString &name);
    void switchConversation(const QString &conversation);
    void updateChatConversationHeading();
    void openDirectChat(const QString &peerId, const QString &peerName);
    void refreshChannelList();
    void refreshDmList();
    forkmesh::chatlist::Preview chatListPreview(const QString &conversation) const;
    void refreshChatListPreview(const QString &conversation);
    void refreshChatListPreviews();
    QPixmap chatListAvatar(const QString &peerId, const QString &name,
                           bool online, bool away);
    void refreshChatMembers();
    void refreshChatUserDirectory();
    void mergeChatUserDirectory(const QJsonArray &users);
    void authorizeAdminUsersDirectory(QNetworkRequest &request) const;
    void fetchNativePackageUserRecords(
        forkmesh::market::NativePackageUserRecordsHandler done);
    void showChatSenderProfile(const QString &senderId,
                               const QString &senderName);
    void showChatUserProfile(const MemberInfo &member, const QStringList &nodeLines);
    void promptAddChannel();
    void promptAddPrivateChannel();
    void showNewPrivateRoomDialog();
    void showNewMessageDialog();
    void startDirectMessageWith(const QString &account,
                                const QString &peerId = QString(),
                                const QString &peerName = QString());
    QStringList chatPickerAccounts() const;
    QStringList pickChatPeople(const QString &title, const QString &prompt,
                               bool multiple, const QStringList &exclude);
    // Sidebar channel groups (ChannelGroups.h): a presentation-layer view
    // over m_channels, persisted in one settings key.
    QString autoGroupForChannel(const QString &channel) const;
    QString channelGroupHeaderText(const QString &group, bool collapsed) const;
    QString promptChannelGroupName(const QString &title,
                                   const QString &current);
    void promptAddChannelGroup();
    void promptRenameChannelGroup(const QString &group);
    void removeChannelGroup(const QString &group);
    void moveChannelToGroup(const QString &channel, const QString &group,
                            int index = -1);
    void setChannelGroupCollapsed(const QString &group, bool collapsed);
    void persistChannelGroups();
    void selectFirstChannel();
    void persistChatSidebarWidth(int width);
    void promptInviteToPrivateChannel();
    void startDirectCallOnWeb(const QString &media);
    // Everything a channel manager controls, opened from beside the channel
    // name on the chat header.
    void openChannelSettings();
    void openChannelSettingsFor(const QString &conversation);
    void showMeshChannelSettings(const QString &conversation);
    void refreshChannelSettingsButton();
    void refreshChatComposerAccess();
    // The room's second encryption layer, opened from the padlock on the chat
    // header. The relay derives the room key itself, so only a seal keeps the
    // person who runs it out of the room.
    void promptRoomSeal();
    void refreshRoomSealButton();
    void renameChatConversation(const QString &oldConversation,
                                const QString &newConversation);
    void promptDeleteRoom(const QString &channel);
    void showEmojiPicker(QWidget *anchor);
    void insertEmojiIntoComposer(const QString &emoji);
    void insertLinkIntoComposer();
    void wrapComposerSelection(const QString &before, const QString &after);
    void sendMessageToPrompt(const QString &text);
    void restorePrivateChannels();
    void onPrivateChannelJoined(const QString &channel);
    void persistPrivateChannels();
    QString privateChannelsSettingsKey() const;
    void ensureTeamRoom();
    void sendCurrentMessage();
    void maybeAskForkbot(const QString &conversation, const QString &text,
                         const QString &threadRootId = QString());
    void onComposerEdited(const QString &text);
    void refreshMentionCandidates();
    void updateMentionPopup();
    void insertComposerCompletion(const QString &completion);
    void insertMention(const QString &name);
    void sendTypingState(bool active);
    void refreshTypingLabel();
    void attachFile();
    void showChatImageDetail(const QString &fileName, const QByteArray &data);
    void showChatVideoDetail(const QString &fileName, const QString &fileMime,
                             const QByteArray &data);
    bool openChatMediaExternally(const QString &fileName, const QString &fileMime,
                                 const QByteArray &data);
    bool trySendClipboardImage();
    bool canAttachToOfficeConversation() const;
    void saveIncomingFile(const QString &fileName, const QByteArray &data);
    QString chatHistoryKey() const;
    QString chatHistoryPathForServerUrl(const QString &serverUrl,
                                        const QString &room) const;
    QStringList chatHistoryCompatibilityPaths() const;
    QString chatHistoryPath() const;
    void saveChatHistory();
    void loadChatHistory();
    void scheduleChatSave();
    void pruneExpiredChatHistory();
    // Drop unread counters whose messages are gone -- expired by the retention
    // window, trimmed by the per-conversation save cap, or never restored --
    // so a badge always has something to open. True when it changed anything.
    bool reconcileUnreadWithHistory();
    QString avatarCacheDirectory() const;
    QString avatarCachePath(const QString &peerId) const;
    void loadCachedAvatars();
    void loadRepositories();
    bool reconcileMirrorPath(RepositoryRecord &repo);
    void adoptMaterializedMirror(RepositoryRecord &repo,
                                 const QString &repositoryPath);
    void saveRepositories() const;
    void flushRepositorySyncTimes() const;
    void refreshRepositoryList();
    QStringList mentionCandidateNames() const;
    // mentionCandidateNames() with the @model bot aliases in front, for the
    // issue composers -- the only threads where a model mention is answered.
    QStringList issueMentionCandidateNames() const;
    void promptAddRepository();
    void importRepositoriesFrom(const QString &selectedPath,
                                bool openSingleImport = false);
    void showChangedRepositoriesDiff();
    void createNewRepository();
    int provisionNewRepository(const QString &dest, const QString &name,
                               const QString &description,
                               const QString &firstPrompt, bool addReadme,
                               bool isPrivate, bool localOnly, QString *error);
    // Clone a remote repo (GitHub/GitLab/any https git URL) into a local working
    // copy, then add it like a local repo. An optional per-host access token
    // (Settings) authenticates the clone to dodge unauthenticated rate limits.
    void importRemoteRepository();
    void importSingleRemoteRepository(const QString &url);
    QStringList importAuthGitArgs(const QString &url) const;
    struct GitlabGroupImport {
        QString group;          // full group path, e.g. acme/platform
        QString parentDir;      // directory each clone lands in
        QStringList cloneUrls;  // projects still to clone
        QStringList names;      // repository name chosen for each clone URL
        int total = 0;
        int imported = 0;
        int failed = 0;
    };
    GitlabGroupImport m_gitlabGroupImport;
    QString gitlabGroupPathFor(const QUrl &url) const;
    void probeGitlabGroup(const QString &url, const QString &groupPath);
    void fetchGitlabGroupProjects(const QString &groupPath, int page);
    void cloneNextGitlabGroupProject();
    void finishGitlabGroupImport();
    void setImportStatus(const QString &text, bool error);
    void setImportControlsEnabled(bool enabled);
    void previewAdvertisedRepo(const QString &ownerName);
    void mirrorAdvertisedRepo(const QString &ownerName);
    void mirrorPreviewRepository(int index);
    void syncRepository(int index, bool quiet = false);
    bool validateMirrorSyncAssociation(int index, bool quiet);
    bool applyMirrorSyncAssociation(int index, const QString &reason);
    static QString mirrorSyncAssociationRequest(const RepositoryRecord &repo,
                                                const QString &source);
    QHash<QString, bool> m_mirrorSyncAssociationChecks;
    QSet<QString> m_mirrorSyncAssociationPassed;
    void stampRepositorySyncIdentity(const RepositoryRecord &repo);
    void queueGitRepositoryIdentityStamp(const QString &gitDir,
                                         const QString &identity);
    QHash<QString, QString> m_pendingIdentityStamps;
    quint64 m_repoRemotesLoadGen = 0;
    QString m_repoRemotesTableDir;
    bool m_repoRemoteWriteInFlight = false;
    bool repoRemoteRowsAreCurrent(const QString &gitDir) const;
    void runRepoRemoteWrite(const QString &gitDir,
                            const QList<QPair<QStringList, QString>> &steps,
                            const QString &doneLog);
    // Drain the latest coalesced commit event for one source repository. If a
    // fetch is already running, the event remains queued and is retried as soon
    // as that fetch releases the repository.
    void drainNodeCommitEvent(const QString &repositoryKey);
    void pushToSshMirrorRemotes(int index);
    void pushToSshMirrorRemotes(int index, bool wakeManagedIfUnchanged);
    void pushToSshMirrorRemotes(int index, bool wakeManagedIfUnchanged,
                                bool pruneRefs);
    // Returns the number of gateways the push was handed to. Zero has
    // several distinct causes, and a user-initiated fan-out has to name the
    // right one: "no SSH push remotes configured" is a lie when the real
    // problem is a working copy that moved out from under the record.
    // idleReason carries that sentence out for the caller to show.
    int pushToSshMirrorRemotes(int index, bool userInitiated,
                               const QString &releaseTag,
                               QString *idleReason = nullptr,
                               bool pruneRefs = false);
    void scheduleSshMirrorPushRetry(const QString &repoKey,
                                    const QStringList &skippedHosts,
                                    qint64 nowMs);
    // The unattended SSH fan-out reads its targets straight out of `git
    // remote -v`, so a mirror node that is destroyed leaves a remote nobody
    // removes. These carry the outage streak across restarts (otherwise a
    // desktop that relaunches daily never reaches the day-long retirement
    // threshold and re-announces the same dead gateway forever) and unwire
    // the fan-out when a node is deleted for good.
    void loadSshMirrorGatewayHealth();
    void saveSshMirrorGatewayHealth();
    int removeSshMirrorRemotesForNode(const QString &node);
    void syncPublicEncryptedRepository(int index, bool quiet = false);
    void syncPrivateRepository(int index, bool quiet = false,
                               bool idleChecked = false);
    // Turn network serving on or off for one repository. The control-node
    // permission table and the repo Settings checkbox both route through here
    // so the two surfaces cannot apply different transitions. Returns false
    // when nothing changed (already in that state, or not the owner).
    bool setRepositoryNetworkServing(int index, bool enabled);
    // True when a background pass may leave this private repository alone: the
    // sealed source's refs still match the fingerprint recorded at the last
    // successful seal, and that seal is recent. Keeps the periodic timer from
    // naming a private repository to the relay for no reason.
    bool privateSyncIdle(const RepositoryRecord &repo,
                         const QString &refsFingerprint) const;
    QSet<QString> m_privateSyncIdleChecks;
    void syncPrivateRepositoryWithRecipients(
        int index, bool quiet,
        const QList<QJsonObject> &recipientBundles);
    void resealPrivateRepositoryRecipients(
        const RepositoryRecord &repo,
        const QList<QJsonObject> &recipientBundles,
        std::function<void(bool ok, QString error)> onDone);
    void ensurePrivateMirrorRecipientIdentityRegistered(
        std::function<void(bool ok, QString error)> onDone = {});
    void downloadPrivateReplica(int index, bool quiet = false);
    void startSyncFetch(int index, bool quiet, bool hasMirror,
                        const QStringList &args, const QString &beforeDigest,
                        const QString &beforeHeadCommit);
    bool completePendingRepoAutoOpen(int index);
    bool completePendingRepoFork(int index);
    void autoSyncMirrors();
    void syncMirrorRepositoryAt(int index);
    void sweepPullLedgerAt(int index, QSet<QString> *swept);
    // The periodic form of autoSyncMirrors(): one repository per event-loop
    // turn, so the safety timer never holds the GUI thread for a whole fleet.
    void startMirrorSyncSweep();
    void stepMirrorSyncSweep();
    bool m_mirrorSweepActive = false;
    bool m_mirrorSweepLedgerPhase = false; // phase 2 walks the pull ledgers
    int m_mirrorSweepIndex = 0;
    QSet<QString> m_mirrorSweepSeen;   // owner/name already swept this pass
    QString m_mirrorSweepRoutine;      // the routine that started the sweep
    void restartMirrorSyncTimer();
    void syncMirrorsBehindRoster();
    // Does this desktop hold the working copy the mesh treats as canonical for
    // the repo? Only such a node converges *from* the mesh; every other node
    // pulls through its normal upstream sync instead.
    bool isSourceOfTruthRepo(const RepositoryRecord &repo) const;
    static bool mirrorHasCommit(const QString &mirrorPath, const QString &commit);
    struct RosterBehindInput {
        int index = -1;
        QString mirrorPath;
        bool sourceOfTruth = false;
        QList<MirrorAdvert> adverts;
    };
    struct RosterBehindResult {
        int index = -1;
        QString mirrorPath;
        bool behind = false;
        bool artifactsBehind = false;
    };
    static QList<RosterBehindResult>
    readMirrorsBehindRoster(const QList<RosterBehindInput> &inputs);
    bool m_rosterBehindReadInFlight = false;
    bool m_rosterBehindReadPending = false;
    // Quiet transient retries are grouped by canonical repository instead of
    // turning slightly different Git stderr into separate alert cards.
    QHash<QString, qint64> m_mirrorTransientFailureAlertMs;
    // A local source can advance while its served bare mirror is fetching. Git
    // correctly rejects that stale compare-and-swap; retry the whole snapshot
    // a bounded number of times instead of leaving unrelated refs stale.
    QHash<QString, int> m_mirrorRefRaceRetries;
    QHash<QString, QHash<QString, qint64>> m_mirrorSkippedRefs;
    // force = an explicit user action (opening a collaboration tab), which
    // bypasses the periodic sweep's per-repo interval.
    void convergeSourceRepoFromMesh(int index, bool force = false);
    QHash<QString, qint64> m_sourceConvergeAttemptMs; // owner/name -> last try
    QSet<QString> m_sourceConvergePending; // reverse refresh waiting on Git writer
    QSet<QString> m_issueInboxDraining; // one paged drain per repository URL
    QHash<QString, QList<std::function<void()>>> m_issueInboxDrainWaiters;
    void propagateRepoUpdate(int index);
    void propagateRepoUpdate(int index, bool includeManagedHosts);
    void propagateRepoUpdate(int index, bool includeManagedHosts,
                             bool pruneRefs);
    // Branch deletion does not create a Git push-hook event.  Start the same
    // source-to-mirror propagation immediately after a successful local delete.
    void propagateBranchDeletion(const QString &owner, const QString &name);
    void onPeerMirrorUpdated(const QString &ownerName, const QString &peerName,
                             const QString &commit);
    void onPeerMirrorSynced(const QString &ownerName, const QString &peerName,
                            const QString &commit);
    bool applyPeerMirrorCommit(const QString &ownerName, const QString &peerName,
                               const QString &commit);
    // A brand-new ForkMesh account was just created. The relay signs and sends
    // this; celebrate it here the way the World and the website do — confetti,
    // fireworks, and a notice that opens #general so this node can say hello.
    void onNewMemberJoined(const QString &accountName);
    // A peer opened an encrypted cove. If this node created it (creatorKey matches
    // our identity) and the opener's signature checks out, raise a notification.
    void onCoveOpened(const QString &creatorKey, const QString &coveId,
                      const QString &coveName, const QString &openerKey,
                      const QString &openerName, qint64 ts, const QString &signature);
    void onCoveInvited(const QString &inviteeAccount, const QString &coveId,
                       const QString &coveName, const QString &inviterName,
                       const QString &shareUrl, qint64 ts);
    void quickRebuildRestart();
    void maybeStartQueuedRebuild();
    void changeMirrorLocation();
    void changePreviewCacheLocation();
    void refreshRepositoryMirror(int index, bool quietSync = false);
    void ensurePrivateRepositoryControlPlane(int index,
                                             bool showDialogOnError);
    void registerPrivateReplicaRoute(int index);
    void updateRepoActionMenus();
    void deleteCurrentMirror();
    void deleteRepositoryAt(int index, bool reopenRepoDetail);
    void updateRepoDetailStatus();
    void startNodeEventSocket();
    void stopNodeEventSocket();
    QString repositorySource(const RepositoryRecord &repo) const;
    // Fresh "-c http.extraHeader=Authorization: Basic..." git args carrying an
    // owner-key-signed forkmesh-view-v1 token, so clone/fetch of our own private
    // repo through the mainnode passes the relay's read gate. Empty when the repo
    // is public or the source URL is not the mainnode host (never leak the token).
    QStringList viewAuthGitArgs(const RepositoryRecord &repo,
                                const QString &source) const;
    QByteArray privateReplicaAuthorization(
        const RepositoryRecord &repo) const;
    QString repositoryChannel(const RepositoryRecord &repo) const;
    QString repositoryMirrorRoot() const;
    QString repositoryPreviewRoot() const;
    QString repositoryPreviewPath(const QString &owner, const QString &name) const;
    QString repositoryNetworkCloneUrl(const QString &owner, const QString &name) const;
    QString repositoryWebUrl(const QString &owner, const QString &name) const;
    QString repositoryWebUrl(const RepositoryRecord &repo) const;
    QString activeRelayOrigin() const;
    QString repositoryRelayOrigin(const RepositoryRecord &repo) const;
    void stampRepositoryRelay(RepositoryRecord &repo) const;
    bool repositoryBelongsToActiveRelay(const RepositoryRecord &repo) const;
    // The relay stores its own repository list; these pull it, remember it per
    // relay origin, and let it decide which relay a legacy record belongs to.
    forkmesh::relayscope::Scope relayRepositoryScope() const;
    void loadRelayRepositoryLists();
    void saveRelayRepositoryLists() const;
    void refreshRelayRepositoryList();
    void applyRelayRepositoryCatalog(const QString &origin,
                                     const QJsonArray &catalog);
    QUrl catalogApiUrl() const;
    void migrateReposForProfileName(const QString &oldOwner,
                                    const QString &newOwner);
    void onProfileNameChanged(const QString &name);
    void onAvatarChosen(const QByteArray &pngData);
    void showFirewallBanner(const QString &displayCommand,
                            const QString &privilegedCommand);
    void hideFirewallBanner();
    void allowFirewall();
    // Offers the firewall banner when a host firewall stands between the
    // local mesh room and the devices around it (see MeshFirewall.h).
    void checkLocalMeshFirewall();
    void refreshMeshDebugBar();
    void copyMeshDiagnostics();
    void noteLocalMeshReachable();
    void notifyIfInactive(const QString &title, const QString &body,
                          const QString &activateHref = QString(),
                          const QString &icon = QString(),
                          bool fileLocally = true);
    // Direct tray balloons predate postNotification(), so keep their pause
    // check here instead of relying on every event source to remember it.
    void showPingTrayMessage(const QString &title, const QString &body,
                             bool warning = false, int timeoutMs = 6000);
    QString senderColor(const QString &sender) const;

    ForkMeshIdentity m_profileIdentity;
    ChatBackend *m_backend = nullptr;
    QNetworkAccessManager *m_networkAccess = nullptr;
    QList<QJsonObject> m_pendingPullSubmissions;
    QTimer *m_pullSubmissionRetryTimer = nullptr;
    bool m_pullSubmissionInFlight = false;

    QStackedWidget *m_stack = nullptr;
    QStackedWidget *m_sectionStack = nullptr;
    QButtonGroup *m_navGroup = nullptr;
    QVBoxLayout *m_appNavigationRailLayout = nullptr;
    RailFolders::State m_railFolderState;
    bool m_railFolderStateLoaded = false;
    QHash<QString, QPointer<RailFolderWidget>> m_railFolderWidgets;
    RailGroups::State m_railGroupState;
    bool m_railGroupStateLoaded = false;
    QHash<QString, QPointer<RailGroupSeparator>> m_railGroupSeparators;
    QPointer<QPushButton> m_railSortButton;
    QTimer *m_railCountSortTimer = nullptr;
    RailEventHook *m_railEventHook = nullptr;
    QPointer<QWidget> m_railDragSource;
    QPoint m_railDragStart;
    QObject *m_railDragReorder = nullptr;
    RailShortcuts::State m_railShortcutState;
    bool m_railShortcutStateLoaded = false;
    QHash<QString, QPointer<ActivityRailButton>> m_railShortcutButtons;
    QStringList m_knownPageButtons;
    bool m_knownPageButtonsLoaded = false;
    QPointer<QAbstractButton> m_pageProbeButton;
    QElapsedTimer m_pageProbeClock;
    QPointer<QAbstractButton> m_pageDragSource;
    QPoint m_pageDragStart;
    QPointer<QAbstractButton> m_pageDragButton;
    QPointer<QWidget> m_railDropIndicator;
    struct PoppedOutPage
    {
        QPointer<QStackedWidget> stack;
        QPointer<QWidget> page;
        QPointer<QWidget> placeholder;
        QPointer<QWidget> window;
        QString key;
    };
    QList<PoppedOutPage> m_poppedOutPages;
    // The two shell surfaces a reduced view mode gives back to the page, and
    // the chrome button that switches between the modes.
    QWidget *m_appNavigationRail = nullptr; // the rail's scroll area
    QWidget *m_appStatusArea = nullptr;     // the bottom status strip
    // ForkMesh Ads, "desktop" placement (MainWindowAds.cpp): a Sponsored strip
    // at the top of the status area, built when the first ad arrives and never
    // before. The serve call names the placement and nothing else.
    void scheduleAdBanner();
    void refreshAdBanner();
    void refreshAdBannerOnActivation();
    void showAdBanner(const QJsonObject &serveResponse);
    void hideAdBanner();
    QFrame *m_adBanner = nullptr;
    bool m_adBannerScheduled = false; // first fetch queued for this window
    qint64 m_adBannerAskedMs = 0;     // last /api/ads/serve request
    QPointer<QNetworkReply> m_adBannerReply;
    QString m_adBannerId; // the ad on screen; empty while the strip is hidden
    QUrl m_adBannerUrl;
    QPushButton *m_viewModeButton = nullptr;
    QPushButton *m_packageActionsButton = nullptr;
    forkmesh::ui::AppViewMode m_viewMode = forkmesh::ui::AppViewMode::Standard;
    // Agents' list|detail splitter and its Workspace-only terminal pane.
    QSplitter *m_agentWorkspaceSplit = nullptr;
    QWidget *m_workspaceTerminalDock = nullptr;
    TerminalWidget *m_workspaceTerminal = nullptr;
    forkmesh::ui::OnboardingTour *m_onboardingTour = nullptr;
    QTabWidget *m_marketTabs = nullptr;
    QVBoxLayout *m_marketListLayout = nullptr;
    QVBoxLayout *m_marketInstalledListLayout = nullptr;
    QVBoxLayout *m_marketNotInstalledListLayout = nullptr;
    int m_marketAllTabIndex = -1;
    int m_marketInstalledTabIndex = -1;
    int m_marketNotInstalledTabIndex = -1;
    QLabel *m_marketStatus = nullptr;
    QLineEdit *m_marketSearch = nullptr;
    QLabel *m_marketSearchEmpty = nullptr;
    QString m_marketCategoryFilter;
    QComboBox *m_marketSort = nullptr;
    QVBoxLayout *m_marketPacksLayout = nullptr;
    QVBoxLayout *m_marketGuidesLayout = nullptr;
    int m_marketPacksTabIndex = -1;
    int m_marketGuidesTabIndex = -1;
    bool m_marketBatchInstall = false;
    QString m_marketSettingsPackageId;
    QPointer<QProcess> m_marketModelProcess;
    QPointer<QLabel> m_marketModelStatus;
    QString m_marketModelPackageId;
    QString m_marketModelName;
    QString m_marketModelStage;
    QString m_marketModelOutput;
    bool m_marketModelCancelled = false;
    // packageId -> {downloads, installs} from /api/market/installs. Empty until
    // the first successful fetch, which is what keeps a card free of invented
    // numbers rather than showing a zero the relay never reported.
    QHash<QString, QPair<qint64, qint64>> m_packageInstallCounts;
    bool m_packageInstallCountsPending = false;
    qint64 m_packageInstallCountsFetchedMs = 0;
    QHash<QString, MarketPackageScore> m_packageMarketScores;
    bool m_packageMarketScoresPending = false;
    qint64 m_packageMarketScoresFetchedMs = 0;
    QHash<QString, QPixmap> m_marketPreviewCache;
    int m_marketPreviewDecodes = 0;
    std::unique_ptr<forkmesh::market::PackageRegistry> m_packageRegistry;
    std::unique_ptr<forkmesh::capture::CaptureFeature> m_captureFeature;
    std::unique_ptr<forkmesh::ledger::LedgerFeature> m_ledgerFeature;
    struct NativePackagePage {
        QString id;
        QString entrypoint;
        QPointer<QPushButton> navigation;
        QPointer<forkmesh::market::NativePackageWidget> widget;
        QPointer<QWidget> window;
        QPointer<QWidget> placeholder;
        // The page was opened, or warmed for the top search box, so its
        // records are loading or loaded and searchHits has something to read.
        bool searchPrepared = false;
    };
    QList<NativePackagePage> m_nativePackagePages;
    QString m_packageDataTestFilePath;
    std::unique_ptr<forkmesh::timers::TimersFeature> m_timersFeature;
    QWidget *m_timersNavStrip = nullptr;
    QHBoxLayout *m_timersChipRow = nullptr;
    QPushButton *m_timersClockButton = nullptr;
    QTimer *m_timersTick = nullptr;
    // Only the timer strip reads this; window tests move it to watch a
    // countdown run out without waiting for it.
    qint64 m_timersClockOffsetMs = 0;
    // False until the event loop turns after the package attaches, so a timer
    // that expired while the app was closed announces itself to a built window
    // rather than to one still under construction.
    bool m_timersAnnouncementsReady = false;
    QSystemTrayIcon *m_trayIcon = nullptr;

    QList<ServerConfig> m_servers;
    QStringList m_legacyServerUrls;
    QStringList m_legacyServerRooms;
    int m_activeServer = 0;
    QHash<QString, QPixmap> m_faviconCache; // host -> favicon
    QSet<QString> m_faviconFetching;        // hosts with an in-flight favicon GET
    QSet<QString> m_faviconMissing;         // hosts whose favicon GET failed once
    // host -> the 16px pixmap the log views embed. Drawing one means a
    // QPainter, a font resolve and (for downloaded icons) a smooth rescale;
    // doing that per rendered line put a 2.8 s stall inside letterFavicon()
    // when the Logs section re-rendered. The drawing only depends on the host.
    QHash<QString, QPixmap> m_logFaviconPixmaps;
    qreal m_logFaviconPixmapDpr = 0; // the DPR m_logFaviconPixmaps was drawn at

    QWidget *m_solanaBanner = nullptr;
    QLabel *m_solanaBannerLabel = nullptr;
    QWidget *m_walletVerifyBanner = nullptr;
    QWidget *buildWalletVerifyNotice();
    void updateWalletVerifyNotice();

    QLabel *m_breadcrumb = nullptr;
    QPushButton *m_projectMenuButton = nullptr;
    QPushButton *m_relayMenuButton = nullptr;
    QLabel *m_relayJoinDot = nullptr;
    QPushButton *m_relayJoinApproveButton = nullptr;
    int m_pendingRelayJoins = 0;
    RelaySpeedDot *m_relaySpeedDot = nullptr;
    void setRelayLinkSpeed(const QString &host, int ms);
    void refreshRelayMenuTooltip();
    struct RelayLatencySample {
        int ms = -1;
        qint64 stampMs = 0;
    };
    QHash<QString, RelayLatencySample> m_relayHostLatency;
    QSet<QString> m_relaySpeedProbes; // hosts with a dropdown probe in flight
    void probeRelayHostSpeed(const QString &serverUrl,
                             std::function<void(int)> done);
    QString relaySpeedText(const QString &host) const;
    QString relayMenuEntryText(const QString &host) const;
    QTimer *m_relayLatencyTimer = nullptr; // one-minute local node-status repaint
    bool m_relayProbeInFlight = false;     // guard against overlapping probes
    qint64 m_lastWsLatencySampleMs = 0;    // when the room socket last ponged
    int m_relayProbeFailures = 0;          // consecutive failed probes; the radar
    int m_relayProbeElevated = 0;          // consecutive elevated-latency samples;
    // Consecutive probes that failed for a *connectivity* reason -- a refused
    // or unroutable host, a timeout, a dead session. Counted apart from
    // m_relayProbeFailures because a request the firewall or the test build
    // answered locally says nothing about whether this machine has internet,
    // and must never be what puts the app into offline mode.
    int m_offlineProbeFailures = 0;
    static constexpr int kOfflineProbeFailureLimit = 3;
    static constexpr qint64 kAutoOfflineProbeMaxDelayMs = 30 * 1000;
    QLabel *m_nodeLabel = nullptr;
    QLabel *m_repoLabel = nullptr;
    QLabel *m_navSolanaBalance = nullptr;
    QWidget *m_navTokenUsage = nullptr;
    QWidget *m_navCodexUsage = nullptr;
    QAbstractButton *m_nodeOnlineToggle = nullptr;
    QLabel *m_nodeOnlineStatusLabel = nullptr;
    QLabel *m_nodeRewardStatus = nullptr;
    QLabel *m_nodeUptimeLabel = nullptr;
    QWidget *m_profileOnlineSection = nullptr; // wraps the switch + status lines
    bool m_nodeOffline = false;
    bool m_offlineMode = false;     // the app is not using the network at all
    bool m_offlineModeAuto = false; // ...and the connectivity watch is why
    // A stored offline mode the connectivity watch had turned on is not
    // restored (issue #712); this remembers that it was dropped so startup can
    // say so. The companion flag asks the shell to announce a kept one.
    bool m_offlineModeWasAutomaticLastSession = false;
    bool m_offlineModeAnnouncementPending = false;
    qint64 m_navSolanaLamports = -1; // last known balance, -1 = not yet fetched
    qint64 m_navSolanaFetchedMs = 0;
    bool m_navSolanaFetchInFlight = false;
    // Normalized account name -> last GET /api/accounts/<name> body. Holds this
    // node's account and its owner's; capped because each record carries a
    // base64 avatar PNG.
    QHash<QString, QJsonObject> m_accountRecords;
    static constexpr int kAccountRecordCacheMax = 8;
    bool m_profileAccountFetchInFlight = false;
    QString m_webSolanaAccount;
    QString m_webSolanaAddress;
    bool m_webSolanaKnown = false;
    bool m_webSolanaFetchInFlight = false;
    qint64 m_webSolanaFetchedMs = 0;
    QPushButton *m_chatButton = nullptr; // top-bar chat toggle (next to the bell)
    QPushButton *m_tasksNavButton = nullptr;
    int m_organizationTaskBadgeStored = -1;
    QPushButton *m_calendarNavButton = nullptr;
    QPushButton *m_notesNavButton = nullptr;
    QStackedWidget *m_calendarViews = nullptr;
    QTableWidget *m_calendarMonthTable = nullptr;
    QTableWidget *m_calendarWeekTable = nullptr;
    QListWidget *m_calendarDayList = nullptr;
    QListWidget *m_calendarAgendaList = nullptr;
    QListWidget *m_calendarList = nullptr;
    QLineEdit *m_calendarSearch = nullptr;
    QLabel *m_calendarHeading = nullptr;
    QLabel *m_calendarStatus = nullptr;
    QJsonArray m_calendarEvents;
    QJsonArray m_repositoryCalendarEvents;
    QJsonArray m_calendars;
    QString m_calendarActor;
    QDate m_calendarAnchor;
    // Repository activity is read one repository-month at a time, cached by
    // that key, and drained in slices so the first paint does not wait on
    // every repository. `m_repositoryCalendarScans` is what the current view
    // needs; the queue is the part of it not already cached.
    QHash<QString, QJsonArray> m_repositoryCalendarMonths;
    QStringList m_repositoryCalendarMonthOrder;
    QList<forkmesh::ui::RepositoryCalendarScan> m_repositoryCalendarScans;
    QList<forkmesh::ui::RepositoryCalendarScan> m_repositoryCalendarQueue;
    QString m_repositoryCalendarRequest;
    int m_repositoryCalendarCursor = 0;
    bool m_repositoryCalendarScanning = false;
    bool m_repositoryCalendarStale = false;
    QTimer *m_calendarSearchTimer = nullptr;
    int m_calendarAgendaLimit = 0;
    QString m_calendarMode = QStringLiteral("month");
    bool m_calendarLoading = false;
    QListWidget *m_notesList = nullptr;
    QLineEdit *m_noteTitle = nullptr;
    MarkdownEditor *m_noteEditor = nullptr;
    QComboBox *m_noteStorageMode = nullptr;
    QCheckBox *m_notePublic = nullptr;
    QLabel *m_notePublicLink = nullptr;
    QLabel *m_notesStatus = nullptr;
    QJsonArray m_localNotes;
    QJsonArray m_cloudNotes;
    QJsonObject m_selectedNote;
    QString m_selectedNoteStorage;
    bool m_notesLoading = false;
    bool m_noteDirty = false;
    QTableWidget *m_organizationTasksTable = nullptr;
    QString m_organizationTasksSearchMirror;
    QTextBrowser *m_organizationTaskDetail = nullptr;
    QTextBrowser *m_organizationTaskFields = nullptr;
    QWidget *m_organizationTaskSide = nullptr;
    QWidget *m_organizationTaskProperties = nullptr;
    QComboBox *m_organizationTaskStatusEdit = nullptr;
    QSpinBox *m_organizationTaskPriorityEdit = nullptr;
    QSpinBox *m_organizationTaskProgressEdit = nullptr;
    QComboBox *m_organizationTaskAssigneeEdit = nullptr;
    QComboBox *m_organizationTaskDepartmentEdit = nullptr;
    QLineEdit *m_organizationTaskTeamEdit = nullptr;
    QLineEdit *m_organizationTaskRepositoryEdit = nullptr;
    QLineEdit *m_organizationTaskParentEdit = nullptr;
    QString m_organizationTaskPropertiesTaskId;
    QPlainTextEdit *m_organizationTaskQuickComment = nullptr;
    QPushButton *m_organizationTaskQuickCommentSend = nullptr;
    QString m_organizationTaskQuickCommentTaskId;
    QHash<QString, QString> m_organizationTaskCommentDrafts;
    QPushButton *m_organizationTaskRefreshButton = nullptr;
    QLabel *m_organizationTasksStatus = nullptr;
    QLabel *m_organizationTasksSummary = nullptr;
    QPushButton *m_organizationTaskNewButton = nullptr;
    QPushButton *m_organizationTaskDeleteClosedButton = nullptr;
    QPushButton *m_organizationTaskSortButton = nullptr;
    QPushButton *m_organizationTaskViewButton = nullptr;
    forkmesh::ui::ListViewMode m_organizationTaskListView =
        forkmesh::ui::ListViewMode::Full;
    QPushButton *m_organizationTaskMineFilterButton = nullptr;
    QWidget *m_organizationTaskPeopleStrip = nullptr;
    QHBoxLayout *m_organizationTaskPeopleLayout = nullptr;
    QButtonGroup *m_organizationTaskPeopleGroup = nullptr;
    QStringList m_organizationTaskPeopleKeys;
    QPushButton *m_organizationTaskOpenFilterButton = nullptr;
    QPushButton *m_organizationTaskClosedFilterButton = nullptr;
    QPushButton *m_organizationTaskAllFilterButton = nullptr;
    QPushButton *m_organizationTaskFollowUpButton = nullptr;
    QPushButton *m_organizationTaskEditButton = nullptr;
    QPushButton *m_organizationTaskAgentButton = nullptr;
    QPushButton *m_organizationTaskStartButton = nullptr;
    QPushButton *m_organizationTaskCompleteButton = nullptr;
    QPushButton *m_organizationTaskUndoButton = nullptr;
    QPushButton *m_organizationTaskQaButton = nullptr;
    QPushButton *m_organizationTaskReturnButton = nullptr;
    QPushButton *m_organizationTaskDeleteButton = nullptr;
    QPushButton *m_organizationTaskAttachWorktreeButton = nullptr;
    QHash<QString, QJsonArray> m_taskWorkItemAttachQueue;
    QHash<QString, QJsonArray> m_taskWorkItemAttachSending;
    QSet<int> m_taskWorkItemSessionsAttached;
    QHash<int, QString> m_taskWorkItemSessionWorktree;
    QSet<QString> m_taskWorkItemCleanupInFlight;
    QPushButton *m_organizationTaskPromptButton = nullptr;
    QPushButton *m_organizationTaskIssueButton = nullptr;
    QPushButton *m_organizationTaskStartAgentButton = nullptr;
    QPushButton *m_organizationTaskCommentButton = nullptr;
    QPushButton *m_organizationTaskCheckinButton = nullptr;
    QPushButton *m_organizationTaskRemindButton = nullptr;
    QLabel *m_organizationTaskPageLabel = nullptr;
    QList<int> m_organizationTaskMatches;
    int m_organizationTaskShownRows = 0;
    QTimer *m_organizationTaskSpinTimer = nullptr;
    qreal m_organizationTaskSpinAngle = 0.0;
    QString m_organizationTaskSortKey = QStringLiteral("created-desc");
    QString m_organizationTaskStatusFilter = QStringLiteral("open");
    QString m_organizationTaskAssigneeFilter = QStringLiteral("all");
    QJsonArray m_organizationTasks;
    // Edits made on this device, by task id: the optimistic row while the
    // PATCH is in flight, then the relay's answer until a catalog read at
    // least as new (by updatedAt) replaces it.
    QHash<QString, QJsonObject> m_organizationTaskLocalEdits;
    QHash<QString, int> m_organizationTaskEditsInFlight;
    QString m_pendingOrganizationTaskSelection;
    QStringList m_organizationTaskMembers;
    // Timeline per task id, and the id whose read is in flight, so selecting
    // through the list quickly cannot stack one request per row.
    QHash<QString, QJsonArray> m_organizationTaskHistory;
    QHash<QString, qint64> m_organizationTaskHistoryAt;
    QString m_organizationTaskHistoryInFlight;
    QStringList m_organizationTaskDepartments;
    QString m_organizationTaskActor;
    bool m_organizationTasksCanManage = false;
    bool m_organizationTaskIssueMoveInFlight = false;
    bool m_organizationTaskClosedDeleteInFlight = false;
    QStringList m_organizationTaskClosedDeleteIds;
    int m_organizationTaskClosedDeleteTotal = 0;
    int m_organizationTaskClosedDeleteCount = 0;
    int m_organizationTaskClosedDeleteRetries = 0;
    int m_organizationTaskClosedDeleteKept = 0;
    bool m_organizationTaskClosedDeleteForce = false;
    QStringList m_organizationTaskClosedDeleteBusy;
    QStringList m_organizationTaskClosedDeleteReasons;
    bool m_organizationTasksLoading = false;
    // The task list is pulled once per launch, once when the page first opens,
    // and whenever the user asks (Refresh, or their own edit) — never on a
    // timer. The startup pull lands before the page is built, so it parks its
    // payload here and the first open renders from it instead of pulling again.
    bool m_organizationTasksLoaded = false;
    // A "tasks" push that arrived while a board read was already in flight.
    // That read may predate the write, so it is re-run when this is set.
    bool m_organizationTaskEventPending = false;
    QJsonObject m_organizationTasksStartupPayload;
    qint64 m_organizationTasksLoadedAt = 0;
    QHash<QString, QByteArray> m_organizationTaskImageData;
    QHash<QString, QString> m_organizationTaskImageMime;
    QSet<QString> m_organizationTaskImageLoads;
    // Settings -> Organizations (MainWindowOrganizations.cpp).
    QTableWidget *m_organizationsTable = nullptr;
    QLabel *m_organizationsStatus = nullptr;
    QLineEdit *m_organizationNewName = nullptr;
    QLineEdit *m_organizationNewDisplay = nullptr;
    QLineEdit *m_organizationNewDescription = nullptr;
    QWidget *m_organizationDetail = nullptr;
    QLabel *m_organizationHeading = nullptr;
    QLineEdit *m_organizationDisplayEdit = nullptr;
    QLineEdit *m_organizationDescriptionEdit = nullptr;
    QPushButton *m_organizationDeleteButton = nullptr;
    QTableWidget *m_organizationMembersTable = nullptr;
    QLineEdit *m_organizationMemberName = nullptr;
    QComboBox *m_organizationMemberRole = nullptr;
    QTableWidget *m_organizationTeamsTable = nullptr;
    QLineEdit *m_organizationTeamName = nullptr;
    QComboBox *m_organizationTeamPermission = nullptr;
    QLabel *m_organizationTeamMembersLabel = nullptr;
    QTableWidget *m_organizationTeamMembersTable = nullptr;
    QLineEdit *m_organizationTeamMemberName = nullptr;
    QTableWidget *m_organizationReposTable = nullptr;
    QLineEdit *m_organizationRepoName = nullptr;
    QLineEdit *m_organizationRepoNode = nullptr;
    QJsonArray m_organizations;
    // Per-org member/team/repo counts, filled in as each detail loads: the
    // collection endpoint returns names and roles only, so an unvisited row
    // shows a dash rather than a misleading zero.
    QJsonObject m_organizationCounts;
    QString m_organizationPendingSelection;
    bool m_organizationsLoading = false;
    bool m_organizationsLoaded = false;
    int m_organizationsSettingsTabIndex = -1;
    QPushButton *m_agentsNavButton = nullptr;
    AgentDotMatrix *m_agentDotMatrix = nullptr;
    NodeDotMatrix *m_nodeDotMatrix = nullptr;
    QWidget *m_chromeDotDivider = nullptr;
    void refreshNodeDotMatrix();
    void updateChromeDotDivider();
    ActionRunStrip *m_actionRunStrip = nullptr;
    QWidget *m_chromeActionDivider = nullptr;
    QString m_agentDotTooltipKey;
    QString m_actionRunStripTooltipKey;
    QHash<QString, QString> m_actionWorkflowIcons;
    QLabel *m_connectionDot = nullptr;
    QString m_connectionStatusColor;      // last dot colour (skip redundant repaints)
    QLabel *m_adminCrownBadge = nullptr;
    QWidget *m_pageNoticeArea = nullptr;
    QVBoxLayout *m_pageNoticeLayout = nullptr;
    forkmesh::ui::kit::InlineNotice *m_loadStatusNotice = nullptr;
    QList<PageNotice> m_pageNotices;
    QList<QPointer<QWidget>> m_pageNoticeRows;
    QStringList m_recentNotices;
    quint64 m_nextPageNoticeId = 1;
    QString m_renderedNoticePage;
    QWidget *m_errorBorderOverlay = nullptr;
    QTimer *m_errorBorderTimer = nullptr;
    QWidget *m_celebrationBorderOverlay = nullptr;
    QTimer *m_celebrationBorderTimer = nullptr;
    bool m_noticeOwnsLoggedError = false;
    QString m_lastLocalOperationError;
    QString m_lastOperationNotice;
    quint64 m_operationNoticeSerial = 0;
    bool m_inLoggedErrorAlert = false;    // re-entrancy guard for alertOnLoggedError
    bool m_muteLoggedErrorAlert = false;
    QString m_lastLoggedErrorText;        // last error alerted on, for repeat de-duplication
    qint64 m_lastLoggedErrorAtMs = 0;
    forkmesh::ErrorPingGroups m_loggedErrorGroups;
    forkmesh::PlannedRestarts m_plannedRestarts;
    QTimer *m_plannedRestartTimer = nullptr;
    bool m_errorLogMonitorsResumed = false;
    int m_errorLogRepairHeldForSession = 0;
    QTimer *m_errorLogRepairRetryTimer = nullptr;
    QTimer *m_logMonitorRetryWakeTimer = nullptr;
    qint64 m_logMonitorRetryWakeAtMs = 0;
    bool m_crashRecordsQueued = false; // the log is scanned once a launch
    QWidget *m_restartCautionBorderOverlay = nullptr;
    QTimer *m_restartCautionBorderTimer = nullptr;
    bool m_repoPinMismatch = false;
    QHash<QString, qint64> m_repoPinAutoHealAtMs; // owner/name -> last automatic pin re-attest (rate-limits the source-of-truth auto-heal in refreshRepoPinBanner)

    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_solanaEdit = nullptr;
    QLabel *m_pubkeyLabel = nullptr;
    QLineEdit *m_serverUrlEdit = nullptr;
    QLineEdit *m_roomNameEdit = nullptr;
    QLabel *m_setupError = nullptr;
    QPushButton *m_updateButton = nullptr;
    QLabel *m_updateStatus = nullptr;
    QLabel *m_buildStatusLabel = nullptr;
    QPushButton *m_buildButton = nullptr;
    QDialog *m_updateLogDialog = nullptr;
    QPlainTextEdit *m_updateLog = nullptr;
    QTextEdit *m_footerUpdateLog = nullptr;
    QPushButton *m_footerLogPauseButton = nullptr;
    bool m_footerLogScrollPaused = false;
    QWidget *m_footerDock = nullptr;
    QWidget *m_footerLeftRegion = nullptr;
    QWidget *m_debugBar = nullptr;
    QHBoxLayout *m_debugActivityLayout = nullptr;
    QPushButton *m_statusVersionButton = nullptr;
    QAbstractButton *m_statusOfflineToggle = nullptr;
    QLabel *m_statusOfflineLabel = nullptr;
    bool m_debugBarStartupApplied = false;
    QCheckBox *m_debugBarStartupCheck = nullptr;
    QTextEdit *m_debugLogTail = nullptr;
    QPushButton *m_debugLogTailButton = nullptr;
    QPushButton *m_debugCopyDiagnosticsButton = nullptr;
    int m_debugLogTailHeight = 0; // the strip the window grows by, in pixels
    bool m_debugLogTailShown = false;
    QStringList m_footerLogPending;
    QStringList m_debugLogTailPending;
    QTimer *m_footerLogFlushTimer = nullptr;
    QPointer<VerticalIconButton> m_cloudLogFilterChip;
    QCheckBox *m_cloudLogMonitorSettingCheck = nullptr;
    QProcess *m_cloudLogMonitorProcess = nullptr;
    QByteArray m_cloudLogMonitorBuffer; // partial tail record across reads
    QQueue<QString> m_cloudLogMonitorPendingRecords;
    bool m_cloudLogMonitorDrainScheduled = false;
    int m_cloudLogMonitorErrors = 0;     // errors seen since monitoring began
    int m_cloudLogMonitorEvents = 0;     // Worker events seen since then
    bool m_cloudLogMonitorStopping = false; // a deliberate stop, not a crash
    QTimer *m_cloudLogMonitorRestartTimer = nullptr;
    QElapsedTimer m_cloudLogMonitorUptime;
    int m_cloudLogMonitorRestarts = 0;
    bool m_cloudLogMonitorResuming = false;
    bool m_cloudLogMonitorWorkerMissing = false;
    QString m_cloudLogMonitorWorkerName;
    bool m_cloudLogNodeProbeRunning = false;
    forkmesh::control::NodeToolchain m_cloudLogNodeToolchain;
    QString m_cloudLogMonitorIdleReason;
    QWidget *m_globalOverlayHost = nullptr;
    void setupGenieSpider();
    bool genieSpiderShown() const;
    void setGenieSpiderShown(bool shown);
    void installGeniePetHost();
    void positionGenieSpider();
    void applyGenieVitals();
    void measureGenieVitals(bool force);
    void fetchGenieCrm(qint64 since);
    void fetchGenieMarketing(qint64 since);
    void finishGenieMeasure();
    GenieWatch genieWatch() const;
    void setGenieWatch(const GenieWatch &watch);
    void noteGenieOrganizations(const QJsonArray &organizations);
    QWidget *buildGenieWatchSettings();
    void noteGeniePage();
    void recordGenieVisit(const QString &page);
    void openGeniePage(const QString &page);
    void toggleGenieCoach();
    void showGenieMenu(const QPoint &globalPos);
    void showNextGenieTip();
    void danceGenie();
    void cheerGenieFor(const QJsonObject &alert);
    GenieDesktopStage *genieStage();
    void releaseGenieToDesktop(const QPoint &grab);
    void moveGenieToDesktop(bool thrown, const QPointF &foot, const QPointF &velocity);
    void bringGenieInside();
    void addGenieTipToAgent(const QString &prompt);
    void raiseForGenie();
    GenieMarketingFacts genieFacts() const;
    GenieLessons genieSpiderLessons() const;
    bool genieSpiderTakesReplies() const;
    int genieSpiderTeammateRepo() const;
    void replyToGenieSpider(const QString &tip);
    QString genieSpiderBriefing(const QString &teammateId) const;
    void learnFromGenieSpiderRun(const QString &teammateId, const QString &summary);
    QString genieSaidPath() const;
    const GenieSaidLog &genieSaidLog() const;
    bool saveGenieSaidLog();
    QString recordGenieSaid(const QString &kind, const QString &text, const QString &task, const QString &basis);
    void commentOnGenieSaid(const QString &id);
    void removeGenieSaidComment(const QString &id, int index);
    void fillTeammateHomeSaid();
    QPointer<GenieSpiderWidget> m_genieSpider;
    QPointer<GenieDesktopStage> m_genieStage;
    bool m_genieOnDesktop = false;
    QPointer<GenieCoachPanel> m_genieCoach;
    QPointer<GenieTipBubble> m_genieTip;
    QTimer *m_genieTimer = nullptr;
    GenieVitalSigns m_genieSigns;
    GenieCrmCounts m_genieCrm;
    bool m_genieCrmKnown = false;
    bool m_genieReadme = false;
    bool m_genieReadmePicture = false;
    QString m_genieReadmeRepository;
    GenieCrmDigest m_genieCrmDigest;
    GenieMarketingDigest m_genieMarketingDigest;
    QList<GenieShippedChange> m_genieShipped;
    bool m_genieShippedKnown = false;
    bool m_genieRemeasure = false;
    QList<QPointer<QCheckBox>> m_genieWatchChecks;
    QPointer<QCheckBox> m_genieShownCheck;
    int m_genieMeasuring = 0;
    int m_genieLevel = -1;
    qint64 m_genieMeasuredMs = 0;
    QString m_genieLastTip;
    QString m_genieLastSaidId;
    mutable GenieSaidLog m_genieSaid;
    mutable bool m_genieSaidLoaded = false;
    int m_genieTipIndex = 0;
    QWidget *m_promptOverlayHost = nullptr;
    forkmesh::ui::LogActivityLights *m_logActivityLights = nullptr;
    forkmesh::ui::LogActivityLights *m_logActivityHeader = nullptr;
    QPushButton *m_debugLogAllButton = nullptr;
    int m_debugAllLogCount = 0;
    bool m_logOverlayExpanded = false;
    bool m_promptOverlayCollapsed = false;
    QWidget *m_promptDragHandle = nullptr;
    QWidget *m_promptResizeGrip = nullptr;
    QPushButton *m_promptDetachButton = nullptr;
    QPushButton *m_promptResetButton = nullptr;
    QPushButton *m_promptClearButton = nullptr; // empties the prompt text area
    // Ticked (default): a prompt's agent gets its own branch and worktree.
    // Unticked: the run works directly in the repository's checkout.
    QCheckBox *m_promptWorktreeCheck = nullptr;
    QPushButton *m_promptCloudButton = nullptr;
    QCheckBox *m_promptVibeCheck = nullptr;
    int m_promptAnchoredHeight = 0;
    bool m_positioningFooterOverlays = false;
    QWidget *m_promptDetachWindow = nullptr;
    bool m_promptOverlayFloating = false;
    bool m_promptOverlayDetached = false;
    QPoint m_promptOverlayPos;  // top-left within m_globalOverlayHost
    QSize m_promptOverlaySize;  // user-chosen size; invalid means "auto"
    QSize m_promptOverlayHostSize;
    QRect m_promptDetachGeometry;
    bool m_promptPlacementDragging = false;
    bool m_promptDetachDragging = false;
    bool m_promptDetachMoveAsked = false;
    bool m_promptDetachResizing = false;
    bool m_promptPlacementResizing = false;
    QPoint m_promptPlacementGrab;
    QRect m_promptPlacementStartRect;
    bool m_footerWebsiteStatusInFlight = false;
    bool m_statusPagePackageInstalled = true;
    struct FooterStatusRow {
        QString id;
        QString label;
        QString status;
        QString reason;
        qint64 minuteTs = 0;
        bool local = false; // measured here rather than reported by the relay
        QString sampleStatus;
    };
    QList<FooterStatusRow> m_footerRelayStatuses;
    QList<FooterStatusRow> m_footerDesktopStatuses;
    QList<FooterStatusRow> m_footerCustomStatuses;
    struct StatusCustomMonitor {
        QString id;
        QString name;
        QString url;
    };
    QList<StatusCustomMonitor> m_statusCustomMonitors;
    bool m_statusCustomMonitorsLoaded = false;
    int m_statusCustomMonitorsMax = 20;
    qint64 m_statusMonitorsReadMs = 0;
    bool m_statusMonitorsInFlight = false;
    bool m_statusMonitorsBusy = false;
    QString m_statusMonitorsError;
    QString m_statusMonitorDraftName;
    QString m_statusMonitorDraftUrl;
    QSet<QString> m_statusShownDesktopProbes;
    bool m_statusShownDesktopProbesKnown = false;
    qint64 m_accountPackagesReadMs = 0;
    bool m_accountPackagesInFlight = false;
    int m_accountPackagesGeneration = 0;
    bool m_applyingAccountPackages = false;
    QSet<QString> m_desktopProbesInFlight;
    // When the footer status was last read from the relay. Nothing re-reads
    // it on a timer; window activation does, once this is old enough.
    qint64 m_footerStatusReadMs = 0;
    struct DesktopProbeSample {
        qint64 ts = 0;
        QString status;
    };
    QHash<QString, QList<DesktopProbeSample>> m_desktopProbeHistory;
    qint64 m_localFirewallAlertMs = 0; // last "firewall is blocking ForkMesh" ping
    bool m_adminConsoleUrlInFlight = false;
    QWidget *m_statusBackgroundHost = nullptr;
    QHBoxLayout *m_statusBackgroundLayout = nullptr;
    QLabel *m_statusBackgroundLabel = nullptr;
    QHash<QString, forkmesh::ui::BackgroundTaskChip *>
        m_backgroundTaskChips;                           // word -> chip
    QHash<QString, int> m_backgroundTaskCounts;          // word -> open tickets
    QHash<QString, qint64> m_backgroundTaskSince;        // word -> first ticket ms
    QHash<QString, QString> m_backgroundTaskDetails;     // word -> newest note
    // word -> where the newest note's ticket was opened. Tracked beside the
    // note rather than folded into it because the outcome line links to it
    // instead of printing it, and the two have to describe the same run.
    QHash<QString, forkmesh::LogSource> m_backgroundTaskOrigins;
    QHash<quint64, QString> m_backgroundTaskWords;       // ticket -> word
    struct BackgroundBlockingSpan {
        qint64 longestMs = 0;
        QString detail;
        forkmesh::LogSource source;
        BackgroundTaskContext context;
    };
    QHash<QString, BackgroundBlockingSpan> m_backgroundTaskBlocking;
    QHash<quint64, qint64> m_backgroundTaskBlockingStarts;
    QHash<QString, int> m_backgroundTaskBackgroundedRuns;
    // word -> the routines, checkout, queue waits and failures its open
    // tickets reported (adhoc #186)
    QHash<QString, BackgroundTaskContext> m_backgroundTaskContexts;
    struct BackgroundOutcomeTally {
        int runs = 0;
        qint64 longestMs = 0;
        qint64 firstAt = 0;
        QString detail;
        forkmesh::LogSource source;
        QStringList routines;     // every distinct routine the runs ran for
        QString workingDirectory; // the newest run's checkout
        qint64 longestQueuedMs = -1;
        int failures = 0;
        QString lastFailure;
    };
    QHash<QString, BackgroundOutcomeTally> m_backgroundTaskDone; // word -> ✓
    QHash<QString, BackgroundOutcomeTally> m_backgroundTaskUiBlocking; // word -> ✕
    QTimer *m_backgroundTaskSpinTimer = nullptr;
    qreal m_backgroundTaskSpinAngle = 0.0; // shared ring rotation, degrees
    int m_backgroundTaskIdleTicks = 0;
    QString m_updateAsUser;

    QLabel *m_statusLine = nullptr;
    QLabel *m_channelTitle = nullptr;
    QLabel *m_encryptionLabel = nullptr;
    QLabel *m_serverCopiesLabel = nullptr;
    QHash<QString, int> m_directServerCopies;
    // "Seal"/"Sealed" — sets or removes the room's participant-held second
    // encryption layer.
    QPushButton *m_roomSealButton = nullptr;
    QPushButton *m_inviteButton = nullptr; // "Invite" — shown only in private rooms
    QPushButton *m_callAudioButton = nullptr;
    QPushButton *m_callVideoButton = nullptr;
    // "Settings" — shown only on an office channel this account manages.
    QPushButton *m_channelSettingsButton = nullptr;
    // QPointer, so a dialog closed by WA_DeleteOnClose clears this without a
    // destroyed() handler writing into a MainWindow that is being torn down.
    QPointer<QDialog> m_channelSettingsDialog;
    forkmesh::ui::ChannelGroupTree *m_channelList = nullptr;
    QPushButton *m_nodeMenuButton = nullptr; // top-bar node switcher
    QString m_navSolanaBalanceAddress;
    struct NodeMenuEntry {
        QString name;
        QString platform;
        bool online = false;
        bool self = false;
        int repoCount = 0;
    };
    QList<NodeMenuEntry> m_nodeMenuEntries;
    QList<NodeMenuEntry> m_nodeDotEntries;
    // refreshNodesTable() pieces (MainWindowNodes.cpp): the merged row set,
    // one table row, and that row's signed-catalog columns.
    void collectVisibleMeshNodes(QList<NodeMenuEntry> &visible,
                                 QList<MemberInfo> &visibleRoster);
    // Live state shared by the Nodes table and the chrome dot matrix: the
    // relay's onlineNodes set is authoritative once fetched, except for our
    // own node, whose local backend knows better.
    bool nodeEntryOnline(const NodeMenuEntry &entry) const;
    bool fillNodesTableRow(int row, const NodeMenuEntry &entry,
                           const QSet<QString> &savedHosts,
                           const MemberInfo &member, qint64 tableNowMs,
                           bool &sawCatalogRecord);
    bool fillNodesTableCatalogCells(int row, const QString &nodeName);
    struct NodeDotRepoState {
        bool online = false; // the Mirrors panel saw it serving this repo
        bool behind = false;
        bool integrityFailing = false;
    };
    QHash<QString, NodeDotRepoState> m_nodeDotRepoStates;
    void setNodeDotRepoStates(const QHash<QString, NodeDotRepoState> &states);
    QString m_selectedNode;             // node whose repos fill the repos column
    QPushButton *m_repoMenuButton = nullptr; // top-bar repo switcher
    QPushButton *m_repoViewButton = nullptr; // "Code" button on the repo header row
    QPushButton *m_reposNavButton = nullptr; // network-wide "Repos" section
    QPushButton *m_settingsNavButton = nullptr; // Settings button on the repo header row
    QPushButton *m_tourNavButton = nullptr; // replay the guided feature tour
    QPushButton *m_logNavButton = nullptr; // full Log destination in the left rail
    QPushButton *m_floatingLogButton = nullptr; // retired; retained for safe resize no-op
    QPushButton *m_controlNodeNavButton = nullptr; // local control-node operations
    QPushButton *m_networkNavButton = nullptr; // "Network" diagnostics top-nav button
    QPushButton *m_usersNavButton = nullptr; // admin-only World user statistics
    QPushButton *m_teamNavButton = nullptr;  // AI teammates, below Users
    QPushButton *m_filesNavButton = nullptr; // local file explorer, under Users
    QPushButton *m_statsNavButton = nullptr; // stat tiles dashboard, under Files
    QPushButton *m_componentHealthNavButton = nullptr; // code health board
    QPushButton *m_memoryNavButton = nullptr; // live memory breakdown
    QPushButton *m_homeGridNavButton = nullptr;
    QPushButton *m_navRebuildButton = nullptr; // small rebuild+restart button (opt-in)
    QPushButton *m_navSignInButton = nullptr;
    QPushButton *m_accountSwitcherButton = nullptr;
    QPushButton *m_navScreenshotButton = nullptr; // drag-a-region screenshot -> prompt
    QPushButton *m_ledgerNavButton = nullptr; // installed Ledger package shortcut
    QTableWidget *m_ledgerTable = nullptr;
    QLabel *m_ledgerSummary = nullptr;
    QLabel *m_ledgerTotals = nullptr; // income and expenses for the whole page
    QLabel *m_ledgerStatus = nullptr;
    QWidget *m_ledgerWalletStrip = nullptr;
    QPushButton *m_ledgerRefreshButton = nullptr;
    QPushButton *m_ledgerSendPaymentButton = nullptr;
    QCheckBox *m_ledgerPendingOnly = nullptr;
    QWidget *m_ledgerPayoutHoldPanel = nullptr;
    QLabel *m_ledgerPayoutHoldTitle = nullptr;
    QLabel *m_ledgerPayoutHoldDetails = nullptr;
    QPushButton *m_ledgerPayoutRetryButton = nullptr;
    QPushButton *m_ledgerPayoutExplorerButton = nullptr;
    QPushButton *m_ledgerPayoutUndoButton = nullptr;
    // The entry form: one full record, of any kind, in the reader's own book.
    QComboBox *m_ledgerEntryDirection = nullptr;
    QDateEdit *m_ledgerEntryDate = nullptr;
    QLineEdit *m_ledgerEntryCategory = nullptr;
    QLineEdit *m_ledgerEntryDescription = nullptr;
    QLineEdit *m_ledgerEntryCounterparty = nullptr;
    QLineEdit *m_ledgerEntryAmount = nullptr;
    QComboBox *m_ledgerEntryCurrency = nullptr;
    QCheckBox *m_ledgerEntryOutstanding = nullptr;
    // Two separate relay answers, deliberately not merged on arrival: the
    // page is what the public ledger says happened, and the overlay is the
    // owner-authored plan naming an exact recipient address and amount. A row
    // only offers to pay where both agree.
    forkmesh::ledger::LedgerPage m_ledgerPage;
    QList<forkmesh::ledger::LedgerRecord> m_ledgerPayable;
    bool m_ledgerRecordsLoaded = false;
    bool m_ledgerPayableLoaded = false;
    bool m_ledgerRecordsInFlight = false;
    bool m_ledgerPayableInFlight = false;
    bool m_ledgerRecipientLookupInFlight = false;
    qint64 m_ledgerLoadedAtMs = 0;
    // Public balance readings for the wallets this machine names itself,
    // keyed by address: lamports, and when the relay last checked. A missing
    // key means "not read yet", which is a different card from "empty".
    QHash<QString, QPair<qint64, qint64>> m_ledgerWalletBalances;
    QSet<QString> m_ledgerWalletBalanceFetches; // addresses in flight
    // A payment may finish while the page-opening balance read is still in
    // flight. Remember its forced follow-up instead of silently keeping the
    // pre-payment result for the remainder of the cache window.
    QSet<QString> m_ledgerWalletBalanceRefreshQueued;
    QStringList m_ledgerManualWallets; // addresses typed into the form above
    // The reader's own records, kept in QSettings and merged into the relay
    // page on every rebuild. Never sent anywhere: the relay's ledger is the
    // public record, and this is the book beside it.
    QList<forkmesh::ledger::LedgerRecord> m_ledgerLocalRecords;
    bool m_ledgerLocalRecordsLoaded = false;
    QString m_ledgerPayingRecordId;
    // The row whose review dialog is open. One at a time; it does not wait on
    // a payment in flight, which is what m_ledgerPayingRecordId tracks.
    QString m_ledgerPreparingRecordId;
    QString m_ledgerPayingWalletAddress;
    bool m_ledgerPaymentStarted = false;
    bool m_ledgerPaymentProcessing = false;
#ifdef FORKMESH_WINDOW_TESTS
    // The destination the last ledger click handed over, recorded instead of
    // opened so the offscreen suite can assert which click follows which link.
    QUrl m_ledgerLastOpenedLink;
#endif
    // The tip flow narrates itself through tipStatus(), which writes to the
    // system log. A payment started from the Ledger sets this so those lines
    // reach the page the reader is actually on.
    bool m_ledgerPaymentReporting = false;
    // The last thing a payment said, still owed to the reader: paying ends by
    // refreshing the ledger, and the redraw that lands afterwards would
    // otherwise put the page's standing summary over the outcome. Held until
    // the reader's next action on the page, which then owns the status line.
    QString m_ledgerPaymentLine;
    QString m_ledgerRemindingRecordId; // reward whose reminder is in flight
    // Contributors asked for a payout address during this run. Keyed on the
    // person because the relay's limit is: one reminder per contributor, not
    // one per reward they are owed.
    QSet<QString> m_ledgerRemindedRecipients;
    // When the relay last sent each contributor (lower-cased account name) a
    // payout-address reminder, in epoch ms, from the owner-scoped estimate
    // list or a reminder reply. A reminder sent from another machine, or
    // before this run, still shows on the Remind button.
    QHash<QString, qint64> m_ledgerReminderSentAt;
    int m_ledgerReminderTimesInFlight = 0;
    QPushButton *m_marketNavButton = nullptr; // creator marketplace and Library
    QPushButton *m_navResizeButton = nullptr; // snap window to a common minimal size
    QPushButton *m_restartSpinButton = nullptr; // button whose icon spins mid-restart
    QLabel *m_restartCountdownLabel = nullptr;
    QTimer *m_restartCountdownTimer = nullptr;
    QElapsedTimer m_restartElapsedTimer;
    // Remaining time estimated when the latest milestone was first reached,
    // and how far into the restart that was; the countdown runs from there.
    qint64 m_restartMilestoneRemainingMs = 0;
    qint64 m_restartMilestoneElapsedMs = 0;
    QMap<int, qint64> m_restartMilestones; // percent -> elapsed ms, first reach
    int m_restartProgress = 0;
    int m_restartHistoryCount = 0;
    RestartKind m_restartKind = RestartKind::Rebuild;
    bool m_restartDurationRecorded = false;
    // What the restart is doing ("configuring", "compiling") and, during a
    // build pass, the build's own live step, shown beside the percentage.
    QString m_restartPhase;
    int m_restartBuildPass = 0;       // 0 outside the build step
    QString m_restartBuildStep;       // "compiling 412/1210"
    QString m_restartBuildLine;       // the progress line as the build printed it
    QString m_restartOutputCarry;     // a partial line held for its newline
    QList<QPointer<QProcess>> m_updateSteps; // live rebuild/update child processes
    int m_updateRunToken = 0; // bumped on cancel; stale step callbacks then bail
    bool m_rebuildRestartQueued = false;
    QTimer *m_rebuildQueuePollTimer = nullptr;
    QTableWidget *m_networkReposTable = nullptr;
    QLabel *m_networkReposStatus = nullptr;
    QPushButton *m_networkReposRefreshButton = nullptr;
    int m_networkReposLoadGen = 0;
    // "owner/name" -> last good /api/repo/*/mirrors body and when it landed.
    // The mirror tally is fetched per table row, so without this every
    // re-render re-asked the relay about every repository on the page.
    QHash<QString, QByteArray> m_networkRepoMirrorsCache;
    QHash<QString, qint64> m_networkRepoMirrorsFetchedMs;
    QJsonArray m_networkReposLastPayload; // regroup when user/node ownership arrives
    // -1 until the catalog table has been filled once; the local-only table
    // stays blank before that rather than claiming every repo is relay-less.
    int m_networkRepoRowCount = -1;
    // "Private & local" table under the catalog: this machine's repositories
    // that no row above already stands for.
    QTableWidget *m_localReposTable = nullptr;
    QLabel *m_localReposStatus = nullptr;
    // m_repositories indexes the catalog table already accounts for, filled in
    // by renderNetworkRepos as it resolves each row's local fork/mirror.
    QSet<int> m_networkReposCoveredLocal;
    // "owner/name" -> the advert refreshMirrorAdverts last computed for it, so
    // the local rows can show branch/commit/counts without new git work.
    QHash<QString, MirrorAdvert> m_localRepoAdverts;
    // Opaque private archive ids are delivered only in an authenticated,
    // ACL-filtered catalog response. They let the client use a name-free
    // /api/private-replicas/<id> URL; no private owner/repository identity is
    // placed in an edge-visible path or query string.
    QHash<QString, QString> m_privateCatalogAccessIds; // owner/name -> 64 hex
    // relay origin -> owner/name keys that relay last listed for this desktop.
    QHash<QString, QSet<QString>> m_relayRepositoryLists;
    QHash<QString, QByteArray> m_relayRepositoryListEtags;
    QSet<QString> m_relayRepositoryListInFlight;
    QTabWidget *m_controlNodeTabs = nullptr;
    int m_controlCloudflareTabIndex = -1;
    // Export page: every value that would be written, plus one row per
    // top-level application-data entry. Bulk, re-fetchable trees start
    // unticked, and the choice is remembered in control/exportExcludedData.
    int m_controlExportTabIndex = -1;
    QTableWidget *m_exportValuesTable = nullptr;
    QTableWidget *m_exportDataTable = nullptr;
    QLabel *m_exportSummary = nullptr;
    QLabel *m_exportDataStatus = nullptr;
    QCheckBox *m_exportRevealSecrets = nullptr;
    QPushButton *m_exportRunButton = nullptr;
    QList<forkmesh::control::CompleteSystemDataEntry> m_exportDataEntries;
    QSet<QString> m_exportExcludedData;    // effective: what the export skips
    QSet<QString> m_exportForcedIncluded;  // bulk entries ticked on by hand
    QSet<QString> m_exportForcedExcluded;  // ordinary entries ticked off
    bool m_exportSurveyRunning = false;
    bool m_exportSurveyDone = false;
    int m_exportSurveyEpoch = 0;
    bool m_exportApplyingDataTable = false;
    // Import page: blank until a bundle is chosen, then every field is filled
    // in from it and Deploy becomes available.
    int m_controlImportTabIndex = -1;
    QJsonObject m_importedSystemBundle;
    QLabel *m_importBundlePathLabel = nullptr;
    QLabel *m_importSummaryLabel = nullptr;
    QLabel *m_importReviewLabel = nullptr;
    QTableWidget *m_importValuesTable = nullptr;
    QLineEdit *m_importVultrKeyEdit = nullptr;
    QLineEdit *m_importCloudflareTokenEdit = nullptr;
    QLineEdit *m_importNodeNameEdit = nullptr;
    QLineEdit *m_importZoneEdit = nullptr;
    QLineEdit *m_importAccountEdit = nullptr;
    QLineEdit *m_importRelayHostEdit = nullptr;
    QLineEdit *m_importMirrorHostEdit = nullptr;
    QCheckBox *m_importAgentClisCheck = nullptr;
    QCheckBox *m_importApplySettingsCheck = nullptr;
    QPushButton *m_importDeployButton = nullptr;
    QLabel *m_controlNodeStatus = nullptr;
    QLabel *m_controlNodeHealth = nullptr;
    QLabel *m_controlIdentityStatus = nullptr;
    QLabel *m_controlWalletStatus = nullptr;
    QTableWidget *m_controlPermissionsTable = nullptr;
    bool m_controlRefreshingPermissions = false;
    QLineEdit *m_controlWalletEdit = nullptr;
    QLineEdit *m_cloudflareHostnameEdit = nullptr;
    QLineEdit *m_cloudflareMirrorHostnameEdit = nullptr;
    QLineEdit *m_cloudflareZoneEdit = nullptr;
    QLineEdit *m_cloudflareAccountEdit = nullptr;
    QLineEdit *m_cloudflareNodeNameEdit = nullptr;
    QLineEdit *m_cloudflareRelayLabelEdit = nullptr;
    QLineEdit *m_cloudflareMainRelayEdit = nullptr;
    QLineEdit *m_cloudflareTokenEdit = nullptr;
    QLineEdit *m_cloudflareVpsHostEdit = nullptr;
    QLineEdit *m_cloudflareVpsUserEdit = nullptr;
    QLineEdit *m_cloudflareVpsPasswordEdit = nullptr;
    QCheckBox *m_cloudflareInstallVpsCheck = nullptr;
    QCheckBox *m_cloudflareConnectCheck = nullptr;
    QPushButton *m_cloudflareDryRunButton = nullptr;
    QPushButton *m_cloudflareDeployButton = nullptr;
    QPushButton *m_cloudflareCancelButton = nullptr;
    QPlainTextEdit *m_controlNodeOutput = nullptr;
    // API token tab. m_controlTokenSecret is the same kind of short-lived
    // redaction copy as m_cloudflareActiveSecret: it exists so this tab's own
    // output can never echo the credential, and is cleared when a check ends.
    QLineEdit *m_controlTokenEdit = nullptr;
    QLabel *m_controlTokenStatus = nullptr;
    QLabel *m_controlTokenTargets = nullptr;
    QTableWidget *m_controlTokenTable = nullptr;
    QPushButton *m_controlTokenTestButton = nullptr;
    QPushButton *m_controlTokenGenerateButton = nullptr;
    QPlainTextEdit *m_controlTokenOutput = nullptr;
    QString m_controlTokenSecret;
    QStringList m_controlTokenGrantedGroups;
    QHash<QString, QString> m_controlTokenProbeResults; // requirement key -> live check
    QString m_controlTokenAccountId;
    QString m_controlTokenZoneId;
    QString m_controlTokenUserResource;
    bool m_controlTokenPolicyReadable = false;
    bool m_controlTokenBusy = false;
    QPushButton *m_siteDeployButton = nullptr;
    QList<QPushButton *> m_siteDeployTargetButtons;
    // Armed by the operator for one run: the next deployment passes --force,
    // which deploys the target even when deploy.sh found no input changes.
    QPushButton *m_siteDeployForceButton = nullptr;
    bool m_siteDeployForced = false;
    QPushButton *m_siteDeployCancelButton = nullptr;
    QLabel *m_siteDeployStatus = nullptr;
    QString m_siteDeployReadyStatus;
    BusySpinner *m_siteDeploySpinner = nullptr;
    QPlainTextEdit *m_siteDeployOutput = nullptr;
    QProcess *m_siteDeployProcess = nullptr;
    QString m_siteDeployTarget;
    bool m_siteDeployActive = false;
    QPushButton *m_nodeDeployButton = nullptr;
    QLabel *m_nodeDeployStatus = nullptr;
    BusySpinner *m_nodeDeploySpinner = nullptr;
    QPlainTextEdit *m_nodeDeployOutput = nullptr;
    QProcess *m_nodeDeployProcess = nullptr;
    bool m_nodeDeployActive = false;
    QProcess *m_cloudflareBootstrapProcess = nullptr;
    QProcess *m_cloudflareTunnelBootstrapProcess = nullptr;
    QString m_cloudflareActiveSecret;
    QString m_cloudflareDeployHostname;
    bool m_cloudflareConnectAfterDeploy = false;
    bool m_cloudflareInstallVpsAfterDeploy = false;
    QTimer *m_controlNodeRefreshTimer = nullptr;
    // Full encrypted-archive authentication hashes hundreds of megabytes for a
    // large mirror. Keep it off the GUI thread and let the Control page render
    // the most recent completed snapshot.
    QHash<QString, bool> m_controlMirrorReadyCache;
    bool m_controlMirrorProbeInFlight = false;
    qint64 m_controlMirrorProbeCompletedAtMs = 0;
    QString m_controlPermissionsSignature;
    QLabel *m_rewardPoolVaultStatus = nullptr;
    QLabel *m_rewardPoolAddress = nullptr;
    QLabel *m_rewardPoolStatus = nullptr;
    QLineEdit *m_rewardPoolRpcEdit = nullptr;
    QComboBox *m_rewardPoolNetworkCombo = nullptr;
    QTableWidget *m_rewardPoolIntentsTable = nullptr;
    QTableWidget *m_contributorRewardsTable = nullptr;
    QPushButton *m_contributorRewardsFetchButton = nullptr;
    QPushButton *m_contributorRewardsPayButton = nullptr;
    QLabel *m_contributorRewardsStatus = nullptr;
    QHash<QString, QJsonObject> m_contributorRewards;
    QPushButton *m_contributorAutoPayoutButton = nullptr;
    QLabel *m_contributorAutoPayoutStatus = nullptr;
    // The wallet passphrase exists only for an explicitly armed app session;
    // public receipt and daily-budget state may be persisted, but this value
    // never is. It is wiped on disarm and teardown.
    QString m_contributorAutoPayoutPassphrase;
    // The public address verified while arming is also fixed for the session;
    // replacing or unlinking the desktop vault disarms the workflow.
    QString m_contributorAutoPayoutWallet;
    // Session authorization is bound to the repository in which the owner
    // armed it, so another checkout cannot activate a repository-authored CFO
    // configuration against the same unlocked wallet.
    QString m_contributorAutoPayoutScope;
    QDate m_contributorAutoPayoutDay;
    quint64 m_contributorAutoPaidLamports = 0;
    bool m_contributorPayoutRunActive = false;
    QString m_contributorPayoutTeammateId;
    QString m_contributorPayoutTeammatesDirectory;
    QString m_contributorPayoutRuntimePrefix;
    QString m_contributorPayoutRepository;
    QString m_contributorPayoutTrigger;
    QString m_contributorPayoutCurrentRewardId;
    quint64 m_contributorPayoutCurrentLamports = 0;
    QUrl m_contributorPayoutReceiptEndpoint;
    int m_contributorPayoutFetchAttempts = 0;
    bool m_contributorPayoutWaitingForStartupAuth = false;
    // The deferred-notice queue sends one at a time: several sign-ins and a
    // reward-pool run can all ask for a flush within the same second.
    bool m_settledNoticeInFlight = false;
    QPushButton *m_rewardPoolFetchButton = nullptr;
    QPushButton *m_rewardPoolSignButton = nullptr;
    QPushButton *m_rewardPoolReconcileButton = nullptr;
    QHash<QString, QJsonObject> m_rewardPoolIntents;
    bool m_rewardPoolBusy = false;
    QTimer *m_rewardPoolFinalizeTimer = nullptr;
    QPushButton *m_microjobAutoPayoutButton = nullptr;
    QLabel *m_microjobAutoPayoutStatus = nullptr;
    // Held only while the app runs and only after the owner explicitly armed
    // auto-payouts. Never written to QSettings, a file, or a log.
    QString m_microjobAutoPayoutPassphrase;
    quint64 m_microjobAutoPaidLamports = 0;
    QDate m_microjobAutoPayoutDay;
    QLineEdit *m_hostIpEdit = nullptr;
    QLineEdit *m_hostUserEdit = nullptr;
    QLineEdit *m_hostPassEdit = nullptr;
    QLineEdit *m_hostNameEdit = nullptr;
    QHash<QString, QString> m_hostSessionPasswords;
    // node name (lowercased) -> ms we removed it from the Hosts page
    QJsonObject m_decommissionedNodes;
    bool m_decommissionedNodesLoaded = false;
    QCheckBox *m_hostUploadBinaryCheck = nullptr;
    QPushButton *m_hostAddButton = nullptr;
    QPushButton *m_hostInstallButton = nullptr;
    QPushButton *m_hostInstallAllButton = nullptr;
    QPushButton *m_hostReinstallAllButton = nullptr;
    QPushButton *m_hostUpdateAllSourceButton = nullptr;
    QLabel *m_hostInstallStatus = nullptr;
    QPlainTextEdit *m_hostInstallLog = nullptr;
    QString m_hostInstallLogCarry;
    int m_hostInstallLogFg = -1;
    bool m_hostInstallLogBold = false;
    QString m_hostInstallRawTail;
    QString m_hostInstallAttemptBanner;
    QString m_hostInstallLastFailure;
    QTableWidget *m_hostsTable = nullptr;
    QLabel *m_hostsFleetSummary = nullptr;
    QTableWidget *m_fleetNodesTable = nullptr;
    QPushButton *m_addCoreNodeButton = nullptr;
    forkmesh::fleet::AddCoreNodeController *m_addCoreNodeController = nullptr;
    QPointer<QDialog> m_addCoreNodeDialog;
    forkmesh::fleet::RemoveCoreNodeController *m_removeCoreNodeController = nullptr;
    QPointer<QDialog> m_removeCoreNodeDialog;
    QTableWidget *m_runnersTable = nullptr;
    QLabel *m_runnersStatus = nullptr;
    QList<forkmesh::runners::RunnerInfo> m_runners;
    bool m_runnersLoaded = false;
    bool m_runnersFetchInFlight = false;
    QList<std::function<void()>> m_runnersFetchWaiters;
    int m_runnersPendingRefreshes = 0;
    QHash<QString, QPointer<QDialog>> m_runnerLoginDialogs;
    QPushButton *m_runnerAttentionButton = nullptr;
    QList<forkmesh::runners::RunnerAttentionItem> m_runnerAttention;
    qint64 m_runnerAttentionFetchedAtMs = 0;
    qint64 m_runnerAttentionEndpointMissingUntilMs = 0;
    qint64 m_runnerAttentionPollMs = 300000;
    bool m_runnerAttentionInFlight = false;
    QPushButton *m_hostsProbeButton = nullptr;
    QTimer *m_hostProbeTimer = nullptr;
    QSet<QString> m_hostProbesInFlight;
    QHash<QString, QString> m_hostReachability;
    QHash<QString, QString> m_hostClaudeAvailability;
    QHash<QString, QString> m_hostCodexAvailability;
    QProcess *m_hostInstallProcess = nullptr; // running ssh install session, if any
    // True only while that session is installing a node (not uninstalling it),
    // so the Network rail lamp blinks for deployments alone.
    bool m_hostInstallDeploying = false;
    QProcess *m_hostLogProcess = nullptr;     // running ssh log-tail session, if any
    QProcess *m_hostActionsProcess = nullptr; // one-shot stdin-only Actions config
    QProcess *m_hostAgentInstallProcess = nullptr; // Claude/Codex CLI install
    QProcess *m_hostDiskProcess = nullptr;    // running ssh size-map read, if any
    QStackedWidget *m_hostsViewStack = nullptr;   // saved-host list | live view
    QPushButton *m_hostsLiveViewButton = nullptr;
    QWidget *m_hostsLivePanes = nullptr;          // row of per-host live panes
    QLabel *m_hostsLiveSummary = nullptr;
    QList<QPointer<QProcess>> m_hostsLiveProcesses; // one ssh stream per pane
    struct HostLogFeed;
    QHash<QString, QSharedPointer<HostLogFeed>> m_hostLogFeeds; // by host key
    bool m_hostLogFeedsArmed = false; // set once the startup delay has passed
    struct CoreLogFeed;
    QHash<QString, QSharedPointer<CoreLogFeed>> m_coreLogFeeds;
    QQueue<QPair<QString, QByteArray>> m_coreLogPending;
    bool m_coreLogDrainScheduled = false;
    bool m_coreLogFeedsArmed = false;
    QString m_coreLogIdleReason;
    forkmesh::corelogs::RepeatFilter m_coreLogRepeats;
    forkmesh::deploylogs::DeployLogFollower *m_deployLogFollower = nullptr;
    forkmesh::corelogs::ActivityTally m_coreLogTally;
    forkmesh::corelogs::TracebackAssembler m_coreLogTracebacks;
    forkmesh::corelogs::NodeLogFilter m_nodeLogFilter;
    QHash<QString, QString> m_coreLogCursors;
    QHash<QString, QSet<QString>> m_coreLogUnitsSeen;
    QSet<QString> m_coreLogAccessNodes;
    QSharedPointer<const forkmesh::ipcountry::Table> m_ipCountryTable;
    qint64 m_ipCountryCheckedMs = 0;
    bool m_ipCountryBusy = false;
    QHash<QString, forkmesh::ipwhois::Info> m_accessWhois;
    QStringList m_accessWhoisQueue;
    QSet<QString> m_accessWhoisPending;
    int m_accessWhoisRunning = 0;
    QList<forkmesh::corelogs::CoreNode> m_vultrFleetNodes;
    QFileSystemWatcher *m_fleetInventoryWatcher = nullptr;
    QTimer *m_fleetInventoryTimer = nullptr;
    QPushButton *m_nodeLogFilterButton = nullptr;
    qint64 m_coreLogCursorsSavedMs = 0;
    bool m_coreLogCursorsLoaded = false;
    bool m_coreLogCursorsDirty = false;
    bool m_coreLogCursorSaveArmed = false;
    QFuture<bool> m_coreLogCursorSaveFuture;
    bool m_coreLogTracebackTimerArmed = false;
    bool m_fleetInventoryInFlight = false;
    qint64 m_fleetInventoryFetchedMs = 0;
    qint64 m_fleetInventoryRetryAtMs = 0;
    int m_fleetInventoryFailures = 0;
    QTimer *m_coreLogD1RefreshTimer = nullptr;
    QCheckBox *m_coreLogFeedSettingCheck = nullptr;
    QLineEdit *m_vultrApiKeyEdit = nullptr;
    QString m_vultrRememberedKey;
    QLineEdit *m_vultrNameEdit = nullptr;
    QCheckBox *m_vultrAgentClisCheck = nullptr;
    QPushButton *m_vultrCreateButton = nullptr;
    QPushButton *m_vultrEndDeploymentButton = nullptr;
    QLabel *m_vultrStatus = nullptr;
    QWidget *m_vultrProgressPanel = nullptr;
    QList<QLabel *> m_vultrStageNumbers;
    QList<QLabel *> m_vultrStageLabels;
    QList<QLabel *> m_vultrStageDetails;
    QList<QString> m_vultrStageDetailText;
    QLabel *m_vultrLiveBadge = nullptr;
    bool m_vultrProvisionActive = false;
    bool m_vultrResumeRequested = false;
    bool m_vultrResumeChain = false;
    bool m_vultrLogSaveScheduled = false;
    int m_vultrProvisionStage = 0;
    QString m_vultrProvisionState;
    QString m_vultrProvisionDetail;
    QString m_vultrProvisionMessage;
    QString m_vultrProvisionNode;
    // Which part of the public-traffic contract the verifying node has not
    // cleared yet, carried between polls so stage 6 says why it is waiting.
    QString m_vultrPublicationGap;
    QString m_vultrInstanceId;
    QString m_vultrInstanceIp;
    QString m_vultrIdentityFile;
    QPointer<QProcess> m_vultrSeedProcess;
    int m_vultrPollCount = 0;        // instance boot polls used this run
    int m_vultrInstallAttempts = 0;  // SSH install attempts used this run
    int m_vultrSshWaitCount = 0;     // SSH reachability probes used this run
    QProcess *m_vultrSshProbeProcess = nullptr;
    bool m_vultrInstallUseLocalBinary = false;
    bool m_vultrInstallAgentClis = false;
    QString m_vultrDnsHostname;      // Cloudflare name provisioned this run
    // Session-only Cloudflare credential for the fresh node's Tunnel
    // bootstrap. It is sent as the first SSH stdin line, never argv/logged,
    // and scrubbed as soon as provisioning reaches a terminal state.
    QString m_vultrTunnelApiToken;
    // Non-secret billing/provenance facts captured from Vultr's selected plan
    // and created instance. These are persisted with the saved Host row so an
    // operator can identify the plan and expected monthly cost later.
    QJsonObject m_vultrHostMetadata;
    // A plan and region the owner's phone picked for the next create; the
    // plan step consumes them (chooseVultrPlan), empty means the defaults.
    QString m_vultrRequestedPlan;
    QString m_vultrRequestedRegion;
    QStringList m_vultrInstallAttemptLog;
    QString m_hostInstallLinkTail;
    bool m_hostLinkPrompted = false;
    QString m_hostRunnerToken;
    QString m_hostRunnerName;
    QString m_hostRunnerApp;

    QLabel *m_hostDeployLabel = nullptr;      // "Live output — per host" header
    QWidget *m_hostDeployPanel = nullptr;     // container holding the per-host panes
    QGridLayout *m_hostDeployGrid = nullptr;  // lays the panes out roughly square
    QList<HostDeploySession *> m_hostDeploySessions;
    int m_hostDeployRemaining = 0;            // sessions still running
    int m_hostDeployFailed = 0;               // sessions that finished with an error
    // Non-empty only for an artifact rollout requested by the release dialog.
    // Its durable intent is consumed after every active host succeeds, so an
    // interrupted or partially failed rollout resumes on the next launch.
    QString m_hostDeployReleaseOwner;
    QString m_hostDeployReleaseName;
    QString m_hostDeployReleaseTag;
    bool m_hostDeployReleaseRefsReady = true;

    int m_relayProbesInFlight = 0;          // outstanding /api/version probes
    QHash<QString, forkmesh::nodedir::RelayProbe> m_relayProbes;
    QString m_relayProbesUpdatedAt;
    QTableWidget *m_nodesTable = nullptr;
    QLabel *m_nodesStatus = nullptr;            // "N nodes · M online" summary line
    // Keep deletion progress visible while ordinary roster refreshes arrive.
    // Transient names suppress older in-flight responses immediately; the
    // timestamped ledger handles later refreshes and survives a relaunch.
    bool m_nodeDeleteInProgress = false;
    QString m_nodeDeleteTarget;
    QSet<QString> m_deletedMeshNodeNames;
    QJsonObject m_deletedMeshNodes;
    bool m_deletedMeshNodesLoaded = false;
    // Node names (lowercased) the relay currently reports online — an update
    // channel or a fresh signed heartbeat. Merged into the Nodes page's status so
    // headless mirror nodes that serve via the relay (but never join this
    // client's chat room) show online instead of permanently offline.
    QSet<QString> m_relayOnlineNodes;
    qint64 m_relayOnlineNodesFetchedMs = 0; // throttle between relay fetches
    // True once /api/network/stats has answered at least once, so the Nodes page
    // knows the relay's authoritative live set is available. Before the first
    // reply we fall back to the encrypted roster's presence flag; after it, the
    // relay is trusted over a possibly-stale roster entry.
    bool m_relayOnlineNodesFetched = false;
    QHash<QString, QJsonObject> m_nodesCatalogInfo;
    qint64 m_nodesCatalogFetchedMs = 0; // throttle between catalog sweeps
    struct FirewallHistoryEntry {
        QString method;
        QString url;
        QString destination;
        QString rule;
        bool allowed = false;
        qint64 timestampMs = 0;
    };
    QCheckBox *m_requestFirewallEnabledCheck = nullptr;
    QLabel *m_requestFirewallStatus = nullptr;
    QLineEdit *m_requestFirewallRuleEdit = nullptr;
    QTableWidget *m_requestFirewallRulesTable = nullptr;
    QTableWidget *m_requestFirewallHistoryTable = nullptr;
    QPushButton *m_requestFirewallRemoveButton = nullptr;
    QList<FirewallHistoryEntry> m_requestFirewallHistory;
    QLabel *m_networkDiagnosticsStatus = nullptr;
    QTableWidget *m_networkEndpointsTable = nullptr;
    QTableWidget *m_networkDiagnosticsTable = nullptr;
    QPushButton *m_networkDiagnosticsRefreshButton = nullptr;
    bool m_networkEndpointFadeScheduled = false;
    bool m_networkEndpointsUserSorted = false;
    QWidget *m_networkWebRequestsChart = nullptr;
    QTableWidget *m_networkWebRequestsTable = nullptr;
    QLabel *m_networkWebRequestsStatus = nullptr;
    QComboBox *m_networkWebRequestsRange = nullptr;
    int m_networkWebRequestsTabIndex = -1;
    int m_networkFirewallRulesTabIndex = -1;
    int m_networkWebRequestsMinutes = 60;
    bool m_networkWebRequestsUserSorted = false;
    // Coalesces redraws while tail hits stream in and the tab is showing.
    QTimer *m_networkWebRequestsRefreshTimer = nullptr;
    QTabWidget *m_networkTabs = nullptr;
    static constexpr int kNetworkNodesTab = 0;
    static constexpr int kNetworkHostsTab = 1;
    static constexpr int kRetiredMirrorNodesDetailTab = 13;
    int m_networkRelayCount = 0;
    int m_networkNodeCount = 0;
    int m_networkHostCount = 0;
    QTabWidget *m_usersTabs = nullptr;
    QTableWidget *m_usersTable = nullptr;
    QTableWidget *m_usersQuarantineTable = nullptr;
    QWidget *m_usersSignupReferralCodePanel = nullptr;
    QLabel *m_usersSignupReferralCodeStatus = nullptr;
    QLineEdit *m_usersSignupReferralCodeEdit = nullptr;
    QPushButton *m_usersSignupReferralCodeCopyButton = nullptr;
    QString m_usersSignupReferralCode;
    QLabel *m_usersStatus = nullptr;
    QPushButton *m_usersRefreshButton = nullptr;
    QPushButton *m_usersTipButton = nullptr;
    struct UsersModerationBatch {
        QStringList targets;
        QStringList succeeded;
        QStringList failures;
        int nextIndex = 0;
        int lastRemaining = -1;
    };
    QPushButton *m_usersQuarantineButton = nullptr;
    bool m_usersQuarantineInFlight = false;
    UsersModerationBatch m_usersQuarantineBatch;
    QPushButton *m_usersUnquarantineButton = nullptr;
    bool m_usersUnquarantineInFlight = false;
    QPushButton *m_usersDeleteButton = nullptr;
    bool m_usersDeleteInFlight = false;
    UsersModerationBatch m_usersDeleteBatch;
    QJsonArray m_usersDirectoryPayload;
    // The public roster returned beside an administrator's complete users
    // projection. Moderation can remove a row locally without ever feeding
    // private/inactive accounts into Chat or World member presentation.
    QJsonArray m_usersPublicDirectoryPayload;
    QJsonArray m_usersQuarantinePayload;
    bool m_usersDirectoryAdminPayload = false;
    int m_repoPinCheckIndex = -1;            // repo index an in-flight pin check belongs to
    struct RepoMenuEntry {
        QString label;
        QIcon icon;
        int index = -1;     // m_repositories index; -2 = advertised mirror
        QString advertised; // ownerName when index == -2
        QString detail;     // tooltip explaining what distinguishes this entry
    };
    QList<RepoMenuEntry> m_repoMenuEntries;
    QListWidget *m_dmList = nullptr;
    QPushButton *m_discordConnectButton = nullptr;
    QWidget *m_discordSettingsCard = nullptr;
    QPushButton *m_chatMembersButton = nullptr;
    QVBoxLayout *m_chatMembersLayout = nullptr;
    QLabel *m_chatMembersHeading = nullptr;
    QWidget *m_firewallBanner = nullptr;
    QLabel *m_firewallBannerLabel = nullptr;
    QPushButton *m_firewallAllowButton = nullptr;
    QString m_firewallPrivilegedCommand;
    QString m_firewallSettingsStamp; // recorded once the command succeeds
    bool m_meshFirewallLogged = false;
    // One-line transport diagnostics under the #mesh room's header, refreshed
    // every second while that room is open.
    QWidget *m_meshDebugBar = nullptr;
    QPushButton *m_meshDebugToggleButton = nullptr;
    QLabel *m_meshDebugLabel = nullptr;
    QTimer *m_meshDebugTimer = nullptr;
    bool m_meshDebugVisible = false;
    QString m_localMeshLastNote; // the node's last system message
    QWidget *m_chatUnreadBanner = nullptr;
    QLabel *m_chatUnreadBannerLabel = nullptr;
    QPushButton *m_chatUnreadJumpButton = nullptr;
    QPushButton *m_chatAllUnreadButton = nullptr;
    QLabel *m_chatAllUnreadBadge = nullptr;
    QLabel *m_chatWorkspaceUser = nullptr;
    QLabel *m_chatWorkspaceAvatar = nullptr;
    QWidget *m_chatMembersPanel = nullptr;
    QWidget *m_chatBody = nullptr;
    bool m_chatMembersPanelWanted = true;
    QLabel *m_channelTopicLabel = nullptr;
    QScrollArea *m_messageScroll = nullptr;
    QWidget *m_messageContainer = nullptr;
    QVBoxLayout *m_messageLayout = nullptr; // message rows + a trailing stretch
    bool m_stickToBottom = true;
    QLabel *m_typingLabel = nullptr;
    QLineEdit *m_messageInput = nullptr;
    QCompleter *m_mentionCompleter = nullptr;
    QStringListModel *m_mentionModel = nullptr;
    QAbstractItemView *m_mentionCompleterPopup = nullptr;
    bool m_showingSlashCommands = false;

    QLineEdit *m_settingsNameEdit = nullptr;        // Username (the account)
    QLineEdit *m_settingsMachineNodeEdit = nullptr; // this machine's node name
    QLineEdit *m_settingsNodeLabelsEdit = nullptr;  // extra Actions `runs-on:` labels
    QLineEdit *m_settingsSolanaEdit = nullptr; // #66: node Solana address in Settings
    QLabel *m_settingsEmailLabel = nullptr;
    QLabel *m_settingsEmailVerifiedBadge = nullptr;
    QLabel *m_settingsAvatarPreview = nullptr;
    QPointer<QDialog> m_accountProfileDialog; // showEditAccountProfileDialog()
    QPointer<QDialog> m_userStatusDialog; // showSetUserStatusDialog()
    forkmesh::userstatus::UserStatus m_ownUserStatus;
    bool m_ownUserStatusKnown = false; // a server answer has been applied
    qint64 m_ownUserStatusWrittenMs = 0; // last local write, vs directory lag
    QTimer *m_userStatusExpiryTimer = nullptr; // next status to lapse
    QString m_rosterUserStatusSignature; // live-frame statuses last painted
    QLabel *m_identityBackupNag = nullptr; // #368: "back up your key" warning
    QTextBrowser *m_settingsLog = nullptr;
    QPointer<QDialog> m_logPopout;
    QPointer<QTextBrowser> m_logPopoutView;
    QPointer<QLabel> m_logPopoutStatus;
    QString m_logPopoutDate;         // last day divider written to the pop-out
    bool m_logPopoutFilling = false; // history render in progress
    QStringList m_logPopoutPending;  // lines logged while it was filling
    quint64 m_logPopoutFillGeneration = 0;
    QStringList m_logPopoutFillHistory;
    int m_logPopoutFillCursor = 0;
    QString m_logPopoutFillDate;
    LogTimelineChart *m_logTimelineChart = nullptr;
    QTabWidget *m_logViewTabs = nullptr;
    QWidget *m_logFrequencyChart = nullptr;
    QLabel *m_logFrequencySummary = nullptr;
    QTimer *m_logFrequencyRefreshTimer = nullptr;
    int m_logAggregationTabIndex = -1;
    QTextBrowser *m_logUsageView = nullptr;
    QTimer *m_logUsageRefreshTimer = nullptr;
    int m_logUsageTabIndex = -1;
    // Account-wide usage from /api/accounts/admin-usage-report, or the {ok:
    // false, error} body explaining why there is none. Empty until the first
    // reply; the live tally below renders regardless.
    QJsonObject m_logUsageReport;
    bool m_logUsageFetchInFlight = false;
    qint64 m_logUsageFetchedAtMs = 0;
    QTextBrowser *m_logD1UsageView = nullptr;
    int m_logD1UsageTabIndex = -1;
    // The rows behind the D1 numbers: table browser, audited row CRUD and
    // the #682 audit findings, over /api/accounts/admin-database.
    forkmesh::DatabaseManagerWidget *m_databaseManager = nullptr;
    int m_logDatabaseTabIndex = -1;
    forkmesh::FleetVisualizer *m_fleetVisualizer = nullptr;
    int m_logFleetVisualizerTabIndex = -1;
    forkmesh::QueryInspector *m_queryInspector = nullptr;
    forkmesh::DesktopProcessView *m_desktopProcessView = nullptr;
    int m_logDesktopResourceTabIndex = -1;
    // D1's account-wide daily row metrics and direct bound-database storage
    // measurement from /api/accounts/admin-d1-usage-report.
    QJsonObject m_logD1UsageReport;
    bool m_logD1UsageFetchInFlight = false;
    qint64 m_logD1UsageFetchedAtMs = 0;
    // Every SQL statement in the Worker source of this checkout, so a ranked
    // provider query can name the place that issues it. Built once, off the
    // GUI thread, and dropped when the tab is refreshed by hand.
    QList<forkmesh::D1QueryStatement> m_d1QuerySourceIndex;
    bool m_d1QuerySourceIndexReady = false;
    bool m_d1QuerySourceIndexBuilding = false;
    // Live tail DB lines resolved against that index, keyed by the Worker's
    // origin file and the statement; cleared whenever the index is rebuilt.
    QHash<QString, forkmesh::D1QuerySite> m_d1LiveQuerySites;
    QTextBrowser *m_logWorkerFootprintView = nullptr;
    int m_logWorkerFootprintTabIndex = -1;
    // Public, non-secret source/asset measurements emitted into the static
    // deploy stage only after the same growth budgets have passed.
    QJsonObject m_logWorkerFootprintReport;
    bool m_logWorkerFootprintFetchInFlight = false;
    qint64 m_logWorkerFootprintFetchedAtMs = 0;
    QTextBrowser *m_logCronView = nullptr;
    int m_logCronTabIndex = -1;
    // The maintenance schedule, the retained tick history and the cron
    // settings from /api/accounts/admin-cron-report, or the {ok: false,
    // error} body saying why there is none. Empty until the first reply.
    QJsonObject m_logCronReport;
    bool m_logCronFetchInFlight = false;
    qint64 m_logCronFetchedAtMs = 0;
    forkmesh::control::CloudUsageTally m_cloudUsageTally;
    // Per-route /api statistics for the Network page's Web Requests tab.
    forkmesh::control::WebRequestTally m_webRequestTally;
    QLabel *m_logTimelineSummary = nullptr;
    QPushButton *m_logTimelineResetZoom = nullptr;
    QButtonGroup *m_logTimelineRangeGroup = nullptr;
    QButtonGroup *m_logFrequencyRangeGroup = nullptr;
    int m_logTimelinePresetMinutes = 24 * 60; // 0 means a custom range
    qint64 m_logTimelineCustomFromMs = 0;
    qint64 m_logTimelineCustomToMs = 0;
    // The summary's two formatted endpoints, kept while the view range holds.
    qint64 m_logTimelineSummaryFromMs = 0;
    qint64 m_logTimelineSummaryToMs = 0;
    QString m_logTimelineSummaryFormat;
    QString m_logTimelineSummaryFromText;
    QString m_logTimelineSummaryToText;
    QHBoxLayout *m_logFilterRow = nullptr;    // chip row above the network log
    QButtonGroup *m_logFilterGroup = nullptr; // non-exclusive category chips
    // An empty set is the All view. Otherwise the detail log and timeline show
    // the union of the chosen activity categories.
    QSet<QString> m_logFilters;
    // Categories switched off in the Log types dialog. Separate from the chips
    // above: those hide lines from the view, these stop the lines being
    // recorded at all. Persisted in kMutedLogCategoriesSetting.
    mutable QSet<QString> m_mutedLogCategories;
    // Lines are logged from the first moments of the constructor, well before
    // the Logs page exists, so the set loads itself on first use.
    mutable bool m_mutedLogCategoriesLoaded = false;
    mutable bool m_loggingOff = false;
    mutable bool m_loggingOffLoaded = false;
    QPushButton *m_loggingOffButton = nullptr;
    QLineEdit *m_logSearchEdit = nullptr;     // free-text filter above the log
    // Typing arms this instead of re-rendering the log per keystroke.
    QTimer *m_logSearchDebounce = nullptr;
    QString m_logTextFilter;                  // active text filter ("" = none)
    // `accounts/*` is how the Web Requests tab names a route group, so the text
    // filter reads `*` and `?` as wildcards rather than looking for them
    // literally. Compiled once per filter change, not once per log line.
    bool m_logTextFilterIsGlob = false;
    QRegularExpression m_logTextFilterRegex;
    QPushButton *m_rebuildButton = nullptr;
    QLabel *m_rebuildStatus = nullptr;
    QLineEdit *m_mirrorRootEdit = nullptr;
    QLineEdit *m_previewCacheRootEdit = nullptr;
    QTableWidget *m_dataDirTable = nullptr;
    quint64 m_dataDirScanGeneration = 0;
    QLabel *m_dataStatus = nullptr;
    QTableWidget *m_backupTable = nullptr;
    QCheckBox *m_backupEnabledCheck = nullptr;
    QSpinBox *m_backupKeepSpin = nullptr;
    QLabel *m_backupStatus = nullptr;
    QPushButton *m_backupNowButton = nullptr;
    QTimer *m_backupTimer = nullptr;     // hourly tick
    QProcess *m_backupProcess = nullptr; // the in-flight `tar` (one at a time)
    // Settings -> Data -> Export & import. The archive is the same multi-
    // gigabyte tarball the hourly snapshot writes, so both directions run
    // `tar` asynchronously; this holds the one in flight.
    QPushButton *m_exportConfigButton = nullptr;
    QPushButton *m_importConfigButton = nullptr;
    QProcess *m_configArchiveProcess = nullptr;
    QLineEdit *m_importUrlEdit = nullptr;
    QPushButton *m_importButton = nullptr;
    QLabel *m_importStatus = nullptr;
    QCheckBox *m_autostartCheck = nullptr;
    QLabel *m_autostartInfo = nullptr;
    QPushButton *m_autostartRemoveButton = nullptr;
    QComboBox *m_themeCombo = nullptr;
    forkmesh::ui::WindowBackdrop *m_windowBackdrop = nullptr;
    forkmesh::ui::WindowFade *m_windowFade = nullptr;
    QComboBox *m_defaultAgentProviderCombo = nullptr;
    QListWidget *m_composerModelVisibilityList = nullptr;
    QLineEdit *m_maxRunningAgentsEdit = nullptr;
    QLineEdit *m_codexApiKeyEdit = nullptr;
    QLineEdit *m_openAiAdminKeyEdit = nullptr;
    QLineEdit *m_codexModelEdit = nullptr;
    QLineEdit *m_claudeApiKeyEdit = nullptr;
    QLineEdit *m_claudeAdminKeyEdit = nullptr;
    QLineEdit *m_codexCommandEdit = nullptr;
    QLineEdit *m_claudeCommandEdit = nullptr;
    QLineEdit *m_agentContextEdit = nullptr;
    QLineEdit *m_agentMaxOutputEdit = nullptr;
    QPlainTextEdit *m_agentPromptPreambleEdit = nullptr;
    QPlainTextEdit *m_prioritizePromptEdit = nullptr;
    QTimer *m_mirrorSyncTimer = nullptr;
    QHash<QString, QString> m_pendingMirrorEventStates;
    QSet<QString> m_mirrorEventRetryScheduled;
    QSet<QString> m_mirrorEventHashChecks;
    QTimer *m_relaySyncDebounce = nullptr;
    bool m_relaySyncSupported = true;
    bool m_relaySyncInFlight = false;
    QSet<QString> m_automaticIssueSyncChecksInFlight;
    QHash<QString, AutomaticIssueSyncRequest> m_automaticIssueSyncPending;
    QTimer *m_autoUpdateTimer = nullptr; // periodic check for maybeAutoUpdate()
    bool m_autoUpdateChecking = false;   // a background "git fetch" check is in flight
    bool m_closingDown = false;
    QSet<int> m_agentExitsSettling;

    QLineEdit *m_issueSearch = nullptr;
    QComboBox *m_issuesRepoCombo = nullptr;
    QComboBox *m_issueStatusFilter = nullptr;
    QComboBox *m_issueLabelFilter = nullptr;
    QComboBox *m_issueMilestoneFilter = nullptr;
    QTableWidget *m_issueTable = nullptr;
    QStackedWidget *m_issueListStack = nullptr;
    QTableWidget *m_issueMilestonesTable = nullptr;
    QTableWidget *m_issueLabelsTable = nullptr;
    QWidget *m_issueBoard = nullptr;            // Kanban board (list stack index 3)
    QHBoxLayout *m_issueBoardColumns = nullptr; // holds one widget per board column
    QWidget *m_issueDetail = nullptr;          // collapsible detail panel
    QStackedWidget *m_issueDetailStack = nullptr;
    QWidget *m_issueComposePage = nullptr;
    QPushButton *m_issueDetailToggle = nullptr;
    QPlainTextEdit *m_issueQuickAdd = nullptr;
    QFrame *m_promptWrapper = nullptr; // geometry anchor for notification/prompt bubbles
    QLabel *m_quickAddCharCount = nullptr; // characters left in the title (max 16000)
    QComboBox *m_quickAddAgentProvider = nullptr;
    QComboBox *m_quickAddAgentModelSelector = nullptr;
    QComboBox *m_quickAddTaskAssignee = nullptr;
    QComboBox *m_quickAddClaudeModel = nullptr;
    // Permission-mode chooser: Ask before edits/Edit automatically/
    // Plan mode/Auto mode, styled like the provider/model combos beside it and
    // backed by the same kClaudeAutoModeSetting as the agent composer's toggle.
    QComboBox *m_quickAddModeSelector = nullptr;
    QComboBox *m_quickAddSpeedSelector = nullptr;
    QCheckBox *m_quickAddCreatePr = nullptr;    // request PR from quick-add agent
    QPushButton *m_quickAddSendToAgentButton = nullptr;
    QPushButton *m_quickAddGenieButton = nullptr;
    QPushButton *m_quickAddSendButton = nullptr;
    QPushButton *m_quickAddImageButton = nullptr;
    QStringList m_quickAddImages;               // image paths queued for next send
    QWidget *m_quickAddAttachStrip = nullptr;   // chips w/ thumbnail + "x" remove
    // Slash-actions menu: the "/" button left of the Agent checkbox
    // and its popup — a filter box over Context/Model action rows plus the
    // Claude Code CLI's slash commands. The command list is probed live from
    // `claude` (control-protocol initialize) the first time the popup opens.
    QPushButton *m_quickAddSlashButton = nullptr;
    QFrame *m_slashActionsPopup = nullptr;
    QLineEdit *m_slashActionsFilter = nullptr;
    QScrollArea *m_slashActionsScroll = nullptr;
    QWidget *m_slashActionsListHost = nullptr;
    QVBoxLayout *m_slashActionsListLayout = nullptr;
    QList<QWidget *> m_slashActionRows; // visible activatable rows, display order
    int m_slashActionSelected = -1;     // index into m_slashActionRows
    struct ClaudeSlashCommand {
        QString name;
        QString description;
        QString argumentHint;
    };
    QList<ClaudeSlashCommand> m_claudeSlashCommands;
    bool m_claudeSlashCommandsLoaded = false;
    QProcess *m_claudeSlashProbe = nullptr;
    QByteArray m_claudeSlashProbeBuf;
    QProcess *m_claudeEffortProbe = nullptr;
    QByteArray m_claudeEffortProbeBuf;
    QPushButton *m_quickAddMicButton = nullptr;
    QPlainTextEdit *m_voiceTargetEdit = nullptr;
    QPushButton *m_voiceActiveButton = nullptr;
    QString m_voiceIdlePlaceholder;
    QList<QPushButton *> m_voiceButtons;
    QProcess *m_voiceRecordProc = nullptr;
    QProcess *m_voiceTranscribeProc = nullptr;
    QTimer *m_voiceLiveTimer = nullptr;
    QString m_voiceWavPath;
    bool m_voiceRecording = false;
    int m_voiceInsertPos = -1;
    int m_voiceInsertLen = 0;
    qint64 m_voiceLastTranscribeSize = 0;
    QString m_voiceLastPreview;
    WorldSpeechBridge *m_worldSpeechBridge = nullptr;
    OfficeChannelMirror *m_officeChannelMirror = nullptr;
    LocalMeshNode *m_localMesh = nullptr;
    LocalControlServer *m_localControl = nullptr;
    forkmesh::localcontrolimages::Staging m_localControlImages;
    // Host management from the phone: one-use confirmations, the log streams
    // each phone holds open, and the newest lines per host for "detail".
    forkmesh::localcontrol::hosts::ConfirmationStore m_hostControlConfirmations;
    struct HostLogSubscription {
        int connectionId = 0;
        QString host; // lowercase
        forkmesh::localcontrol::hosts::LogStream stream;
    };
    QHash<QString, HostLogSubscription> m_hostLogSubscriptions; // by stream id
    QHash<QString, QStringList> m_hostRecentLogLines; // by lowercase host
    QTimer *m_hostLogFlushTimer = nullptr;
    // The host action a phone started and waits on, by "action:host".
    QHash<QString, int> m_hostControlRunning;
    QTimer *m_localControlAgentsTimer = nullptr;
    qint64 m_localControlAgentsRungAtMs = 0;
    struct LocalControlFollow {
        int connectionId = 0;
        int sessionId = 0;
        qint64 next = 0;
        QString status;
        bool active = false;
        bool busy = false;
        bool dirty = false;
        qint64 readAtMs = 0;
        qint64 seenSize = -1;
        forkmesh::localcontrol::LogTail tail;
    };
    QList<LocalControlFollow> m_localControlFollows;
    QHash<QString, forkmesh::localcontrol::terminals::Terminal> m_localControlTerminals;
    QTimer *m_localControlTerminalTimer = nullptr;
    QTimer *m_localControlFollowTimer = nullptr;
    QTimer *m_localControlFollowWatchTimer = nullptr;
    struct OutsideTranscriptTail {
        QString path;
        QString kind;
        qint64 offset = -1;
        qint64 seenSize = 0;
        qint64 base = 0;
        QString log;
    };
    QHash<int, OutsideTranscriptTail> m_localControlOutsideTails;
    QHash<int, QPointer<QWidget>> m_localControlPrompts;
    QLabel *m_localControlStatus = nullptr;
    QListWidget *m_localControlList = nullptr;
    QPushButton *m_localControlSendButton = nullptr;
    QPushButton *m_localControlForgetButton = nullptr;
    DiscordClient *m_discordClient = nullptr;
    bool m_discordTranscriptRefreshPending = false;
    bool m_discordPackageActive = false; // Discord package contributions attached
    QSet<QString> m_hiddenPackageSurfaces;
    bool m_packageSurfacesSynced = false;
    std::map<QString,
             std::unique_ptr<forkmesh::market::DesktopPackageExtension>>
        m_desktopPackageExtensions;
    QHash<QString, QWidget *> m_desktopPackageLogPages;
    QTimer *m_chatSidebarWidthTimer = nullptr;
    int m_pendingChatSidebarWidth = 0;
    // A joining node is handed the room's backlog a message at a time, and a
    // replayed thread reply or an out-of-order arrival each cost a full
    // transcript teardown. Collapse a burst of those into one rebuild.
    bool m_transcriptRebuildPending = false;
    bool m_transcriptRebuildStickToBottom = false;
    QLineEdit *m_worldSpeechOriginEdit = nullptr;
    QLineEdit *m_worldSpeechPairCodeEdit = nullptr;
    QLabel *m_worldSpeechStatusLabel = nullptr;
    QPushButton *m_worldSpeechPairButton = nullptr;
    QPushButton *m_worldSpeechRevokeButton = nullptr;
    QPlainTextEdit *m_worldSpeechDraftEdit = nullptr;
    QPushButton *m_worldSpeechHiddenMicButton = nullptr;
    QString m_worldSpeechCaptureId;
    bool m_worldSpeechCancelPending = false;
    QProgressBar *m_voiceLevelMeter = nullptr;
    QTimer *m_voiceLevelTimer = nullptr;
    qint64 m_voiceLevelPos = 0;
    QWidget *m_voiceTranscribeSpinner = nullptr;
    QProcess *m_whisperInstallProc = nullptr;
    QLabel *m_whisperStatusLabel = nullptr;      // Settings install-status line
    QPushButton *m_whisperInstallButton = nullptr;
    QComboBox *m_whisperModelCombo = nullptr;
    QComboBox *m_voiceEngineCombo = nullptr;
    QComboBox *m_parakeetModelCombo = nullptr;
    QProcess *m_parakeetInstallProc = nullptr;
    QComboBox *m_voiceDeviceCombo = nullptr;
    SettingsSectionsView *m_settingsSections = nullptr;
    int m_voiceSettingsTabIndex = -1;
    int m_pingsSettingsTabIndex = -1;
    PingSettingsPanel *m_pingSettingsPanel = nullptr;
    // The administrator card on Settings > Pings, shown only to admins.
    QWidget *m_pingAdminCard = nullptr;
    int m_suggestedAppsSettingsTabIndex = -1;
    QLabel *m_suggestedAppsSummaryLabel = nullptr;
    QHash<QString, QLabel *> m_suggestedAppStatusLabels;
    QHash<QString, QPushButton *> m_suggestedAppTestButtons;
    QHash<QString, QPushButton *> m_suggestedAppInstallButtons;
    QHash<QString, QPointer<QProcess>> m_suggestedAppTestProcesses;
    QProcess *m_suggestedAppInstallProcess = nullptr;
    QString m_suggestedAppInstallingId;
    QWidget *m_settingsProfileHost = nullptr;
    int m_profileSettingsTabIndex = -1;
    QPushButton *m_voiceTestMicButton = nullptr;
    QProgressBar *m_voiceTestMeter = nullptr;
    QProcess *m_voiceTestProc = nullptr;
    QTimer *m_voiceTestTimer = nullptr;
    QString m_voiceTestWavPath;
    qint64 m_voiceTestPos = 0;
    bool m_voiceTestRecording = false;
    QStringList m_quickAddHistory;
    int m_quickAddHistoryIndex = -1;
    QString m_quickAddDraft;
    bool m_quickAddHistoryNavigating = false;
    // The composer's typed text as of the last edit, waiting for
    // m_quickAddDraftSaveTimer to write it to PromptDraftStore.
    QString m_quickAddPendingDraft;
    bool m_quickAddDraftDirty = false;
    QTimer *m_quickAddDraftSaveTimer = nullptr;
    QLabel *m_footerGitIdentity = nullptr;
    QLabel *m_footerWorktreeInfo = nullptr;
    QLabel *m_footerCommitInfo = nullptr;
    QLabel *m_statusAppPath = nullptr;
    QWidget *m_resourceChart = nullptr;
    // The debug strip is updated by the existing one-second diagnostics tick.
    // Keep its activity totals separate from retained log history: a chart
    // should describe work done during this launch, not a log restored from
    // disk at startup.
    QElapsedTimer m_debugSessionUptime;
    QWidget *m_debugUptimeTile = nullptr;
    QWidget *m_debugNetworkActivityTile = nullptr;
    QWidget *m_debugLogActivityTile = nullptr;
    qint64 m_debugNetworkRequestTotal = 0;
    qint64 m_debugLastNetworkRequestTotal = -1;
    qint64 m_debugLastLogActivityTotal = -1;
    double m_debugLastCpuPercent = -1.0;
    forkmesh::ui::QtRuntimeDiagnostics *m_qtDiagnostics = nullptr;
    QVector<QWidget *> m_qtDiagnosticTiles;
    QPointer<QDialog> m_qtDiagnosticsDialog;
    QPlainTextEdit *m_qtDiagnosticsReport = nullptr;
    qint64 m_qtDiagnosticsReportLastRefreshMs = 0;
    QWidget *m_repoSizeChart = nullptr;
    QWidget *m_repoLinesChart = nullptr;
    QWidget *m_repoFilesChart = nullptr;
    QToolButton *m_repoRatchetButton = nullptr;
    qint64 m_repoStatsLastRefreshMs = 0;
    StallWatchdog *m_stallWatchdog = nullptr;
    QTimer *m_diagTimer = nullptr;
    int m_stallCount = 0;
    QStringList m_stallLog;          // recent stalls, each with its backtrace
    QString m_stallLogPath;          // durable on-disk stall log
    // Teammate/log-monitor pairs already handed a repair run this session.
    // Clearing diagnostics resets these alongside the observed counts.
    QSet<QString> m_triggeredTeammateLogMonitors;
    QString m_lastStallReportSignature;
    qint64 m_lastStallReportMs = 0;
    bool m_highMemoryAlertArmed = true;
    bool m_highMemoryAutoKillPending = false;
    QSet<qint64> m_highMemoryAutoKilledPids;
    QStringList m_highMemoryAutoKilledNames;
    QPointer<QDialog> m_highMemoryDialog;
    QLabel *m_highMemoryHeading = nullptr;
    QTableWidget *m_highMemoryProcessTable = nullptr;
    QLabel *m_highMemoryProcessStatus = nullptr;
    QPointer<QProcess> m_highMemoryProcessQuery;
    QHash<qint64, QVector<double>> m_highMemoryRssHistory;
    double m_highMemoryHostPct = -1.0;
    QString m_highMemoryHostTrendArrow = QStringLiteral("→");
    qulonglong m_diagLastCpuTicks = 0;
    qint64 m_diagLastCpuMs = 0;
    qint64 m_fdPressureLastCheckMs = 0;
    bool m_fdPressureAlertArmed = true;

    int m_repoDetailIndex = -1;
    QButtonGroup *m_issueTabGroup = nullptr; // Issues / Milestones / Labels tabs
    QButtonGroup *m_repoDetailTabs = nullptr;
    QWidget *m_repoDetailChrome = nullptr;   // repo actions + repository tabs
    QWidget *m_repoFilesModeBar = nullptr;   // Code overview toolbar
    QWidget *m_repoOverviewChrome = nullptr; // branch toolbar + commit strip
    ActivityRailButton *m_railCodeButton = nullptr;
    ActivityRailButton *m_railGitButton = nullptr;
    QHBoxLayout *m_repoTabRowLayout = nullptr;
    QList<QPair<QString, QPointer<QWidget>>> m_repoTabRowTabs;
    QList<QPair<QString, QPointer<QWidget>>> m_repoTabRowActions;
    QHBoxLayout *m_repoCodeBarLayout = nullptr;
    QList<QPair<QString, QPointer<QWidget>>> m_repoCodeBarButtons;
    bool m_repoChangeBadgeRefreshQueued = false;
    QPushButton *m_repoCodeTab = nullptr;
    QString m_repoCodeSizePath; // mirror the displayed "Code (N MB)" was computed for
    QPushButton *m_repoIssuesTab = nullptr;
    QPushButton *m_repoPullsTab = nullptr;
    QPushButton *m_repoDiscussionsTab = nullptr;
    QPushButton *m_repoActionsTab = nullptr;
    QPushButton *m_repoReleasesTab = nullptr; // handle for the Releases (N) badge
    QPushButton *m_repoBranchesTab = nullptr; // handle for the Branches (N) badge
    QStackedWidget *m_repoDetailStack = nullptr;
    int m_chatStackIndex = -1; // index of the Chat page in m_repoDetailStack
    int m_insightsTabIndex = -1; // index of the Insights page
    int m_branchesTabIndex = -1; // index of the Branches page
    int m_worktreesTabIndex = -1; // index of the Worktrees page (next to Branches)
    QPushButton *m_repoWorktreesTab = nullptr; // handle for the Worktrees (N) badge
    QTableWidget *m_worktreesTable = nullptr;
    QLabel *m_worktreesSummary = nullptr;
    QLabel *m_worktreeBranchLabel = nullptr; // shows which branch the open detail is on
    QPushButton *m_worktreeMergeButton = nullptr;  // merge the selected worktree into main
    QPushButton *m_worktreeMergeDeleteAgentButton = nullptr; // merge, then delete its agent too
    QPushButton *m_worktreeUpdateButton = nullptr; // merge main into the selected worktree
    QPushButton *m_worktreeResolveButton = nullptr; // resolve a conflicted merge in the worktree
    QPushButton *m_worktreeCommitButton = nullptr; // commit the worktree's uncommitted changes
    QPushButton *m_worktreeRemoveButton = nullptr; // remove the selected worktree
    QString m_worktreeSelectedBranch;              // branch behind the open worktree detail
    QString m_worktreeSelectedPath;                // its on-disk worktree folder
    int m_releasesTabIndex = -1; // index of the Releases page
    int m_artifactsTabIndex = -1; // index of the Artifacts page
    int m_shortcutsTabIndex = -1; // index of the Shortcuts page
    int m_settingsTabIndex = -1; // index of the Settings page
    int m_projectsTabIndex = -1;
    int m_sizeMapTabIndex = -1;
    QWidget *m_sizeMapChart = nullptr;
    QLabel *m_sizeMapStatus = nullptr;
    QWidget *m_sizeMapWorkersBox = nullptr;
    QList<QLabel *> m_sizeMapWorkerLines;
    QLabel *m_sizeMapWorkerOverflow = nullptr;
    QCheckBox *m_sizeMapHideIgnored = nullptr;
    QString m_sizeMapRootOverride;
    QLabel *m_sizeMapRootLabel = nullptr;
    QPushButton *m_sizeMapResetRoot = nullptr;
    QString m_sizeMapScannedPath;
    bool m_sizeMapScanning = false;
    int m_sizeMapScanEpoch = 0;
    QPushButton *m_sizeMapElevate = nullptr;
    QPushButton *m_sizeMapStop = nullptr;
    QFutureWatcher<forkmesh::DirectorySizeScanResult> *m_sizeMapWatcher = nullptr;
    QProcess *m_sizeMapElevatedProcess = nullptr;
    QWidget *m_sizeMapVolumesBox = nullptr;
    QPushButton *m_repoProjectsTab = nullptr; // handle for the Projects (N) badge
    QLabel *m_repoVisibilityHint = nullptr; // explains the current visibility
    QTableWidget *m_branchesTable = nullptr;
    QLabel *m_branchesSummary = nullptr;
    QPushButton *m_branchPullAllButton = nullptr; // safe bulk fast-forward action
    QPushButton *m_branchDeleteMergedButton = nullptr; // "Delete merged" header action
    QPushButton *m_branchDeleteSelectedButton = nullptr;
    bool m_branchRefreshRunning = false;
    QTextBrowser *m_branchDiffView = nullptr;
    QString m_branchDiffBranch;
    int m_branchDiffPullNumber = -1;
    int m_branchScopeDiffGen = 0;
    int m_branchDiffAgentSessionId = -1;
    QHash<QString, bool> m_branchConflictCache;
    QSet<QString> m_branchConflictProbes; // branches a worker is probing right now
    QHash<QString, BranchChangeStat> m_branchChangeStatsCache;
    int m_branchChangeStatsGen = 0;
    bool m_branchChangeStatsLoading = false;
    void startBranchConflictProbes(const QString &dir, const QString &base,
                                   const QList<QPair<QString, QString>> &probes);
    QByteArray m_branchDiffLastPatch;
    QString m_branchDiffLastEmpty;
    bool m_branchDiffLastValid = false;
    QHash<QString, BranchDiffCacheEntry> m_branchDiffCache;
    QStringList m_branchDiffCacheOrder; // oldest key first, for eviction
    qint64 m_branchDiffCacheBytes = 0;
    static constexpr int kBranchDiffCacheEntries = 16;
    static constexpr qint64 kBranchDiffCacheBytes = 8 * 1024 * 1024;
    QString branchDiffCacheKey(const QString &branch, const QString &workDir) const;
    void rememberBranchDiff(const QString &key, const QByteArray &patch,
                            const QString &emptyMessage);
    void forgetBranchDiff(const QString &branch); // after the branch is written to
    void clearBranchDiffCache();
    // Pass by value because git event-loop pumping can invalidate caller storage.
    void beginBranchDiffTransition(QString branch);
    void finishBranchDiffTransition();
    bool m_branchDiffPendingFade = false;
    bool m_branchDiffPaintedFromCache = false;
    // What the range pane is showing: repo, branch, base and pull. A change of
    // any of them clears the pane at once, and bumps m_branchDiffPaneGen so a
    // page still being read for the old range (the no-changes stats, a merge
    // window) cannot paint over the new one when it lands.
    QString branchDiffRangeKey(const QString &branch) const;
    bool enterBranchDiffRange(const QString &branch);
    QString m_branchDiffRangeKey;
    int m_branchDiffPaneGen = 0;
    // Ordering for the branch list's closing re-read of the viewed branch:
    // bumped as each scope read starts and as it lands.
    quint64 m_branchDiffSeq = 0;
    quint64 m_branchScopeDiffStartSeq = 0;
    int m_branchScopeDiffStartGen = -1;
    quint64 m_branchesPanelQueuedSeq = 0;
    void startBranchesPanelLoad(quint64 diffReadSeq);
    void loadBranchesPanelAsOf(quint64 diffReadSeq);
    void deferUntilBranchDiffRead(std::function<void()> work);
    void flushBranchDiffAfterRead();
    std::function<void()> m_branchDiffAfterRead;
    int m_branchDiffAfterReadGen = 0;
    static constexpr int kBranchDiffAfterReadMaxMs = 2000;
    void prefetchBranchDiff(const QString &branch);
    QSet<QString> m_branchDiffPrefetching;
    static constexpr int kBranchDiffPrefetchDwellMs = 120;
    static constexpr int kBranchDiffPrefetchMaxInFlight = 2;
    void warmBranchDiffs(const BranchesPanelData &data);
    bool m_branchDiffWarming = false;
    QHash<QString, QString> m_branchDiffWarmTips;
    static constexpr int kBranchDiffWarmRows = 6;
    QString branchDiffViewedContext(const QString &branch) const;
    QString m_branchDiffViewedContext;
    QString m_branchDiffWorkDir;
    QFrame *m_branchDiffActiveOutline = nullptr;
    QString m_branchActiveFile; // file CHANGES currently points at
    QFrame *m_branchDiffSticky = nullptr;
    QLabel *m_branchStickyPath = nullptr;
    QLabel *m_branchStickyControls = nullptr;
    QString m_branchStickyFile; // file the sticky bar currently mirrors
    QList<QPair<int, QString>> m_branchDiffFileSpans;
    QStringList m_branchDiffFilePaths;
    QStringList m_branchDiffFileAnchors;
    QHash<QString, QString> m_branchStickyLabelHtml;
    // diffViewedScope() -> paths reopened this session; see diffExpandedViewed
    QHash<QString, QSet<QString>> m_diffExpandedViewed;
    // diffViewedScope() -> the fingerprints of the patch last rendered there,
    // so a Viewed tick can be stamped with the file it was ticked on.
    QHash<QString, QHash<QString, QString>> m_diffFileFingerprints;
    QString m_lastSourceControlDiffPath;
    QList<int> m_branchFileTops;
    QTimer *m_branchAutoViewedDebounce = nullptr;
    QWidget *m_branchDiffSearchBar = nullptr;
    QLineEdit *m_branchDiffSearchInput = nullptr;
    QLabel *m_branchDiffSearchCount = nullptr;
    QList<QTextCursor> m_branchDiffSearchMatches;
    int m_branchDiffSearchIndex = -1;
    QPushButton *m_branchSplitButton = nullptr;
    QPushButton *m_branchOpenPullButton = nullptr;
    QPushButton *m_branchOpenAgentButton = nullptr; // "Agent #N" for agent branches
    QPushButton *m_branchConflictButton = nullptr;
    QPushButton *m_branchesDeleteSelBtn = nullptr;
    QLabel *m_branchDetailLabel = nullptr;      // "<branch> -> <base> · N behind · M ahead"
    QWidget *m_branchDiffSummaryBar = nullptr;  // review progress + merge-health strip
    forkmesh::ui::SecretDiffBanner *m_branchSecretBanner = nullptr;
    QLabel *m_branchDiffSummaryLabel = nullptr; // "N of M files viewed · +A -D"
    QList<QLabel *> m_branchDiffHealthLabels;   // Merge / complexity / quality / ...
    QString m_branchDiffHealthPatch;            // patch those scores describe
    int m_branchDiffHealthBehind = -1;
    int m_branchDiffHealthAhead = -1;
    bool m_branchDiffHealthConflict = false;
    bool m_branchDiffHealthWorktreeConflict = false;
    bool m_branchDiffHealthConflictPending = false;
    QPushButton *m_branchCloseButton = nullptr;
    QWidget *m_branchPackageActions = nullptr;
    QPushButton *m_branchClearLockButton = nullptr;  // "Clear lock" (stale .git lock)
    QFileSystemWatcher *m_branchLockWatcher = nullptr; // watches m_branchLockDir
    QString m_branchLockDir;         // git dir of the browsed branch's checkout
    QStringList m_branchLockPaths;   // locks in it that "Clear lock" would remove
    QPushButton *m_branchMergeEditorButton = nullptr; // "Merge editor" (resolve by hand)
    QPushButton *m_branchPullButton = nullptr;  // "Pull <base>" into the branch
    QPushButton *m_branchFixButton = nullptr;   // "Fix with agent" (conflicts only)
    QComboBox *m_branchFixAgentCombo = nullptr;
    QComboBox *m_branchFixModelCombo = nullptr;
    QPushButton *m_branchPrButton = nullptr;    // "Create PR" from the branch
    QPushButton *m_branchQueueButton = nullptr;
    QPushButton *m_branchMergeButton = nullptr; // "Merge to main"
    QPushButton *m_branchMergeDeleteButton = nullptr;
    QPushButton *m_branchDeleteCleanupButton = nullptr;
    QTableWidget *m_releasesTable = nullptr;
    QLabel *m_releasesSummary = nullptr;
    QTableWidget *m_artifactsTable = nullptr;
    QLabel *m_artifactsSummary = nullptr;
    QWidget *m_shortcutCardsHost = nullptr; // card rows, rebuilt by loadShortcutsPanel
    QLabel *m_shortcutsSummary = nullptr;
    QPlainTextEdit *m_shortcutOutput = nullptr; // live output of the running shortcut
    QLabel *m_shortcutRunStatus = nullptr;
    QPushButton *m_shortcutStopButton = nullptr;
    QComboBox *m_shortcutRunPicker = nullptr;
    QProgressBar *m_shortcutRunProgress = nullptr;
    QLabel *m_shortcutRunEta = nullptr;
    QTimer *m_shortcutProgressTimer = nullptr;
    struct ShortcutProgress
    {
        QPointer<QProgressBar> bar;
        QPointer<QLabel> label;
        qint64 startedAtMs = 0;
        qint64 expectedMs = -1;
        int agentSessionId = 0;
    };
    QList<ShortcutProgress> m_shortcutProgress;
    QHash<int, QProcess *> m_shortcutProcesses;
    QHash<QString, int> m_shortcutRestarts;
    QList<forkmesh::localcontrol::insights::ShortcutRun> m_shortcutRuns;
    int m_shortcutRunCounter = 0;
    int m_shortcutShownRun = 0;
    QLineEdit *m_shortcutsSearch = nullptr;
    QComboBox *m_shortcutsFilter = nullptr; // everything / shortcuts / skills
    QComboBox *m_skillsScope = nullptr;
    QComboBox *m_shortcutsView = nullptr;
    QStackedWidget *m_shortcutsViewStack = nullptr;
    QTableWidget *m_shortcutsTable = nullptr;
    // kShortcutUsageSetting mirrored in memory; loaded on first read so the
    // panel can sort without re-parsing QSettings for every card.
    mutable QHash<QString, int> m_shortcutUsage;
    mutable bool m_shortcutUsageLoaded = false;
    // kShortcutRunStatsSetting and kShortcutAgentRunsSetting mirrored the same
    // way, loaded together on first read.
    mutable QHash<QString, ShortcutRunStats> m_shortcutRunStats;
    mutable QHash<int, QString> m_shortcutAgentRuns;
    QHash<int, QSet<QString>> m_agentShortcutUses;
    mutable bool m_shortcutRunStatsLoaded = false;
    QSplitter *m_teammatesSplit = nullptr;
    QWidget *m_teammateCardsHost = nullptr;
    QWidget *m_teammateDashboardSummary = nullptr;
    QLabel *m_teammateDashboardSummaryText = nullptr;
    QStackedWidget *m_teammatesPageStack = nullptr;
    QPushButton *m_teammateDashboardButton = nullptr;
    QPushButton *m_teammateBriefingButton = nullptr;
    QPushButton *m_teammateStatsButton = nullptr;
    QPushButton *m_disableTeammateSchedulesButton = nullptr;
    QHash<QString, QPushButton *> m_teammateMetricButtons;
    SkillUsageChart *m_teammateSkillUsageChart = nullptr;
    QLabel *m_teammateSkillUsageTotal = nullptr;
    QWidget *m_teammatePerformanceChart = nullptr;
    QLabel *m_teammatePerformanceTotal = nullptr;
    QLabel *m_teammatePerformanceGrade = nullptr;
    QWidget *m_teammateBriefingContentHost = nullptr;
    QString m_selectedBriefingTeammateId;
    QWidget *m_teammateDetailPanel = nullptr;
    QWidget *m_teammateDetailHost = nullptr;
    QString m_teammatesSummaryText;
    QString m_teammatesStatusFilter;
    QString m_teammatesSearchMirror;
    bool m_teammatesGridView = true;
    bool m_teammatesDashboardMode = true;
    QComboBox *m_teammatesRepoChooser = nullptr; // which repository the team works on
    // -1 until a repository is chosen (or restored from settings); the open
    // repository stands in until then.
    int m_teamRepoIndex = -1;
    bool m_teammatesRepoChooserUpdating = false;
    QString m_selectedTeammateId;
    // teammate id -> unsent card prompt, so the frequent panel rebuilds
    // (schedule ticks, activity writes, filter edits) do not eat what is
    // half-typed in a card.
    QHash<QString, QString> m_teammatePromptDrafts;
    QWidget *m_teammateHomePage = nullptr;
    QWidget *m_teammateHomeHeader = nullptr;
    QWidget *m_teammateHomeChatHost = nullptr;
    QScrollArea *m_teammateHomeChatScroll = nullptr;
    QWidget *m_teammateHomeWidgetsHost = nullptr;
    QWidget *m_teammateHomeInfoHost = nullptr;
    QWidget *m_teammateHomeSaidSection = nullptr;
    QWidget *m_teammateHomeSaidHost = nullptr;
    QLabel *m_teammateHomeSaidState = nullptr;
    QPushButton *m_teammateHomeSaidCheck = nullptr;
    QLabel *m_teammateHomeChatTitle = nullptr;
    QPushButton *m_teammateHomeBuildWidget = nullptr;
    QPlainTextEdit *m_teammateHomeComposer = nullptr;
    QPushButton *m_teammateHomeSend = nullptr;
    QPlainTextEdit *m_teammateHomeNotes = nullptr;
    QLabel *m_teammateHomeNotesState = nullptr;
    QTimer *m_teammateHomeNotesTimer = nullptr;
    QTimer *m_teammateHomeRefreshTimer = nullptr;
    QString m_teammateHomeId;
    QString m_teammateHomeNotesId;
    QString m_teammateHomeSignature;
    bool m_teammateHomeFromDashboard = true;
    QHash<int, QPair<qint64, QString>> m_teammateHomeOutputCache;
    QWidget *m_teammateRail = nullptr;
    QVBoxLayout *m_teammateRailLayout = nullptr;
    QTimer *m_teammateRailRefreshTimer = nullptr;
    QTimer *m_teammateRailClock = nullptr;
    QString m_teammateRailSignature;
    QPointer<QCheckBox> m_teammateRailCheck;
    QWidget *m_packageRail = nullptr;
    QVBoxLayout *m_packageRailLayout = nullptr;
    QTimer *m_packageRailRefreshTimer = nullptr;
    QTimer *m_packageRailClock = nullptr;
    QTimer *m_packageRailReminderTimer = nullptr;
    QString m_packageRailSignature;
    QString m_packageRailState;
    qint64 m_packageRailChangedAtMs = 0;
    QPointer<QCheckBox> m_packageRailReminderCheck;
    bool m_packageRailHasTiles = false;
    QPointer<QCheckBox> m_packageRailCheck;
    QSet<int> m_teammateCreatorSessions;
    QSet<int> m_teammateDoneUnseen;
    QHash<QString, qint64> m_agentSeenFinishedAt;
    qint64 m_agentReadBaselineMs = 0;
    // Automatic @Name hand-offs are deliberately shallow so two teammates
    // cannot bounce a reply loop forever. Direct human mentions always start
    // at depth zero.
    QHash<int, int> m_teammateMessageDepth;
    QHash<int, QString> m_lastRelayedTeammateText;
    QString m_teammateEditorDirectory;
    bool m_teammateEditorOpen = false;
    QTimer *m_teammateScheduleTimer = nullptr;
    QTableWidget *m_mirrorNodesTable = nullptr;
    QLabel *m_mirrorNodesSummary = nullptr;
    QCheckBox *m_mirrorNodesOnlineOnlyCheck = nullptr;
    QComboBox *m_mirrorNodesRepoCombo = nullptr;
    QString m_mirrorNodesRepoKey;
    QComboBox *m_nodesRoleFilter = nullptr;
    QString m_appReleaseVersion;
    QString m_appReleaseRev;
    void noteAppRelease(const QJsonObject &versionReply);
    mutable bool m_nextFleetNamesLoaded = false;
    mutable forkmesh::noderole::FleetNames m_nextFleetNames;
    QTimer *m_nodeLightTimer = nullptr;
    int m_nodeLightFrame = 0;
    QTimer *m_mirrorPanelRosterTimer = nullptr;
    QHash<QString, CommitIdentity> m_commitIdentityCache;
    QPushButton *m_mirrorResetPinButton = nullptr;
    QString m_repoBranch;
    RepoInfo m_repoInfo;
    QLabel *m_repoHeaderTitle = nullptr;
    QLabel *m_repoDetailNotice = nullptr;
    QLabel *m_repoDetailStatus = nullptr;
    QPushButton *m_notifyButton = nullptr;
    QPushButton *m_forkButton = nullptr;
    QPushButton *m_mirrorButton = nullptr;
    QPushButton *m_sourceButton = nullptr;
    QPushButton *m_repoOpenButton = nullptr; // open this repo on the web
    QMenu *m_mirrorMenu = nullptr;
    QMenu *m_sourceMenu = nullptr;
    QPushButton *m_branchButton = nullptr;
    QPushButton *m_branchesButton = nullptr;
    QPushButton *m_worktreesButton = nullptr; // "N worktrees" toggle in the Code toolbar
    QPushButton *m_remotesButton = nullptr;   // "N remotes" dropdown in the Code toolbar
    QPushButton *m_tagsButton = nullptr;
    QPushButton *m_codeMapButton = nullptr;
    QPushButton *m_filesModeOverviewButton = nullptr; // -> code overview
    QLineEdit *m_fileSearch = nullptr;
    QCompleter *m_fileCompleter = nullptr;
    QLabel *m_securitySummary = nullptr;
    QWidget *m_securitySignalsPanel = nullptr;
    QGridLayout *m_securitySignalsGrid = nullptr;
    QTableWidget *m_securityFindingsTable = nullptr;
    QPushButton *m_securityRefreshButton = nullptr;
    bool m_repoSecurityScanRunning = false;
    QString m_repoSecurityScanningPath;
    // Most recently rendered Security-tab snapshot, cached so a "Run scan" click
    // can redraw the dependency card in its busy state immediately, without
    // re-running the (possibly slow) scan just to get a frame to render.
    RepoSecuritySnapshot m_lastRepoSecuritySnapshot;
    QLabel *m_qualitySummary = nullptr;
    QWidget *m_qualitySignalsPanel = nullptr;
    QGridLayout *m_qualitySignalsGrid = nullptr;
    QTableWidget *m_qualityFindingsTable = nullptr;
    QPushButton *m_qualityRefreshButton = nullptr;
    // Private vulnerability report form (Settings → Security tab).
    QLineEdit *m_vulnTitleEdit = nullptr;
    QPlainTextEdit *m_vulnBodyEdit = nullptr;
    QLineEdit *m_vulnContactEdit = nullptr;
    QComboBox *m_vulnComponentCombo = nullptr;
    QPushButton *m_vulnSubmitButton = nullptr;
    QLabel *m_vulnStatusLabel = nullptr;
    QLabel *m_mcpStatusLabel = nullptr;
    QLineEdit *m_mcpTokenEdit = nullptr;
    QPlainTextEdit *m_mcpConfigEdit = nullptr;
    QPushButton *m_mcpGenerateButton = nullptr;
    QPushButton *m_mcpRevokeButton = nullptr;
    QPushButton *m_mcpTestButton = nullptr;
    QLabel *m_mcpTestLabel = nullptr;
    QProcess *m_mcpTestProcess = nullptr;
    // Genie: the website's remote-MCP credentials, edited in the
    // same Settings → MCP page and used by the quick-add bar's "genie" button.
    QLineEdit *m_genieTokenEdit = nullptr;
    QLineEdit *m_genieOrgEdit = nullptr;
    QComboBox *m_genieWorkflowCombo = nullptr;
    QLabel *m_genieStatusLabel = nullptr;
    QTableWidget *m_commitsTable = nullptr;
    QString m_commitsLoadedRef;
    QString m_commitsLoadedTip;
    QString m_commitsLoadedMirrorTip;
    int m_commitsLoadGen = 0;
    // repo dir + ref + tip + issue-store signature that applyCommitIssueClosures()
    // last scanned. Reading 500 full commit messages and then every issue's signed
    // event log is expensive enough to show up in the stall log, and its result is
    // a pure function of those four things, so an unchanged key means there is
    // nothing to re-derive.
    QString m_commitClosureScanKey;
    mutable QStringList m_branchesCache;
    mutable QString m_branchesCacheDir;
    mutable qint64 m_branchesCacheTime = 0;
    mutable QString m_defaultBranchFastCache;
    mutable QString m_defaultBranchFastCacheDir;
    mutable qint64 m_defaultBranchFastCacheTime = 0;
    mutable QString m_defaultBranchFastCacheSig;
    QLineEdit *m_commitSearch = nullptr;       // filter the commit list by hash/summary
    QLineEdit *m_globalSearch = nullptr;
    // The ✕ at the right end of the top search box. Qt's own
    // setClearButtonEnabled draws nothing under this stylesheet, so the box
    // carries a themed octicon action instead, shown only while there is text.
    QAction *m_globalSearchClear = nullptr;
    QListWidget *m_globalSearchPopup = nullptr;
    QTimer *m_globalSearchTimer = nullptr;     // debounce keystrokes before rebuilding
    int m_globalSearchGen = 0;                 // drops a commit-grep reply for an older query
    QString m_agentPageSearchMirror;
    // "No results for <term> (clear search)" shown in place of a list the top
    // search box filtered down to nothing.
    QLabel *m_agentSearchEmpty = nullptr;
    QLabel *m_issueSearchEmpty = nullptr;
    QLabel *m_commitSearchEmpty = nullptr;
    struct NavPlace {
        int section = 0;
        int repoIndex = -1;
        int detailTab = -1;
        int overviewPage = -1; // files=0, Git=1, branches=2, worktrees=3
        QString branch;
        int subTab = -1;
        int itemNumber = -1;
        QString commit;    // Git view: the commit whose diff is open
        QString filePath;  // Code view: the file open in the editor
        QString overviewDir; // Code view: the directory the file list is showing
        QString conversation; // Chat: the room / DM on screen
        QString itemId; // non-numeric page identity (for example, a teammate)
        bool operator==(const NavPlace &o) const
        {
            return section == o.section && repoIndex == o.repoIndex &&
                   detailTab == o.detailTab && overviewPage == o.overviewPage &&
                   branch == o.branch && subTab == o.subTab &&
                   itemNumber == o.itemNumber && commit == o.commit &&
                   filePath == o.filePath && overviewDir == o.overviewDir &&
                   conversation == o.conversation && itemId == o.itemId;
        }
    };
    QPushButton *m_navBackButton = nullptr;
    QPushButton *m_navForwardButton = nullptr;
    QTimer *m_navLongPressTimer = nullptr; // hold a nav button to open the trail
    bool m_navLongPressForward = false;    // which of the two is being held
    bool m_navMenuFromLongPress = false;   // swallow the click that ends the hold
    QList<NavPlace> m_navHistory;
    int m_navHistoryIndex = -1;     // current position in m_navHistory
    bool m_navRestoring = false;    // suppress recording while replaying the trail
    bool m_navRecordPending = false; // a debounced capture is already queued
    QTreeWidget *m_searchResultsTree = nullptr;
    QLabel *m_searchResultsTitle = nullptr;
    QLabel *m_searchResultsStatus = nullptr;
    QPushButton *m_searchResultsStop = nullptr;
    QList<QProcess *> m_searchProcs; // running git searches (killed by Stop)
    int m_searchPending = 0;         // Git processes + category workers running
    int m_searchPageGeneration = 0;  // invalidates collaboration/log workers
    QString m_searchPageQuery;
    QString m_searchRunScope;          // repository the running search is scoped to
    int m_searchRunGeneration = -1;    // generation of the run to record when done
    bool m_searchRunRecorded = true;   // that run already reached finishSearchPageRun
    QStringList m_searchRunKeys;       // result keys of the last completed run
    QMap<QString, int> m_searchRunKindCounts;
    QPushButton *m_searchResultsSave = nullptr;
    QPushButton *m_searchResultsScrub = nullptr;
    QFrame *m_saveSearchPrompt = nullptr;
    QLabel *m_saveSearchPromptLabel = nullptr;
    QCheckBox *m_saveSearchAlertsCheck = nullptr;
    QCheckBox *m_saveSearchSecurityCheck = nullptr;
    SecurityAlarmState m_securityAlarmState;
    QFrame *m_securityAlarmBanner = nullptr;
    QLabel *m_securityAlarmLabel = nullptr;
    QTimer *m_securityAlarmTone = nullptr;
    bool m_securityAlarmPrimed = false;
    bool m_securityAlarmStartupCheckQueued = false;
    QListWidget *m_savedSearchList = nullptr;
    QLabel *m_savedSearchEmpty = nullptr;
    QLabel *m_savedSearchCaption = nullptr;
    QLabel *m_searchHiddenCaption = nullptr;
    QCheckBox *m_searchHideVariablesCheck = nullptr;
    QPushButton *m_savedSearchRefreshButton = nullptr;
    bool m_savedSearchRefreshAll = false;
    QSet<QString> m_saveSearchPromptDismissed; // scope + query the user declined
    std::unique_ptr<SavedSearchStore> m_savedSearchStore;
    QTimer *m_savedSearchAlertTimer = nullptr;
    QStringList m_savedSearchCheckQueue;
    bool m_savedSearchCheckRunning = false;
    QTimer *m_searchSpinTimer = nullptr;
    int m_searchSpinAngle = 0;
    QLabel *m_commitsUnsyncedBanner = nullptr; // "N commits not yet synced" banner
    QPushButton *m_commitsUnsyncedSyncButton = nullptr;
    QTreeWidget *m_commitsUnsyncedFiles = nullptr;
    bool m_commitsUnsyncedExpanded = false;
    QStringList m_commitsUnsyncedHashes; // pending commits, newest first
    QString m_pendingCommitFileScroll;
    QWidget *m_commitsListPage = nullptr;
    QGraphicsOpacityEffect *m_commitsBannerOpacity = nullptr;
    QPropertyAnimation *m_commitsBannerFade = nullptr;
    QLabel *m_insightsSummary = nullptr;
    QLabel *m_insightsTraffic = nullptr;
    QLabel *m_insightsLanguageBar = nullptr;
    QLabel *m_insightsLanguageLegend = nullptr;
    QLabel *m_insightsActivity = nullptr;
    QTableWidget *m_insightsContributors = nullptr;
    QComboBox *m_insightsRangeCombo = nullptr; // activity time window selector
    QLabel *m_insightsActivityAxis = nullptr;  // "oldest <- ... -> newest" caption
    QPushButton *m_insightsRefreshButton = nullptr;
    static constexpr int kCommitWorkspaceChangesPage = 0;
    static constexpr int kCommitWorkspaceCommitPage = 1;
    static constexpr int kCommitWorkspaceRangePage = 2;
    QStackedWidget *m_gitFilesSlot = nullptr;
    QStackedWidget *m_gitHistorySlot = nullptr;
    QStackedWidget *m_gitWorkspaceStack = nullptr;
    QWidget *m_gitMergedPage = nullptr;
    QLabel *m_gitMergedLabel = nullptr;
    QTextBrowser *m_gitMergedStats = nullptr;
    QString m_gitMergedBranch;
    QString m_gitMergedBase;
    QString m_gitMergedDir;
    QList<AgentSession> m_gitMergedSessions;
    bool m_gitMergedLookBack = false;
    QPushButton *m_commitsLeaderboardButton = nullptr;
    int m_gitMergedStatsGen = 0;
    void loadGitMergedStats();
    void showGitLeaderboard();
    QWidget *buildGitMergedPage();
    bool gitWorkspaceShowsBranchRange(const QString &branch) const;
    void showGitMergedCover(const QString &branch, const QString &base);
    void clearGitMergedCover();
    bool gitMergedCoverShown() const;
    void setCommitWorkspacePage(int page);
    QLabel *m_commitsCompareArrow = nullptr;
    QPushButton *m_commitsCompareBaseButton = nullptr;
    void updateCommitsCompareIndicator();
    QWidget *m_scmPanel = nullptr;
    QWidget *m_scmControlsPanel = nullptr;
    QTreeWidget *m_scmTree = nullptr;
    QPlainTextEdit *m_scmMessage = nullptr; // compact two-line commit/post draft
    QTextBrowser *m_scmDiff = nullptr;
    forkmesh::ui::SecretDiffBanner *m_scmSecretBanner = nullptr;
    QList<RepoSecurityFinding> m_scmSecretFindings;
    QLabel *m_scmCountLabel = nullptr;
    QLabel *m_scmViewedLabel = nullptr;  // "3 of 26 files viewed"
    QPushButton *m_scmGenerateButton = nullptr;
    QPushButton *m_scmGenerateIconButton = nullptr; // the compose row's sparkle
    QComboBox *m_scmGenModel = nullptr;       // AI model for inline generation
    QComboBox *m_scmGenKind = nullptr;        // "Commit message" vs "X post"
    QComboBox *m_scmGenDuration = nullptr;    // diff scope: current / past hour / all day
    QPushButton *m_scmCopyButton = nullptr;   // copy the message to the clipboard
    QLabel *m_scmGenStatus = nullptr;         // inline cost / progress note
    bool m_scmGenerating = false;             // a generation request is in flight
    bool m_scmAutoFillInFlight = false;       // coalesces automatic Git reads
    bool m_scmAutoFillPending = false;        // refresh arrived during that read
    quint64 m_scmMessageReadGeneration = 0;   // rejects stale worker/network text
    int m_scmHeuristicVariant = 0;            // cycles on-device drafts on re-click
    QPushButton *m_scmCommitButton = nullptr;
    QPushButton *m_scmCommitPushButton = nullptr; // commit, then publish/push
    QPushButton *m_scmStageCommitPushButton = nullptr; // stage all, commit, push
    QWidget *m_scmOutgoingPanel = nullptr;
    QLabel *m_scmOutgoingLabel = nullptr; // branch + pending commit count
    QPushButton *m_scmSyncButton = nullptr; // publish/push pending commits
    QWidget *m_scmSyncRow = nullptr;
    ElidingStatusLabel *m_scmSyncStatus = nullptr; // below button:
    int m_scmOutgoingGeneration = 0; // rejects late ahead-count callbacks
    bool m_scmOutgoingProbeInFlight = false;
    bool m_scmOutgoingProbePending = false;
    // Filesystem signature (HEAD/reflog/refs mtimes of the checkout and its
    // mirror) the last ahead-count probe ran against, and what it found. A
    // roster frame or the 60 s stats tick re-asks for the count without any
    // of those having moved; the cached answer is replayed instead of
    // spawning rev-list (and the mirror's ref lookups) again.
    QString m_scmOutgoingProbeSig;
    QString m_scmOutgoingProbeRepoPath;
    bool m_scmOutgoingProbeHaveResult = false;
    bool m_scmOutgoingProbeBranchResolved = false;
    QString m_scmOutgoingProbeBranch;
    int m_scmOutgoingProbePendingCount = 0;
    quint64 m_scmStatusGeneration = 0; // rejects late `git status` callbacks
    // Full re-renders of the working-tree diff. Ticking Viewed must not add to
    // it: a re-render re-lays out every file and moves the page the reviewer is
    // reading (see swapScmDiffFileInPlace).
    quint64 m_scmDiffRenders = 0;
    bool m_scmOutgoingBlocking = false; // commits ahead, or a sync in flight
    int m_scmPendingChangeCount = 0;    // working-tree rows the panel would list
    QHash<int, QString> m_repoSyncActivity;
    void setRepoSyncActivity(int index, const QString &line);
    void clearRepoSyncActivity(int index);
    std::shared_ptr<QString> streamGitProgressActivity(QProcess *process,
                                                       int index,
                                                       const QString &prefix);
    static QString gitErrorsWithoutProgress(const QString &text);
    QPushButton *m_scmRefreshButton = nullptr;
    QPushButton *m_scmPrevButton = nullptr;   // jump to previous changed file
    QPushButton *m_scmNextButton = nullptr;   // jump to next changed file
    QPushButton *m_scmAutoViewedButton = nullptr;
    QPushButton *m_scmTreeModeButton = nullptr;
    QPushButton *m_scmAllReposButton = nullptr;
    bool m_scmTreeFolders = false;
    bool m_scmAllReposView = false;
    QLabel *m_scmEmptyNote = nullptr;
    QByteArray m_scmStatusCache;
    QString m_scmCombinedPatch;      // cached raw patch behind the render
    int m_scmCombinedStagedFiles = 0; // how many of its files are the staged half
    bool m_scmPatchValid = false;    // false until the patch is (re-)read from git
    bool m_scmPatchReadInFlight = false; // worker owns the current invalid patch
    quint64 m_scmPatchGeneration = 0; // rejects reads invalidated by a later status
    QStringList m_scmSectionKeys;
    QStringList m_scmSectionAnchors; // "file-N" per section, aligned to the keys
    QStringList m_scmSectionPaths;   // repo-relative path per section
    QList<int> m_scmFileTops;        // cached absolute y of each section header
    QHash<QString, QString> m_scmStickyLabelHtml; // section key -> sticky label
    QHash<QString, QString> m_scmStickyActionHtml;
    QLabel *m_scmNextFileBar = nullptr;
    QString m_scmNextFileKey;
    QFrame *m_scmDiffActiveOutline = nullptr;
    QString m_scmActiveSectionKey;
    QString m_scmPendingScrollKey;
    QString m_scmDiffRenderKey;      // skip the re-layout when nothing changed
    QString m_scmDiffSourceKey;
    QFrame *m_scmStickyHeader = nullptr;
    QLabel *m_scmStickyPath = nullptr;
    QLabel *m_scmStickyControls = nullptr;
    QString m_scmStickySection;      // section key shown in the sticky header
    // <= 0: how far the next file's header has pushed the sticky bar out
    int m_scmStickyTopOffset = 0;
    QTimer *m_scmAutoViewedDebounce = nullptr;
    int m_scmLastAutoViewedScrollValue = 0;
    int m_scmAutoViewedScrollDirection = 1; // +1 down, -1 back up
    bool m_scmApplyingAutoViewed = false; // ignore render/anchor scroll signals
    bool m_scmSuppressFileScroll = false;
    QStackedWidget *m_commitsStack = nullptr;
    QLabel *m_commitTitle = nullptr;
    QLabel *m_commitMeta = nullptr;
    QLabel *m_commitMessage = nullptr;
    QLabel *m_commitFilesSummary = nullptr;
    QListWidget *m_commitFileList = nullptr;
    QTextBrowser *m_commitDiffView = nullptr;
    QWidget *m_commitDiffSpinner = nullptr; // inline spinner by the files heading
    int m_commitLoadGen = 0;
    QPushButton *m_commitPrevButton = nullptr;
    QPushButton *m_commitNextButton = nullptr;
    QPushButton *m_commitDownloadButton = nullptr;
    QPushButton *m_commitDeleteButton = nullptr; // drop this commit from history
    QPushButton *m_commitRevertButton = nullptr; // commit the inverse of this one
    QPushButton *m_commitSplitButton = nullptr; // toggle unified <-> side-by-side
    QString m_currentCommitHash; // full hash shown in the detail view
    int m_currentCommitRow = -1; // row in m_commitsTable the detail view is showing
    QStackedWidget *m_filesStack = nullptr; // Code overview host
    QString m_overviewPath;                 // current directory in the overview
    // Signature (repo|path|branch|HEAD) of the overview currently rendered, so
    // re-entering the repo screen unchanged skips the expensive git re-read.
    QString m_overviewLoadedKey;
    bool m_overviewLoading = false;
    quint64 m_overviewLoadGen = 0;
    QString m_overviewLoadRequest;
    bool m_overviewReloadPending = false;
    int m_treeLoadedForIndex = -1;          // repo whose explorer tree is built
    QLabel *m_commitBar = nullptr;
    QLabel *m_commitBarAvatar = nullptr; // author portrait, left of the line
    QStackedWidget *m_overviewBodyStack = nullptr;
    QLabel *m_overviewCrumb = nullptr;
    QString m_commitBarStatusHash;
    QString m_commitBarBodyHtml;
    QTreeWidget *m_overviewList = nullptr;
    struct OverviewRow {
        QString name;
        QString path;
        bool isDir = false;
        qint64 size = 0;     // blob bytes (recursive sum for directories)
        qint64 loc = 0;      // lines of code (recursive sum for directories)
        qint64 fileCount = 0; // number of files (recursive) — shown for directories
        qint64 commitTs = 0; // last commit unix time that touched this entry
        QString subject;     // last commit subject
        QString whenText;    // relative "x ago"
    };
    QList<OverviewRow> m_overviewRows;
    struct OverviewRead {
        QString dir;
        QString path;
        QString ref;
        QString keyPrefix;
        QString loadedKey;
        QString recoverBase;
        bool wantCommitBar = false;
        QString cacheKey;
        QByteArray cachedSizeOut;
        QByteArray cachedLocOut;
        QHash<QString, QByteArray> cachedEntryLog;
    };
    struct OverviewLoad {
        QString head;
        bool refMissing = false;
        BrowsedRefRecovery recovery;
        bool unchanged = false;
        bool hasCommit = false;
        QString commitHash;
        QString commitAuthor;
        QString commitWhen;
        QString commitSubject;
        QString messageRow;
        QList<OverviewRow> rows;
        qint64 repoBytes = 0;
        QStringList pendingEntryLogs;
        QByteArray sizeOut;
        QByteArray locOut;
        QString readmePath;
        QString readmeMarkdown;
    };
    qint64 m_overviewRepoBytes = 0; // whole-repo blob total (size-bar denominator)
    QString m_overviewSortKey = QStringLiteral("name");
    bool m_overviewSortDesc = false;
    void populateOverviewTree(); // (re)fill m_overviewList from m_overviewRows
    void showOverviewLoadingPlaceholders();
    QTextBrowser *m_readmeView = nullptr;
    QTreeWidget *m_repoFileTree = nullptr;
    QTabWidget *m_repoFileTabs = nullptr;
    QPushButton *m_repoFileCommitButton = nullptr;
    QPushButton *m_repoFilePullButton = nullptr;
    QPushButton *m_repoFileSaveButton = nullptr;
    QPushButton *m_repoFileHistoryButton = nullptr;
    QPushButton *m_repoFilePreviewButton = nullptr; // toggle markdown source/render
    QHash<QString, QWidget *> m_openFileTabs;
    QStackedWidget *m_filesTreeStack = nullptr; // 0 filesystem, 1 repository, 2 coves
    QPushButton *m_filesRepoTreeButton = nullptr; // -> the repository's tree
    QPushButton *m_filesCoveTreeButton = nullptr; // -> encrypted Cove documents
    QTreeWidget *m_fileExplorerTree = nullptr;
    QPlainTextEdit *m_fileExplorerPreview = nullptr;
    QLineEdit *m_fileExplorerPath = nullptr;
    QLabel *m_fileExplorerStatus = nullptr;
    QCheckBox *m_fileExplorerHidden = nullptr;
    QPushButton *m_filesSizeButton = nullptr; // show the Size column (off by default)
    QString m_fileExplorerRoot;
    QLineEdit *m_filesCommitMessage = nullptr;
    QPushButton *m_filesCommitButton = nullptr;
    QPushButton *m_filesPushButton = nullptr;
    // Files editor (adhoc #1607). The breadcrumb names the open tab's file so
    // it is always clear which one the editor is showing; the find bar and the
    // symbol panel are hidden until Ctrl+F / a symbol trace opens them.
    QLabel *m_filesBreadcrumb = nullptr;
    QLabel *m_filesCursorLabel = nullptr;
    QWidget *m_filesFindBar = nullptr;
    QLineEdit *m_filesFindInput = nullptr;
    QLineEdit *m_filesReplaceInput = nullptr;
    QWidget *m_filesReplaceRow = nullptr;
    QLabel *m_filesFindCount = nullptr;
    QPushButton *m_filesFindCaseButton = nullptr;
    QPushButton *m_filesFindWordButton = nullptr;
    QList<QTextCursor> m_filesFindMatches;
    int m_filesFindIndex = -1;
    // The tab the find bar's matches belong to: a match list is a set of
    // cursors into one document, so switching tabs has to drop it.
    QWidget *m_filesFindTab = nullptr;
    // Symbol trace panel: one query (symbol + direction) at a time, with rows
    // that expand into the next step of the trace.
    QWidget *m_filesSymbolPanel = nullptr;
    // The tabs-over-panel splitter, so opening the panel can take its space
    // from the editor once rather than letting the layout guess every time.
    QSplitter *m_filesEditorSplit = nullptr;
    QLabel *m_filesSymbolTitle = nullptr;
    QTreeWidget *m_filesSymbolTree = nullptr;
    QList<QPushButton *> m_filesSymbolModeButtons;
    QString m_filesSymbolName;
    int m_filesSymbolMode = forkmesh::code::TraceReferences;
    // Bumped per query so a scan the user has already navigated past is dropped
    // when its worker thread finishes.
    int m_filesSymbolGen = 0;

    QComboBox *m_coveExplorerSelector = nullptr;
    QLabel *m_coveExplorerStatus = nullptr;
    QLabel *m_coveExplorerSharedWithLabel = nullptr;
    QTreeWidget *m_coveExplorerTree = nullptr;
    QPushButton *m_coveExplorerNewButton = nullptr;
    QPushButton *m_coveExplorerInviteButton = nullptr;
    QPushButton *m_coveExplorerCopyLinkButton = nullptr;
    QPushButton *m_coveExplorerNewFileButton = nullptr;
    QPushButton *m_coveExplorerDeleteButton = nullptr;
    QList<Cove> m_coveExplorerCoves;
    QHash<QString, QWidget *> m_openCoveExplorerTabs; // coveId\x1fdocPath -> editor
    QString m_coveExplorerCurrentId;

    QTableWidget *m_discussionTable = nullptr;
    QLineEdit *m_discussionSearch = nullptr;
    QComboBox *m_discussionCategoryFilter = nullptr;
    QPushButton *m_discussionNewButton = nullptr;
    QPushButton *m_discussionSyncButton = nullptr;
    QPushButton *m_discussionCloseButton = nullptr;
    QPushButton *m_discussionArchiveButton = nullptr;
    QPushButton *m_discussionDeleteButton = nullptr;
    QLabel *m_discussionTitle = nullptr;
    QLabel *m_discussionMeta = nullptr;
    QLabel *m_discussionInlineNotice = nullptr;
    QScrollArea *m_discussionThreadScroll = nullptr;
    QWidget *m_discussionThreadContainer = nullptr;
    QVBoxLayout *m_discussionThreadLayout = nullptr;
    MarkdownEditor *m_discussionComposer = nullptr;
    QPushButton *m_discussionCommentButton = nullptr;
    QLabel *m_discussionCategorySummary = nullptr;
    QList<Discussion> m_currentDiscussions;
    int m_discussionCountLoadGen = 0;
    int m_currentDiscussionNumber = -1;
    DiscussionInboxBackoff m_discussionInboxBackoff;
    NetworkBackoff m_pollBackoff;
    // a refill notification waiting to ride the next signed
    // heartbeat (kept as flags, not fired directly, so it retries on the
    // periodic heartbeat timer if the immediate send fails).
    bool m_pendingCreditsRefilled5h = false;
    bool m_pendingCreditsRefilledWeekly = false;
    // A pause deadline this machine just set and has not yet handed to the
    // relay. Unlike the two settings above it is NOT restated on every beat, so
    // a pause started in the browser is not overwritten a minute later.
    bool m_pendingPingPausePush = false;
    // A ping settings change (kinds, email, quiet hours) not yet acknowledged.
    bool m_pendingPingSettingsPush = false;
    // Wakes the UI the moment a pause lapses, so the button stops claiming a
    // deadline that has already passed without waiting for the next ping.
    QTimer *m_pingPauseExpiryTimer = nullptr;
    QPushButton *m_pingPauseButton = nullptr;
    QLabel *m_pingPauseStatus = nullptr;
    QHash<QString, QTimer *> m_usageLimitReminderTimers;
    QHash<int, QTimer *> m_usageLimitAgentHoldTimers;

    // --- Cove (encrypted vault) UI + session state ----------------------------
    QWidget *m_coveSection = nullptr;        // repo Settings "Coves" group
    QListWidget *m_coveList = nullptr;       // coves in the open repo (lock state)
    QLineEdit *m_covePasswordEdit = nullptr; // per-repo unlock password field
    QPushButton *m_covePwRevealBtn = nullptr;// reveal pw (source-of-truth only)
    QCheckBox *m_coveAutoOpenCheck = nullptr;// per-repo auto-open toggle
    QLabel *m_coveEmptyHint = nullptr;
    // Cove id -> the password that unlocked it this session (memory only). Lets
    // "auto-show" reveal a cove without re-prompting and re-encrypt on save.
    QHash<QString, QString> m_coveSessionPasswords;
    QSet<QString> m_coveFailedUnlocks;

    QTableWidget *m_pullTable = nullptr;
    // Spinner row above the list, shown while a load runs with nothing on
    // screen yet — landing on the tab is otherwise a blank pane.
    QWidget *m_pullListLoading = nullptr;
    QLineEdit *m_pullSearch = nullptr;
    QPushButton *m_pullNewButton = nullptr;
    QPushButton *m_pullChooseDirButton = nullptr;
    QPushButton *m_pullImportButton = nullptr;
    QPushButton *m_pullSyncButton = nullptr;
    QPushButton *m_pullRefreshButton = nullptr;
    QPushButton *m_pullShowOpenButton = nullptr;
    QPushButton *m_pullShowClosedButton = nullptr;
    QPushButton *m_pullShowMergedButton = nullptr;
    QPushButton *m_pullShowAllButton = nullptr;
    int m_pullRegisteredCount = -1; // live code-branch + pull-ledger union
    QPushButton *m_pullDeleteClosedAndMergedButton = nullptr;
    QWidget *m_pullDetail = nullptr;
    QLabel *m_pullTitle = nullptr;
    QLabel *m_pullStatusBadge = nullptr;
    QLabel *m_pullMeta = nullptr;
    QLabel *m_pullMergeStatus = nullptr; // conflict / ready-to-merge banner
    QPushButton *m_pullUpdateButton = nullptr;
    QPushButton *m_pullMergeButton = nullptr;
    QPushButton *m_pullResolveButton = nullptr; // opens the conflict merge editor
    QPushButton *m_pullFixButton = nullptr;
    QComboBox *m_pullFixModelCombo = nullptr; // which agent+model "Fix" starts
    QPushButton *m_pullFixConflictsButton = nullptr;
    QPushButton *m_pullReviewAiButton = nullptr;
    QPushButton *m_pullFixAllAiButton = nullptr;
    QPushButton *m_pullEditFileButton = nullptr; // edit selected file on PR branch
    QPushButton *m_pullDeleteFileButton = nullptr; // delete selected file on PR branch
    QPushButton *m_pullCloseButton = nullptr;
    QPushButton *m_pullReopenButton = nullptr;
    QPushButton *m_pullRestoreButton = nullptr; // move an archived code-branch PR into the ledger
    QPushButton *m_pullSendToSourceButton = nullptr;
    QPushButton *m_pullDeleteButton = nullptr;
    QPushButton *m_pullDeleteBranchButton = nullptr; // delete the PR and its head branch
    QPushButton *m_pullMergeDeleteButton = nullptr;  // merge, then delete the PR + branch
    QPushButton *m_pullQueueButton = nullptr;        // add/remove this PR in the merge queue
    QPushButton *m_pullPreviewButton = nullptr;      // build the PR and launch the app
    QDialog *m_pullPreviewDialog = nullptr;          // live build log for the preview
    bool m_pullDeleteConfirmPending = false;
    bool m_pullDeleteInProgress = false; // a deletePull worker thread is running

    QWidget *m_mergeQueuePanel = nullptr;
    QLabel *m_mergeQueueSummary = nullptr;
    QListWidget *m_mergeQueueList = nullptr;
    QPushButton *m_mergeQueuePauseButton = nullptr;
    QPushButton *m_mergeQueueUpButton = nullptr;
    QPushButton *m_mergeQueueDownButton = nullptr;
    QPushButton *m_mergeQueueRemoveButton = nullptr;
    QPushButton *m_mergeQueueClearButton = nullptr;
    QTimer *m_mergeQueueTimer = nullptr;
    bool m_mergeQueueBusy = false;
    QListWidget *m_pullFiles = nullptr;
    QComboBox *m_pullFileAuthorFilter = nullptr;
    QHash<QString, bool> m_pullFileAuthorship;
    QPushButton *m_pullPrevButton = nullptr; // jump to previous change in the PR
    QPushButton *m_pullNextButton = nullptr; // jump to next change in the PR
    QTextBrowser *m_pullDiff = nullptr;
    QTextBrowser *m_pullInlineView = nullptr;
    QFrame *m_pullInlineComposerFrame = nullptr;
    QLabel *m_pullInlineComposerTitle = nullptr;
    QPlainTextEdit *m_pullInlineCommentEdit = nullptr;
    QPushButton *m_pullInlineAddButton = nullptr;
    QPushButton *m_pullInlineNowButton = nullptr;
    QPushButton *m_pullInlineSubmitBatchButton = nullptr;
    QWidget *m_pullInlineBatchBar = nullptr;
    QLabel *m_pullInlineBatchLabel = nullptr;
    QPushButton *m_pullInlineBatchSubmitButton = nullptr;
    QPushButton *m_pullInlineBatchDiscardButton = nullptr;
    QString m_pullInlinePath;
    QString m_pullInlineSide;
    int m_pullInlineStartLine = 0;
    int m_pullInlineEndLine = 0;
    bool m_pullInlineSelecting = false;
    QString m_pullInlineBranch;
    QString m_branchReviewEventsKey;
    QList<PullEvent> m_branchReviewEvents;
    QSet<QString> m_pullOpenLineNotes;
    struct PullInlineDraft {
        QString repoKey;
        int number = 0;
        QString path;
        QString side;
        int lineStart = 0;
        int lineEnd = 0;
        QString body;
        // Captured when the draft is queued: the diff the reviewer saw then
        // is what the comment is about, not whatever is shown at send time.
        QString patchset;
        QString anchorLines;
    };
    QList<PullInlineDraft> m_pullInlineDrafts;
    QWidget *m_pullDiffSearchBar = nullptr;
    QLineEdit *m_pullDiffSearchInput = nullptr;
    QLabel *m_pullDiffSearchCount = nullptr;
    QList<QTextCursor> m_pullDiffSearchMatches;
    int m_pullDiffSearchIndex = -1;
    // Signature (stylesheet + html) of what m_pullDiff currently shows, so
    // renderPullDiff can skip the costly QTextDocument table re-layout when a
    // refresh/poll re-renders the same file with unchanged content. Cleared
    // whenever the widget is set to something other than a rendered diff.
    QString m_pullDiffRenderKey;
    QString m_pullDiffSourceKey;
    // The two halves of m_pullDiffSourceKey a Viewed tick cannot change, kept
    // so an in-place fold can restate the key cheaply.
    QString m_pullDiffNotesKey;
    QString m_pullDiffPatchKey;
    QHash<QString, QString> m_pullFileAnchors;
    QStringList m_pullFileOrder;
    bool m_pullSuppressFileScroll = false;
    QPushButton *m_pullAutoViewedButton = nullptr;
    QLabel *m_pullViewedLabel = nullptr;
    void updatePullViewedLabel();
    QTimer *m_pullAutoViewedDebounce = nullptr;
    QFrame *m_pullStickyHeader = nullptr;
    QLabel *m_pullStickyPath = nullptr;
    QLabel *m_pullStickyControls = nullptr;
    QString m_pullStickyFile; // file path currently shown in the sticky header
    int m_pullStickyTopOffset = 0; // see m_scmStickyTopOffset
    QHash<QString, QString> m_pullStickyLabelHtml;
    QList<int> m_pullFileTops;
    int m_diffFontPt = 12; // diff viewer text size (the +/- zoom control)
    QList<QTextEdit *> m_diffViews;
    QHash<QTextEdit *, int> m_diffRestoreScroll;
    QPushButton *m_pullSplitButton = nullptr; // toggle unified <-> side-by-side
    QListWidget *m_pullCommitsList = nullptr;  // commits that make up the PR
    QButtonGroup *m_pullSubTabs = nullptr;
    QStackedWidget *m_pullSubStack = nullptr;
    QPushButton *m_pullTabConversation = nullptr;
    QPushButton *m_pullTabCommits = nullptr;
    QPushButton *m_pullTabChecks = nullptr;
    QPushButton *m_pullTabFiles = nullptr;
    QPushButton *m_pullTabBadge = nullptr;
    PullBadgeWidget *m_pullBadgeWidget = nullptr;
    QScrollArea *m_pullThreadScroll = nullptr;
    QWidget *m_pullThreadContainer = nullptr;
    QVBoxLayout *m_pullThreadLayout = nullptr;
    QLabel *m_pullChecksSummary = nullptr; // compact pass/fail/running card in thread
    QFrame *m_pullConflictCard = nullptr;    // conflicting-files card above the composer
    QLabel *m_pullConflictDetails = nullptr; // that card's text
    QPushButton *m_pullConflictMessageAuthorButton = nullptr;
    QLabel *m_pullMergeBlockers = nullptr; // why this PR cannot merge, above the buttons
    MarkdownEditor *m_pullComposer = nullptr;
    QLabel *m_pullComposerAvatar = nullptr; // reviewer face in the timeline's avatar column
    QLabel *m_pullComposerTitle = nullptr;  // "Reviewing as <name>"
    QPushButton *m_pullCommentButton = nullptr;
    QPushButton *m_pullApproveButton = nullptr;
    QPushButton *m_pullRequestChangesButton = nullptr;
    QPushButton *m_pullConversationMergeButton = nullptr;
    // "Update branch" beside the conversation merge button: merges the base
    // branch (usually main) into this pull request's head branch.
    QPushButton *m_pullConversationUpdateButton = nullptr;
    QPushButton *m_pullConversationCloseButton = nullptr;
    QPushButton *m_pullConversationDeleteButton = nullptr;
    QWidget *m_pullAgentRevisionRow = nullptr;
    QLineEdit *m_pullAgentRevisionEdit = nullptr;
    QPushButton *m_pullSendToAgentButton = nullptr;
    QPushButton *m_pullLinkIssueButton = nullptr; // "Link issue" in the PR header
    QLabel *m_pullLinksValue = nullptr;           // linked issues card in the thread
    QTableWidget *m_pullChecksTable = nullptr;
    QPlainTextEdit *m_pullChecksLog = nullptr;
    QPushButton *m_pullRunChecksButton = nullptr;
    QPushButton *m_pullApproveChecksButton = nullptr; // release held first-PR runs
    QLabel *m_pullChecksNotice = nullptr;             // why a run is being held
    QList<PullRequest> m_currentPulls;
    quint64 m_repoPullCountGen = 0;
    bool m_repoPullCountInFlight = false;
    bool m_repoPullCountReloadQueued = false;
    quint64 m_repoIssueCountGen = 0;
    bool m_repoIssueCountInFlight = false;
    bool m_repoIssueCountReloadQueued = false;
    QString m_repoIssueCountMirror;
    QHash<QString, QString> m_pullFileDiffs; // current PR: file path -> diff text
    QHash<int, bool> m_pullConflictByNumber;
    QString m_pullConflictCacheBaseTip;
    struct PullConflictEntry {
        QString fingerprint;
        bool conflict = false;
        QStringList conflictFiles;
    };
    QHash<int, PullConflictEntry> m_pullConflictCache;
    // Last answered probe per "owner/name#number". Unlike the cache above it
    // survives a base-tip change — the very moment a conflict usually appears —
    // so a clean-to-conflicted flip can be told from a conflict first seen.
    QHash<QString, bool> m_pullConflictLastProbe;
    // Conflict notices posted this session, by pullConflictNoticeSessionKey():
    // the ledger reload that shows the comment can lag the post.
    QSet<QString> m_pullConflictNoticesPosted;
    struct PullBehindEntry {
        bool valid = false;
        bool behind = false;
        int count = 0;
    };
    QHash<QString, PullBehindEntry> m_pullBehindCache;
    QSet<QString> m_pullBehindChecksPending;
    // Head-branch state behind the pull list's leading column: the same three
    // signals the agents list paints on its branch button — how the head
    // stands against its base, whether a worktree still has it checked out,
    // and whether that checkout is dirty. Filled by one worker pass per load
    // (like m_pullProvenance) because answering it per row on the GUI thread
    // means a `git rev-list` and a `git status` for every visible pull.
    // A missing entry, or counted == false, simply means "not known yet".
    struct PullBranchState {
        bool counted = false; // ahead/behind were read from the head branch
        int ahead = 0;
        int behind = 0;
        QString worktree; // linked checkout holding the head branch, if any
        int dirty = -1;   // uncommitted entries in that checkout; -1 unknown
        bool operator==(const PullBranchState &other) const
        {
            return counted == other.counted && ahead == other.ahead &&
                   behind == other.behind && worktree == other.worktree &&
                   dirty == other.dirty;
        }
    };
    QHash<int, PullBranchState> m_pullBranchState;
    quint64 m_pullBranchStateGen = 0;
    // Which repository the entries above were measured in. Pull numbers repeat
    // across repositories, so opening a different one has to drop them rather
    // than paint another repository's answers until the worker replies.
    QString m_pullBranchStateRepo;
    // Agent attribution per PR, read from the ForkMesh-Agent trailer in the
    // signed commit series. Computed once per load on a worker rather than
    // inside refreshPullList(), which called pullAgentProvenance() for every
    // visible row: a PR with no trailer scans its whole (multi-megabyte) mbox to
    // the end before answering "no", and the stall log caught that table build
    // at 1.15s. Missing entry simply means "no badge yet".
    QHash<int, PullAgentProvenance> m_pullProvenance;
    quint64 m_pullProvenanceGen = 0;
    quint64 m_pullConflictGen = 0;
    quint64 m_pullLoadGen = 0;
    bool m_pullBackgroundLoadInFlight = false;
    bool m_pullBackgroundReloadQueued = false;
    QByteArray m_pullSnapshotDigest;
    // The snapshot the rows on screen were built from. A load whose digest
    // repeats is only skippable once the table has actually rendered it.
    QByteArray m_pullListRenderedDigest;
    QList<QPair<int, QString>> m_pendingPullConflictChecks;
    bool m_pullConflictCheckInFlight = false;
    static QString pullPatchFingerprint(const QString &patch);
    static QString pullDetailDigest(const PullRequest &pr);
    int m_currentPullNumber = -1;
    int m_pullOpenTarget = -1;
    qint64 m_pullOpenTargetDeadlineMs = 0;
    // Last ledger sweep per owner/repo key so relay-frame bursts and the
    // pulls-tab refresh timer cannot stack git fetches.
    QHash<QString, qint64> m_pullLedgerSweepAtMs;
    bool m_pullLedgerSweepInProgress = false;
    // Digest of the pull rendered in the detail pane; a background reload
    // whose digest differs re-renders the open detail in place.
    QString m_pullDetailRenderedDigest;

    struct AppNotification {
        QString title;
        QString body;
        qint64 timestampMs = 0;
        bool warning = false;
        int runId = -1;
        NotificationLink link;
        QString kind;   // "issue" | "chat" | "action" | … (defaults from link)
        QString actor;  // who caused it, when known
        QString actorId;
        QString chatMessageId;
        QString repo;   // "owner/name", when it is about a repository
        qint64 id = 0;  // stable row identity, so one row can be deleted
        forkmesh::PingSync sync = forkmesh::PingSync::LocalOnly;
        QString syncReason; // the specific "why", e.g. "signed out"
        bool syncDecided = false;
        bool quiet = false;
        // A ping nobody is allowed to miss: its card carries no countdown and
        // only the Dismiss button takes it off the screen. `acknowledged`
        // records that the click happened, so one that was still up when the
        // app went down is raised again on the next start rather than lost.
        bool dismissOnly = false;
        bool acknowledged = false;
        bool forward = false;
        QString phonePingId;
    };
    // The initial history scan owns a shared lease, so it remains valid if the
    // window is closed before its worker completes.
    std::shared_ptr<ActionStore> m_actionStore;
    QList<ActionRunner *> m_actionRunners;
    QFileSystemWatcher *m_actionSpoolWatcher = nullptr;
    QList<ActionRun> m_actionRuns;   // loaded history, newest first
    QList<int> m_actionQueue;        // run ids queued for execution
    QList<DeferredActionWorkflowScan> m_deferredActionWorkflowScans;
    QSet<QString> m_explicitActionPushes;
    // "owner\x1fname\x1f<pr>" -> head commit already handed to the pull-request
    // check pass, so a pull reload storm queues each head exactly once. Runs
    // persisted on disk are the durable half of this guard; this map only stops
    // a second pass starting before the first one's off-thread scan lands.
    QHash<QString, QString> m_pullChecksQueued;
    bool m_pullChecksScanInFlight = false;
    // Reload storms are the norm on this page, and resolving heads costs a git
    // process per open pull request. Re-resolve immediately when the open pulls
    // themselves changed; otherwise no more than once a minute, which is what
    // catches a contributor pushing onto a head this list cannot see move.
    QHash<QString, QString> m_pullChecksScanFingerprint; // "owner/name" -> pulls
    QHash<QString, qint64> m_pullChecksScanAtMs;
    // "owner\x1fname\x1f<pr>" for pulls whose signed commits did not replay onto
    // the base. Cleared whenever that repository's open pulls change, so an
    // updated submission is tried again.
    QSet<QString> m_pullChecksNoHead;
    QSet<int> m_actionWaitingRuns;   // queued ids already logged as `needs:`-blocked
    // The encrypted mirror materialization a run is executing out of (see
    // pinActionMirror). Held until the run finishes so a concurrent re-seal
    // cannot delete the served mirror mid-build.
    struct ActionMirrorPin {
        std::shared_ptr<void> materialization; // keeps the directory alive
        QString path;                          // mirror the run was handed
        QString owner;
        QString name;
    };
    struct ActionQueueVerification {
        int runId = -1;
        int repoIndex = -1;
        ActionRun verified;
        QString mirror;
        QString workTree;
        QString repositoryWorkflow;
        QString snapshotError;
        std::shared_ptr<void> mirrorPin;
    };
    // Queued runs whose approved snapshot is being re-verified on a worker,
    // keyed by run id with the time the check began. Each one has an idle
    // runner set aside for it, and they run side by side: a single in-flight
    // flag used to hold the whole queue behind one check, so a deploy whose
    // `needs:` had already passed sat "Queued" while a desktop build ran.
    QHash<int, qint64> m_actionVerifyingRuns;
    QHash<int, ActionMirrorPin> m_actionMirrorPins; // run id -> pinned mirror
    QString m_mirrorActionsConfigGeneration;
    QString m_mirrorActionsRuntimeState;
    qint64 m_mirrorActionsRuntimeStateWrittenAtMs = 0;
    QTimer *m_mirrorActionsSummaryTimer = nullptr;
    qint64 m_mirrorActionsSummaryAttemptedAtMs = 0;
    qint64 m_lastExternalActionsScanMs = 0;
    QList<AppNotification> m_notifications;
    // The newest desktop notification's target. The tray fallback only reports
    // that *a* balloon was clicked, so a click resolves against the last one.
    QString m_desktopNotificationHref;
    int m_desktopNotificationWaiters = 0; // live `notify-send --wait` processes
    qint64 m_nextNotificationId = 1;
    qint64 m_announcingPingId = 0;
    QTimer *m_notificationJournalTimer = nullptr; // debounced journal write
    QSet<QString> m_flashedWebAlertIds;
    QSet<QString> m_celebratedWebAlertIds;
    bool m_webAlertsBaselineLoaded = false;
    QPushButton *m_notificationButton = nullptr;
    QTableWidget *m_notificationsTable = nullptr; // sortable Notifications page
    QScrollArea *m_pingDetailPane = nullptr;
    QString m_pingDetailKey;
    QString m_pingDetailSignature;
    QPushButton *m_webAlertsPrevPageButton = nullptr;
    QPushButton *m_webAlertsNextPageButton = nullptr;
    QLabel *m_webAlertsPageLabel = nullptr;
    QJsonArray m_webAlerts;
    int m_webAlertsUnread = 0;
    qint64 m_localPingsSeenAtMs = -1; // -1 = not loaded from QSettings yet
    int m_webAlertsTotal = 0;
    int m_webAlertsPage = 0;
    bool m_webAlertsLoading = false;
    qint64 m_webAlertsFetchedAtMs = 0;
    int m_selectedRunId = -1;
    QListWidget *m_actionWorkflowList = nullptr; // available actions (left column)
    QComboBox *m_actionNodeCombo = nullptr;      // node the selected action runs on
    QLabel *m_actionNodeLabel = nullptr;         // caption above that dropdown
    QString m_selectedWorkflowFilter;            // workflow path filter, empty = all
    QList<ActionWorkflow> m_repoWorkflows;       // parsed workflows for the open repo
    int m_workflowCountLoadGen = 0;
    QTimer *m_openRepoRefreshTimer = nullptr;
    QTimer *m_agentsSpinTimer = nullptr; // refreshes live Agents metadata
    int m_agentSpinTicks = 0; // paces the detail header's run-stat refresh
    qint64 m_agentProcessMetaPid = 0;
    qint64 m_agentProcessMetaSampledAtMs = 0;
    int m_agentProcessMetaCompilerCount = 0;
    qint64 m_agentProcessMetaCompilerResidentBytes = 0;
    QString m_agentProcessMetaSubprocessText;
    bool m_agentProcessMetaRefreshInFlight = false;
    qint64 m_agentProcessMetaRefreshPid = 0;
    quint64 m_agentProcessMetaRefreshGeneration = 0;
#ifdef FORKMESH_WINDOW_TESTS
    QHash<int, qint64> m_testAgentSessionProcessIds;
    int m_testAgentProcessMetaScanLaunches = 0;
    QSet<int> m_testLiveTransportSessions;
    mutable std::function<void()> m_testWorktreeListCompletedHook;
    std::function<bool(
        const QString &, const QStringList &,
        const std::function<bool(QString *)> &)> m_testMergeConflictEditorHook;
#endif
    QTimer *m_actionsSpinTimer = nullptr; // refreshes live Actions status icons
    int m_actionSpinTicks = 0; // paces the actions table status icon spin
    QTableWidget *m_actionsTable = nullptr;
    // What refreshActionsTable() last rendered; an identical one skips a rebuild.
    QString m_actionsTableSignature;
    QLabel *m_actionRunTitle = nullptr;
    BusySpinner *m_actionRunSpinner = nullptr;
    QLabel *m_actionRunMeta = nullptr;
    // Where the run's log.txt sits on this node, under the meta line. A log
    // that is only ever a scroll view is unreachable from a terminal or an
    // agent; the path makes it an ordinary file again.
    ElidingStatusLabel *m_actionRunLogPath = nullptr;
    QLabel *m_actionApprovalBanner = nullptr;
    QPlainTextEdit *m_actionLog = nullptr;
    QTextBrowser *m_actionDiff = nullptr;
    // Extra run panes docked to the right of the primary detail, left to right.
    QSplitter *m_actionDetailSplitter = nullptr;
    QList<ParallelRunColumn> m_parallelRunColumns;
    // Selected runs the column cap turned away, so the notice is raised once
    // rather than on every step of a drag across the runs table.
    int m_parallelRunsDropped = 0;
    QWidget *m_actionApprovalBar = nullptr;
    QPushButton *m_actionApproveButton = nullptr;
    QPushButton *m_actionRejectButton = nullptr;
    QPushButton *m_actionSplitButton = nullptr;
    QWidget *m_actionManualRunBar = nullptr;
    QPushButton *m_actionManualRunButton = nullptr;
    QComboBox *m_actionRunBranchCombo = nullptr;
    QPushButton *m_actionRerunButton = nullptr;
    QPushButton *m_actionStopButton = nullptr;
    QPushButton *m_actionSkipButton = nullptr;
    QPushButton *m_actionCopyLogButton = nullptr;
    QPushButton *m_actionCopyLogPathButton = nullptr;
    QPushButton *m_actionSendToAgentButton = nullptr;
    QPushButton *m_actionFixButton = nullptr;
    QComboBox *m_actionFixAgentCombo = nullptr;
    QComboBox *m_actionFixModelCombo = nullptr;
    // Settings: full-width variables/secrets list.
    QWidget *m_varsList = nullptr;
    QVBoxLayout *m_varsListLayout = nullptr;
    // When true, the variables list shows secret values in clear text instead
    // of password masking. Toggled by the Reveal/Hide button.
    bool m_varsRevealed = false;
    QPushButton *m_varsRevealButton = nullptr;
    QCheckBox *m_actionsEnabledCheck = nullptr;
    QCheckBox *m_settingsActionsCheck = nullptr;
    QCheckBox *m_actionsAutoApproveCheck = nullptr;
    QCheckBox *m_settingsAutoApproveCheck = nullptr;
    QCheckBox *m_agentGuardrailsCheck = nullptr;
    QListWidget *m_agentAllowedDirectoriesList = nullptr;
    QPushButton *m_agentGuardrailRecommendedButton = nullptr;
    QLabel *m_agentGuardrailHint = nullptr;
    QCheckBox *m_settingsRequirePeerApprovalCheck = nullptr;
    QCheckBox *m_settingsMergeQueueCheck = nullptr;
    QCheckBox *m_secretScanCheck = nullptr;
    QCheckBox *m_repoPrivateCheck = nullptr;
    QCheckBox *m_repoServeCheck = nullptr;
    QWidget *m_collabSection = nullptr;
    QListWidget *m_collabList = nullptr;
    QLineEdit *m_collabEdit = nullptr;
    QLabel *m_collabEmptyHint = nullptr;
    QLineEdit *m_repoSourceEdit = nullptr;
    QLabel *m_repoSourceHint = nullptr;
    QLabel *m_repoForkLocation = nullptr;
    QLabel *m_repoMirrorLocation = nullptr;
    QTableWidget *m_repoRemotesTable = nullptr;
    QCheckBox *m_repoStatsFileCheck = nullptr;
    AgentStore *m_agentStore = nullptr;
    std::unique_ptr<AgentCapabilityBroker> m_agentCapabilityBroker;
    std::unique_ptr<AgentPermissionController> m_agentPermissionController;
    QList<AgentRunner *> m_agentRunners;
    QList<AgentSession> m_agentSessions;
    QList<int> m_agentQueue;
    // Models already tried for an in-flight teammate session. These are
    // runtime-only: once the session reaches a terminal outcome its persisted
    // model identifies the last attempt, while a new teammate run gets a new
    // session and a fresh fallback chain.
    QHash<int, QSet<QString>> m_teammateBackupAttempts;
    // Sessions moved onto another model because the provider rejected the
    // previous one (value: that model), by a teammate fallback or a composer
    // pick. Their next launch starts a fresh native conversation with the
    // saved transcript, since the rejected run may never have recorded the
    // task in its own thread.
    QHash<int, QString> m_agentModelHandoffs;
    // A streaming CLI is being torn down after a credit failure. Its queued
    // replacement waits briefly for that transport to release its run slot.
    QSet<int> m_teammateBackupTransportStops;
    // Sessions whose transport is being torn down to hand their slot back after
    // the concurrent-agent limit was lowered. Their run is parked, not finished,
    // so the finish bookkeeping and the queue pump both stand down until the
    // park lands them back in m_agentQueue.
    QSet<int> m_agentsParking;
    bool m_agentQueuePumpScheduled = false;
    QHash<int, bool> m_agentQueueAwaitingEvents;
    bool m_agentQuietResume = false;
    QSet<int> m_startupQuietAgentSessions;
    int m_selectedAgentSessionId = -1;
    int m_agentDetailTabSession = -1;

    struct AiConflictFix {
        PullStore *store = nullptr; // null in branch-merge mode
        int number = 0;       // PR number (PR mode)
        int repoIndex = -1;
        int sessionId = 0;    // backing agent session
        QString provider;     // "claude" | "openai" | "claude-code"
        QString model;
        QString apiKey;
        QString workTree;
        QStringList files;    // conflicted paths, repo-relative
        int index = 0;        // next file to resolve
        double costUsd = 0.0;
        qint64 inTokens = 0;
        qint64 outTokens = 0;
        bool claudeCode = false;
        QProcess *process = nullptr; // running CLI (claudeCode mode), else null
        bool branchMerge = false;
        QString branch;        // target branch being brought up to date
        QString baseBranch;    // base branch merged into it
        QString restoreBranch; // branch to check back out when done
        bool agentEdit = false;
        QString agentEditPrompt;
        int agentEditFindings = 0;
        QString promptFile;    // prompt handed to the CLI; removed when the run ends
    };
    AiConflictFix *m_aiFix = nullptr;
    void aiFixResolveNextFile();           // send the next conflicted file to the model
    void aiFixRunClaudeCode();             // run the `claude` CLI over the conflict tree
    void aiFixApplyResolved(const QString &resolved); // write back + advance
    void aiFixFinish();                    // commit to branch, mark session success
    void aiFixFail(const QString &message); // abort the merge, mark session failed
    void aiFixFinishBranchMerge();
    void aiFixFinishPull();
    void aiFixFailBranchMerge(const QString &message);
    void aiFixFailPull(const QString &message);
    bool m_aiFixFinishing = false;
    void aiFixLog(const QString &text);    // stream a line into the agent session log
    void aiFixSetSessionStatus(const QString &status, const QString &error = QString());
    // In-flight AI code review: one model
    // call over the PR's diff whose findings land as signed review threads. Null
    // when no review is running. Read-only towards the repo, so unlike m_aiFix it
    // holds no PullStore/git state — just the session bookkeeping.
    struct AiPullReview {
        int number = 0;       // PR under review
        int repoIndex = -1;
        int sessionId = 0;    // backing agent session (Agents-tab visibility)
        QString provider;     // "claude" | "openai" | "claude-code"
        QString model;
        double costUsd = 0.0;
        qint64 inTokens = 0;
        qint64 outTokens = 0;
        QProcess *process = nullptr; // running CLI (claude-code mode), else null
        QString output;              // accumulated CLI stdout (claude-code mode)
        QString promptFile;          // prompt handed to the CLI; removed on finish
    };
    AiPullReview *m_aiReview = nullptr;
    void reviewCurrentPullWithAi();        // "Review with AI" on the PR header
    void aiReviewRunClaudeCode(const QString &prompt); // CLI fallback (no API key)
    void aiReviewHandleReply(const QString &text); // parse findings + post threads
    void aiReviewFail(const QString &message);
    void aiReviewLog(const QString &text);
    void aiReviewSetSessionStatus(const QString &status,
                                  const QString &error = QString());
    void applyPullSuggestionFix(const QString &threadId);
    void fixCurrentPullFindingsWithAgent();
    QWidget *m_agentsPage = nullptr; // reserves room for the docked composer
                                     // while no transcript pane is open
    QTableWidget *m_agentTable = nullptr;
    ActivityRailButton *m_agentSortButton = nullptr;
    ActivityRailButton *m_agentTeammateFilterButton = nullptr;
    AgentTeammateFilter m_agentTeammateFilter = AgentTeammateFilter::All;
    ActivityRailButton *m_agentViewButton = nullptr;
    AgentListView m_agentListView = AgentListView::Compact;
    int m_agentCompactRowHeight = 0;
    ActivityRailButton *m_agentMoveToDesktopButton = nullptr;
    QSet<QString> m_cloudMovedSessionIds;
    QSet<QString> m_cloudLeftoverDeletes;
    bool m_cloudMovePumping = false;
    AgentListSort m_agentListSort = AgentListSort::RecentActivity;
    struct AgentTranscriptHit {
        int count = 0;
        QString snippet;
    };
    QTimer *m_agentTranscriptSearchTimer = nullptr;
    QString m_agentTranscriptSearchQuery;
    QString m_agentTranscriptSearchRepo;
    QHash<int, AgentTranscriptHit> m_agentTranscriptHits; // session id -> hit
    int m_agentTranscriptSearchGen = 0;
    void scheduleAgentTranscriptSearch(); // (re)start the debounce
    void runAgentTranscriptSearch();      // scan transcripts off-thread
    AgentTranscriptHit agentTranscriptHit(int sessionId) const;
    QString agentTranscriptSearchRepoKey() const;
    QString agentFilterQuery() const;
    bool agentTeammateFilterShows(const AgentSession &session) const;
    bool agentSessionMatchesQuery(const AgentSession &session,
                                  const QString &query) const;
    QString agentTranscriptQuery() const;
    QHash<int, QStringList> m_agentSessionImages; // session id -> image paths
    QHash<int, QString> m_agentImageStamps;       // session id -> scanned stamp
    int m_agentImageScanGen = 0;
    bool m_agentImageScanRunning = false;
    bool m_agentImageScanQueued = false;
    bool m_agentImageScanPending = true;
    QHash<QString, QIcon> m_agentThumbnails;
    // Rounded PR-author avatars by (pull, author, source pixmap/bytes) — the
    // running-row animation asked agentSessionPullAvatar() for a fresh
    // smooth-scaled (or synthesised + PNG round-tripped) medallion per row
    // every 60 ms tick.
    QHash<QString, QPixmap> m_agentPullAvatarCache;
    // refreshMirrorAdverts() skipped a pass because the Repos page was hidden.
    bool m_mirrorAdvertStale = false;
    QSet<int> m_pendingAgentSessionSaves;
    QTimer *m_agentSessionSaveTimer = nullptr;
    QJsonObject m_lastCodexRateLimits; // last payload applyCodexRateLimits() applied
    QJsonObject m_lastCodexRateLimitsByLimitId;
    bool m_nodesTableStale = false;    // refreshNodesTable() skipped an off-screen rebuild
    QHash<QString, QPixmap> m_chatMemberAvatarCache; // 28 px rounded member medallions
    QString m_chatMembersSignature; // what the member cards currently render
    bool m_chatMembersMenuOpen = false;
    // ensurePushHook(): checkout path -> ".git/config mtime|mirror" the push
    // URL was last confirmed against (skips a git process per repo per call).
    mutable QHash<QString, QString> m_pushUrlVerified;
    mutable QHash<QString, QString> m_pushUrlVerifying;
    mutable QHash<QString, RepositoryRecord> m_pushUrlRecheck;
    qint64 m_hostProbeLastMs = 0; // last probeSavedHosts() (SSH per saved host)
    QString m_lastCodexRateLimitsAccount;
    QSet<QString> m_agentThumbnailsPending;       // decodes in flight
    void scanAgentSessionImages(const QList<AgentSession> &sessions,
                                const QString &owner, const QString &name);
    QStringList agentSessionImages(int sessionId) const;
    QIcon agentThumbnail(const QString &path);
    void applyAgentThumbnail(const QString &path);
    void showAgentSessionImages(int sessionId);
    QComboBox *m_agentComposeRepo = nullptr;
    QLineEdit *m_agentComposePrompt = nullptr;
    QComboBox *m_agentComposeProvider = nullptr;
    QPushButton *m_agentComposeButton = nullptr;
    void startAgentFromComposer();
    QWidget *m_agentDetail = nullptr; // collapsible detail panel (hidden until a row is picked)
    // The detail pane's own bottom padding, kept so the docked composer's
    // reserved band can be added to it and taken back off again.
    int m_agentDetailBaseBottomMargin = 0;
    QLabel *m_agentTitle = nullptr;
    QWidget *m_agentPromptImages = nullptr;
    QStringList m_agentPromptImagePaths;
    int m_agentPromptImageSession = -1;
    int m_agentPromptImageRenderGen = 0;
    void renderAgentPromptImages(int sessionId);
    QToolButton *m_agentStatusPill = nullptr; // compact model + outcome control
    QLabel *m_agentMeta = nullptr;
    QFrame *m_agentMetaPopup = nullptr;
    QPushButton *m_agentPopOutButton = nullptr;
    QLabel *m_agentNetPanel = nullptr;   // live API-traffic graphic
    QPushButton *m_agentViewPrButton = nullptr;
    QPushButton *m_agentViewIssueButton = nullptr;
    QPushButton *m_agentViewTaskButton = nullptr;
    QPushButton *m_agentCreatePrButton = nullptr;
    QPushButton *m_agentCreateIssueButton = nullptr;
    QPushButton *m_agentMoveToCloudButton = nullptr;
    QPushButton *m_agentContinueHereButton = nullptr;
    QPushButton *m_agentRetryInCloudButton = nullptr;
    QPushButton *m_agentCloudMergeButton = nullptr;
    QPushButton *m_agentCloudCleanupButton = nullptr;
    QPushButton *m_agentCloudUpdateButton = nullptr;
    QPushButton *m_agentCloudActionsButton = nullptr;
    QPushButton *m_agentSignInRunnerButton = nullptr;
    // Sessions whose handoff POST is still in flight, so a second click on the
    // button cannot queue the same continuation twice.
    QSet<int> m_agentCloudHandoffsInFlight;
    QPlainTextEdit *m_agentLog = nullptr;
    QStackedWidget *m_agentOutputStack = nullptr; // log (0) | embedded terminal (1)
    TerminalWidget *m_agentTerminal = nullptr;  // Claude Code runs here
    int m_terminalSessionId = -1; // session currently driving the embedded terminal
    // Makes the app act as the IDE the `claude` CLI connects to:
    // a localhost WebSocket/JSON-RPC server advertised via ~/.claude/ide. Lazily
    // created on the first Claude Code terminal launch.
    ClaudeIdeBridge *m_ideBridge = nullptr;
    ClaudeIdeBridge *ensureIdeBridge();
    void onClaudeOpenDiff(const QString &tabName, const QString &oldPath,
                          const QString &newPath, const QString &newContents);
    ClaudeTranscriptView *m_agentTranscript = nullptr;
    ActivityRailButton *m_agentOutputModeButton = nullptr;
    bool m_agentRawOutputMode = false;
    void setAgentRawOutputMode(bool raw);
    // "Listen": the selected run's stream played as music (AgentSonifier.h).
    // One sonifier for the whole window, so any number of agents can be heard
    // at once, each in its own register and stereo seat.
    ActivityRailButton *m_agentListenButton = nullptr;
    forkmesh::sonify::AgentSonifier *m_agentSonifier = nullptr;
    // The finish is a status change, not a stream event -- and a run can end
    // from a dozen places -- so the ending is rung off a poll of the sessions
    // being listened to rather than off any one of those call sites.
    QTimer *m_agentListenStatusTimer = nullptr;
    QHash<int, QString> m_agentListenStatus;
    // Set once this machine has told us it cannot play anything, so the
    // automatic path stops asking on every run that starts.
    bool m_autoListenRefused = false;
    void toggleAgentListening(int sessionId);
    bool beginAgentListening(int sessionId, bool ringIfFinished = true);
    void autoListenToNewRun(int sessionId);
    void refreshAgentListenButton();
    void pollListenedAgentStatuses();
    // "Announcer": every agent at once, called out loud like a match
    // (AgentAnnouncer.h). The commentator is fed from the same event stream as
    // the sonifier and a twice-a-second look at the session list; a line is
    // only asked for when the voice has finished the one before.
    ActivityRailButton *m_agentAnnouncerButton = nullptr;
    // shared_ptr only for its type-erased deleter: the type is incomplete here.
    std::shared_ptr<forkmesh::announce::Commentator> m_agentCommentator;
    forkmesh::announce::Speaker *m_agentAnnouncerSpeaker = nullptr;
    forkmesh::announce::VoiceInstaller *m_agentAnnouncerInstaller = nullptr;
    QTimer *m_agentAnnouncerTimer = nullptr;
    bool m_agentAnnouncerClosing = false; // signing off; stops when the line ends
    int m_agentAnnouncerInstallPercent = -1;
    QString m_agentAnnouncerLastLine;
    bool agentAnnouncerOn() const;
    void toggleAgentAnnouncer();
    bool startAgentAnnouncer(bool remember);
    void stopAgentAnnouncer(bool signOff, bool remember);
    void tickAgentAnnouncer();
    // No voice installed yet: a menu under `anchor` offering to fetch it.
    // `forProse` says which feature to switch on once it is in.
    void offerAgentAnnouncerVoice(QWidget *anchor = nullptr, bool forProse = false);
    void installAgentAnnouncerVoice();
    void refreshAgentAnnouncerButton();
    // "Speak" on the activity window: every agent's reply read out as it
    // arrives, in the announcer's voice (spokenProse() for the wording). A
    // newer reply replaces one from the same agent still waiting its turn, and
    // nothing starts while the announcer is mid-sentence.
    forkmesh::announce::Speaker *m_agentProseSpeaker = nullptr;
    QTimer *m_agentProseTimer = nullptr;
    struct PendingProse {
        int sessionId = 0;
        QString who;  // how the agent is named when it is a new voice
        QString text; // spokenProse() of its reply
    };
    QList<PendingProse> m_agentProseQueue;
    QString m_agentProseLastWho; // who spoke last: a new voice is introduced
    bool m_agentProseAfterInstall = false; // the voice download was asked for by Speak
    bool agentProseSpoken() const;
    // False when there is no voice to speak with; nothing is started then.
    bool setAgentProseSpoken(bool on);
    void noteAgentProse(int sessionId, const QString &who, const QString &markdown);
    void tickAgentProse();
    forkmesh::sonify::AgentSonifier *ensureAgentSonifier();
    void installAgentActivityHost();
    bool listenToEverything(bool on);
    QSet<int> m_agentMutedMotions;
    void feedAgentActivity(int sessionId, const QJsonObject &event);
    void pumpExternalAgentActivity();
    QString externalActivityTranscript(int sessionId) const;
    QHash<int, qint64> m_agentActivityExternalOffset;
    QSet<int> m_agentActivityExternalReads;
    void tickMachineAgents();
    void scanMachineAgents();
    void applyMachineAgents(const QList<MachineAgent> &agents,
                            const QList<ClaudePeerSession> &peers = {},
                            const QList<TerminalHandoffSession> &terminals = {},
                            const QHash<QString, QList<qint64>> &surfacedPids = {});
    QTimer *m_machineAgentTimer = nullptr;
    QHash<int, MachineAgent> m_machineAgents;
    QList<ClaudePeerSession> m_claudePeerSessions;
    QList<TerminalHandoffSession> m_terminalHandoffSessions;
    QHash<QString, QList<qint64>> m_externalSurfacedPids;
    TerminalHandoff::TypingGate m_terminalTypingGate;
    bool m_machineAgentScanInFlight = false;
    quint64 m_machineAgentScanGeneration = 0;
    void noteAgentActivityTask(const forkmesh::BackgroundTicket &ticket, bool started);
    // "Code": the session's live diff in a pane right of the transcript — a
    // strip of the ten busiest files over the file list and per-file diffs,
    // each with a Viewed pill. It drives the files widgets below, so their
    // refresh runs while it shows.
    ActivityRailButton *m_agentCodeButton = nullptr;
    bool m_agentCodeMode = false;
    void setAgentCodeMode(bool code);
    QWidget *m_agentCodePage = nullptr;
    QSplitter *m_agentCodeSplit = nullptr;
    QWidget *m_agentCodeFileStrip = nullptr;
    QLabel *m_agentCodeViewedLabel = nullptr;
    QString m_agentCodeStripSignature;
    QTimer *m_agentCodePollTimer = nullptr;
    // Viewed marks per session: path -> that file's diff fingerprint when it
    // was ticked. The agent editing the file again changes the fingerprint and
    // reopens it. In memory only: a live run's review state.
    QHash<int, QHash<QString, QString>> m_agentCodeViewed;
    QHash<int, QSet<QString>> m_agentCodeExpanded;
    QHash<int, QHash<QString, QString>> m_agentCodeFingerprints;
    void onAgentCodeAnchorClicked(const QUrl &url);
    void renderAgentCodeFileStrip(int sessionId,
                                  const QList<forkmesh::ui::DiffFileEntry> &files);
    QLineEdit *m_transcriptSearch = nullptr;
    QLabel *m_transcriptSearchCount = nullptr;
    QListWidget *m_agentFilesList = nullptr;     // files edited in this session
    QWidget *m_agentFilesPanel = nullptr;        // wraps the list + heading
    QTabWidget *m_agentDetailTabs = nullptr;
    int m_agentFilesTabIndex = -1;               // tab index of "Files changed"
    // Agents page stack: 0 = sessions (the list/detail splitter), 1 = the
    // live per-tool cost board, opened from the list's action bar.
    ActivityRailButton *m_agentEfficiencyButton = nullptr;
    ActivityRailButton *m_agentGuardrailsButton = nullptr;
    ActivityRailButton *m_agentModelsButton = nullptr;
    AgentModelTable *m_agentModelTable = nullptr;
    QLabel *m_agentModelLive = nullptr;
    std::shared_ptr<AgentModelGitHistory> m_agentModelGitHistory;
    QStringList m_agentModelGitRepos;
    qint64 m_agentModelGitScannedMs = 0;
    bool m_agentModelGitScanning = false;
    QStackedWidget *m_agentPageStack = nullptr;
    // Family details beside the board, opened by clicking a card.
    QWidget *m_agentEfficiencyDetailsPanel = nullptr;
    QLabel *m_agentEfficiencyDetailsTitle = nullptr;
    QTextBrowser *m_agentEfficiencyDetails = nullptr;
    AgentToolEfficiencyBoard *m_agentEfficiencyBoard = nullptr;
    AgentToolUsageTable *m_agentToolUsageTable = nullptr;
    QStackedWidget *m_agentEfficiencyViewStack = nullptr; // Families | Tools | Runs
    QPushButton *m_agentEfficiencyFamiliesViewButton = nullptr;
    QPushButton *m_agentEfficiencyToolsViewButton = nullptr;
    QPushButton *m_agentEfficiencyRunsViewButton = nullptr;
    AgentRunTable *m_agentRunTable = nullptr;
    std::unique_ptr<AgentRunEfficiencyLog> m_agentRunLog;
    QHash<int, QStringList> m_agentRunDisabledFamilies;
    QLabel *m_agentEfficiencyLive = nullptr;
    QTimer *m_agentEfficiencyRefreshTimer = nullptr;
    QTimer *m_agentEfficiencySaveTimer = nullptr;
    bool m_agentEfficiencyDirty = false;
    // Owner-device ledger of tool calls, context consumed, and measured
    // savings; persisted beside the agent store.
    std::unique_ptr<AgentToolEfficiencyLedger> m_agentToolLedger;
    // Guardrails tab: counts of blocked tool calls, Approve/Deny answers, and
    // agents stopped by a tightened boundary, beside each repository policy.
    AgentGuardrailBoard *m_agentGuardrailBoard = nullptr;
    QLabel *m_agentGuardrailLive = nullptr;
    QTimer *m_agentGuardrailRefreshTimer = nullptr;
    QTimer *m_agentGuardrailSaveTimer = nullptr;
    std::unique_ptr<AgentGuardrailLedger> m_agentGuardrailLedger;
    // Realtime log of potential tool-use improvements the ledger's calls
    // reveal; appended to tool-improvements.jsonl beside the ledger.
    std::unique_ptr<AgentToolImprovementLog> m_agentToolImprovements;
    QTextBrowser *m_agentDiffView = nullptr;     // diff viewer in the files tab
    forkmesh::ui::DiffFileNavigator *m_agentDiffNav = nullptr; // sticky header + scroll<->select
    QLabel *m_agentFilesChangedSummary = nullptr; // "N files changed" line
    QLabel *m_agentCommitsHeading = nullptr;     // "Commits" heading over the list
    QListWidget *m_agentCommitsList = nullptr;   // this branch's commits, ahead of base
    int m_agentDiffRenderedSession = -1;
    QString m_agentDiffLastHtml;
    QString m_agentDiffRenderKey;
    QByteArray m_agentDiffRenderedPatch;
    forkmesh::ui::SecretDiffBanner *m_agentSecretBanner = nullptr;
    QList<RepoSecurityFinding> m_agentDiffSecrets;
    int m_agentDiffSecretsSession = -1;
    QPushButton *m_agentMergeButton = nullptr;   // worktree: merge into main
    QPushButton *m_agentMergeDeleteButton = nullptr; // merge + delete agent too
    QPushButton *m_agentUpdateButton = nullptr;  // worktree: update from main
    QPushButton *m_agentWtDeleteButton = nullptr; // worktree: delete worktree+branch
    QTimer *m_agentHourlyTimer = nullptr;        // refreshes spend + files hourly
    // relay owner-encrypted session snapshots and drain encrypted
    // steering prompts. m_agentSyncDebounceTimer is a singleShot
    // re-armed after a status flip so a burst of updates coalesces into one push.
    QTimer *m_agentSyncDebounceTimer = nullptr;
    QHash<QString, QByteArray> m_lastAgentPushPayload;
    QSet<QString> m_agentSnapshotReadsInFlight;
    QSet<QString> m_agentSnapshotReadsPending;
    QSet<QString> m_agentE2EEReady;
    QSet<QString> m_agentE2EEInFlight;
    // Companion owner-device public bundles (the account's other enrolled
    // devices, e.g. its mobile node), keyed by this desktop's own owner key
    // id. Session snapshots are sealed to the vault key plus these so a
    // companion can open them; queued prompts still require the vault key.
    // Refreshed when stale so a newly enrolled device starts receiving
    // snapshots without a desktop restart.
    QHash<QString, QList<QJsonObject>> m_companionOwnerBundles;
    // The same devices as key ids, so a refresh can tell a changed recipient
    // set from an unchanged one without re-deriving digests from the bundles.
    QHash<QString, QStringList> m_companionOwnerBundleKeyIds;
    QHash<QString, qint64> m_companionOwnerBundlesFetchedAtMs;
    QSet<QString> m_companionOwnerBundleFetchesInFlight;
    QSet<QString> m_orgAgentJobsInFlight;
    QSet<QString> m_orgAgentJobDrainsInFlight;
    QSet<QString> m_orgAgentJobDrainsPending;
    // local AgentSession id -> start-job transport needed to publish terminal
    // status through the same authenticated lease after the run finishes. The
    // same object is persisted on AgentSession for restart recovery.
    QHash<int, QJsonObject> m_orgAgentBindings;
    QHash<int, QString> m_orgTaskAgentStatusSent;
    QHash<int, QString> m_orgTaskAgentStatusPending;
    QSet<int> m_orgTaskOpenPending;
    bool m_orgTaskAgentStatusFlushQueued = false;
    qint64 m_agentPresencePausedUntilMs = 0;
    QHash<int, size_t> m_officeProseSent;
    QTimer *m_officeProseTimer = nullptr;
    QList<QList<QPair<int, QString>>> m_orgTaskAgentStatusInFlight;
    QTimer *m_orgTaskAgentStatusAckTimer = nullptr;
    QHash<int, ClaudeStreamSession *> m_streamSessions;
    QHash<int, CodexAppServerSession *> m_codexStreams;
    QHash<int, QList<QJsonObject>> m_streamEvents;
    int m_renderedTranscriptSession = -1;
    int m_renderedTranscriptCount = -1;
    int m_transcriptSkipped = 0;
    // Replaying a stored transcript creates hundreds of rich widgets. Keep each
    // event-loop slice short so selecting an agent never monopolises input.
    quint64 m_agentTranscriptRenderGeneration = 0;
    int m_agentTranscriptRenderSession = -1;
    QList<QJsonObject> m_agentTranscriptRenderEvents;
    int m_agentTranscriptRenderCursor = 0;
    int m_agentTranscriptRenderSkipped = 0;
    bool m_agentTranscriptSkippedNoticeAdded = false;
    QHash<int, int> m_agentTranscriptRenderLimits;
    int m_agentTranscriptScrollRestoreSession = -1;
    int m_renderedExternalSession = -1;
    int m_agentLogSession = -1;
    QString m_agentLogText;
    QHash<int, QString> m_streamRaw;
    QHash<int, qint64> m_sessionTokens; // live token total per session, for the list
    QHash<int, forkmesh::transcripttools::OutputTokenCounter> m_sessionOutputTokens;
    QHash<int, AgentScannerState> m_scannerStates;
    QHash<int, AgentSpinState> m_agentSpinStates; // fireball orbit, per session
    QElapsedTimer m_agentSpinClock; // real time between fireball animation frames
    QTimer *m_scannerTimer = nullptr;
    QHash<int, AgentDiffStat> m_agentDiffStats;
    QHash<int, QString> m_agentDiffSig;
    bool m_agentDiffRefreshPending = false;
    // A reload that a UI-refresh batch deferred replays once, and that replay
    // must not invent a rescan nobody asked for — nor drop one that was.
    bool m_agentRescanDeferred = false;
#ifdef FORKMESH_WINDOW_TESTS
    int m_testAgentListRescanCount = 0;
#endif
    bool m_agentDiffStatsRefreshing = false;
    bool m_agentDiffStatsRefreshQueued = false;
    int m_agentDiffStatsGen = 0;
    QSet<int> m_agentLiveDiffSessions;
    bool m_agentDiffLivePass = false;
    bool m_agentDiffCooldownWaived = false;
    QElapsedTimer m_agentDiffPassClock;
    QElapsedTimer m_agentDiffPassEnded;
    qint64 m_agentDiffPassMs = 0;
#ifdef FORKMESH_WINDOW_TESTS
    int m_testAgentDiffPassCount = 0;
#endif
    // Edit/Write/Bash calls whose result has not come back yet, per session:
    // the result is when the file on disk has changed and the row's file
    // count is worth measuring again.
    QHash<int, int> m_agentPendingFileTools;
    QTimer *m_agentLiveDiffTimer = nullptr;
    // Re-entrancy guard for refreshAgentTable(): its cold-cache Diff cells shell
    // git and pump the event loop (GitKeepAlive), so a queued slot can re-enter
    // and corrupt the half-built table unless we skip the nested rebuild.
    bool m_agentTableRefreshing = false;
    QHash<int, QString> m_lastAssistantText; // last assistant prose, for waiting/question
    QSet<int> m_agentCompletionChecks;
    // The drain window between the CLI's `result` and a genuinely finished run:
    // what the agent left running, whether it is still doing work, and how long
    // we have been waiting on it.
    struct AgentBackgroundWork {
        int count = 0;
        bool busy = true;      // its CPU time moved since the previous poll
        QString summary;       // "ninja (PID 1234), cc1plus (PID 1240)"
        qint64 cpuTicks = 0;
        qint64 startedAtMs = 0;
        qint64 lastBusyAtMs = 0;
        qint64 killedAtMs = 0; // 0 until the straggler stop signal goes out
        int polls = 0;
        bool announced = false; // the transcript already says we are waiting
        QString shown;          // last state pushed to the list/pill/transcript
    };
    QHash<int, AgentBackgroundWork> m_agentBackgroundWork;
    QHash<int, forkmesh::agentclitasks::Ledger> m_agentCliTasks;
    QSet<int> m_agentResultAwaitingCliTasks;
    void trackAgentCliTask(int sessionId, const QJsonObject &event);
    void settleAgentAfterCliTasks(int sessionId);
    QHash<int, int> m_agentRelaunchAttempts;
    void notifyAgentWaiting(int sessionId, bool needsPermission);
    // The counterpart for a run that reached the end: a ping on the Pings page
    // carrying the summary it signed off with, plus a green pulse round the
    // window. Every completion path funnels
    // through here, and m_agentDoneNotified keeps a re-fired signal or a requeue
    // from celebrating the same run twice.
    void notifyAgentDone(int sessionId);
    QString agentClosingSummary(int sessionId) const;
    QSet<int> m_agentDoneNotified; // sessions already celebrated; cleared on a new turn
    bool applyCliExitWithoutResult(int sessionId, bool codex, int exitCode);
    void markAgentSessionRunning(int sessionId);
    QHash<int, QStringList> m_streamFiles;
    QHash<int, QString> m_streamWorktree;        // sessionId -> worktree path
    QHash<int, QThread *> m_worktreeTeardown;
    QHash<int, QStringList> m_streamPending;
    // sessionWorkdir() cache for *reloaded* sessions (not in m_streamWorktree):
    // resolving their worktree shells `git worktree list`, which the Files-changed
    // panel drove on every transcript turn — a per-event subprocess that stalled
    // the GUI thread. A session's branch->worktree binding is fixed
    // for its lifetime, so cache it; re-resolve only if the path was since removed.
    QHash<int, QString> m_sessionWorkdirCache;   // sessionId -> resolved worktree ("" = none)
    QHash<int, QString> m_pendingSteerMessage;
    QHash<int, QString> m_inFlightSteerMessage;
    // Provider streams can replay an item while reconnecting. Key alerts by
    // session and tool-use id so one attempted call produces one decision.
    QSet<QString> m_agentGuardrailAlertedToolUses;
    QHash<int, AgentSession> m_streamSessionInfo;
    QHash<int, QString> m_streamAccountId;
    void startCliTranscript(AgentSession &session, const Issue &issue,
                            const QString &repoPath,
                            const QString &customPrompt = QString(),
                            bool switchToTab = true,
                            const RepositoryRecord *repository = nullptr);
    // startCliTranscript's stages. The plan is what the deferred launch (run
    // once the worktree exists) needs from the preparation step.
    struct CliRuntimeLaunchPlan {
        int sessionId = 0;
        QString prompt;
        QString resumeFallbackPrompt;
        bool autoMode = false;
        QString branchName;
        QString resumeId;
        QString selectedModel;
        QString routeTask;
        bool codex = false;
        QString sessionMode;
        QString sessionStrength;
        QStringList runAccountEnv;
        forkmesh::agentguard::Config guardrails;
    };
    forkmesh::agentguard::Config cliTranscriptGuardrails(
        const AgentSession &session, const QString &repoPath,
        const RepositoryRecord *repository);
    void resolveCliTranscriptBase(AgentSession &session, const QString &repoPath);
    void handleCliRuntimeEvent(int sid, const QJsonObject &ev,
                               const forkmesh::agentguard::Config &guardrails);
    void finishCliTranscriptRun(int sid, bool codex, int exitCode);
    bool desktopStopping() const;
    bool keepCliRunForNextStart(int sid, bool codex, int exitCode);
    void applyListedCodexModels(const QJsonArray &models);
    void launchCliRuntime(const CliRuntimeLaunchPlan &plan, const QString &workdir);
    void failCliTranscriptLaunch(int sid, const QString &why);
    void startCliTranscriptWorktree(int sid, const QString &repoPath,
                                    const QString &worktreeName, int issueNumber,
                                    const QString &branchName, const QString &baseRef,
                                    const QString &resumeId,
                                    std::function<void(const QString &)> launch);
    void resolveAutoClaudeModel(int sessionId, const QString &task,
                                const QString &workdir, ClaudeStreamSession *live,
                                std::function<void(const QString &)> launch,
                                const forkmesh::agentguard::Config &guardrails);
    void runClaudeAutoTriageRung(int sessionId, int rung, bool errorsOnly,
                                 const QString &task, const QString &workdir,
                                 ClaudeStreamSession *live,
                                 std::function<void(const QString &)> launch,
                                 const forkmesh::agentguard::Config &guardrails);
    void applyTranscriptEvent(int sessionId, const QJsonObject &ev);
    void auditAgentGuardrailEvent(int sessionId, const QJsonObject &ev,
                                  const forkmesh::agentguard::Config &guardrails);
    void showAgentGuardrailAlert(
        int sessionId, const QString &tool, const QString &path,
        const QString &directory);
    bool approveAgentGuardrailDirectory(int sessionId,
                                        const QString &directory);
    void recordAgentToolEfficiency(int sessionId, const QJsonObject &ev);
    void scheduleAgentEfficiencyRefresh();
    void refreshAgentEfficiencyBoard();
    void setAgentPageTab(int index);
    QWidget *buildAgentModelsPage();
    void refreshAgentModelBoard();
    void startAgentModelGitScan(bool force);
    // The efficiency page's Tools view: every tool and command, sortable.
    void showAgentToolUsageTable(bool on);
    void showAgentEfficiencyView(int index);
    void noteAgentRunToolConfig(int sessionId, const QJsonObject &launchOptions);
    // Transcript command chips (ClaudeTranscriptView::CommandApproval): the
    // resolver that decides each chip, and the dialog a chip opens listing
    // the run's commands and every permission that applies to them.
    void installAgentCommandApprovals();
    void showAgentCommandPermissions(const QString &toolUseId);
    QPointer<QDialog> m_agentCommandPermissionsDialog;
    void showAgentToolFamilyDetails(const QString &family);
    void refreshAgentToolFamilyDetails();
    QWidget *buildAgentEfficiencyPage();
    // "Send to agent" on the Tool efficiency page: the improvement digest
    // wrapped in the ask to fix every signal at its source. Empty while
    // nothing has been recorded, so there is nothing to launch on.
    QString agentToolImprovementsFixPrompt() const;
    void sendAgentToolImprovementsToAgent();
    QWidget *buildAgentGuardrailPage();
    void scheduleAgentGuardrailRefresh();
    void refreshAgentGuardrailBoard();
    void openRepositoryGuardrailSettings(const QString &repository);
    // Operator tool policy: families switched off in the Tool efficiency
    // tab. Stored in QSettings, mirrored into the ledger and board, and
    // handed to every runtime launch as AgentRuntimeLaunch.options.
    QStringList disabledAgentToolFamilies() const;
    void setAgentToolFamilyEnabled(const QString &family, bool enabled);
    QJsonObject agentRuntimeLaunchOptions() const;
    // Project settings > Agents: the folder lock and the external-call gate
    // every agent this process launches carries (ProjectAgentAccess.h,
    // AgentNetworkGate.h). Read at each launch, so a saved change applies to
    // the next run.
    void applyProjectAgentAccess(forkmesh::agentguard::Config &guardrails) const;
    // Environment entries that point a launch's hook at the gate; empty when
    // the project lets external calls through or the gate cannot listen.
    QStringList agentNetworkGateEnv(int sessionId);
    void handleAgentExternalCall(const forkmesh::agentnet::Request &request);
    // A tool call that failed to reach a folder or host (AgentAccessFailure.h):
    // the agent waits on a prompt that can grant the folder, pin the host's
    // address in /etc/hosts, or tell it to retry or to go without.
    void handleAgentAccessFailure(const forkmesh::agentnet::Request &request);
    void checkAgentHostResolution(const QString &host, QPointer<QMessageBox> box,
                                  QPointer<QPushButton> pin, const QString &evidence,
                                  const QString &waits,
                                  std::shared_ptr<QString> pinnedAddress);
    void pinAgentHost(const QString &id, int sid, const QString &host,
                      const QString &address);
    std::optional<bool> projectDecisionForCommand(int sid, const QString &command);
    forkmesh::projects::GlobalAgentRules globalAgentRules() const;
    forkmesh::projects::AgentAccess effectiveProjectAgentAccess() const;
    forkmesh::agentrules::RunRules agentRunRules(int sid) const;
    bool agentRunGatesCommands(int sid) const;
    QSet<QString> agentRunApprovals(int sid) const;
    QJsonObject agentRunLaunchOptions(int sid, bool codex) const;
    QStringList agentRunToolEnvironment(int sid) const;
    void startAgentRunner(const AgentSession &session, const Issue &issue,
                          const QString &repoPath, AgentRunner::Config config);
    void answerAgentGateRequest(const forkmesh::agentnet::Request &request,
                                const forkmesh::agentrules::Decision &decision);
    void promptAgentDestructiveCommand(const forkmesh::agentnet::Request &request,
                                       const forkmesh::agentrules::Decision &decision);
    void answerAgentAccessFailure(const QString &id, const QString &context);
    void reportCodexAccessFailure(int sid, const QJsonObject &item);
    void showGlobalAgentRules();
    QHash<int, QPair<QString, QStringList>> m_teammateRunGrants;
    QPair<QString, QStringList> m_startingTeammateGrant;
    bool m_startingTeammate = false;
    quint64 m_codexAccessFailureSeq = 0;
    std::unique_ptr<forkmesh::agentnet::Server> m_agentNetworkGate;
    QStringList agentIntakeEnv(int sessionId);
    QJsonObject handleAgentIntakeRequest(const forkmesh::agentintake::Request &request);
    QJsonObject createAgentFromIntake(int requesterId,
                                      const forkmesh::agentintake::CreateRequest &request);
    QString agentIntakeSessionState(int sessionId) const;
    bool startHeldAgentSession(int sessionId);
    std::unique_ptr<forkmesh::agentintake::Server> m_agentIntake;
    bool m_agentIntakeFailed = false;
    QHash<int, QString> m_agentIntakeTokens;
    forkmesh::agentintake::RateLimiter m_agentIntakeLimiter;
    std::unique_ptr<forkmesh::agentwindows::Registry> m_agentWindows;
    QJsonObject handleAgentWindowRequest(int sessionId,
                                         const forkmesh::agentintake::Request &request);
    // Destinations the user allowed for one run, by session token.
    QHash<QString, QSet<QString>> m_agentGateSessionHosts;
    QHash<QString, QPointer<QDialog>> m_agentGatePrompts;
    // Access failures the user declined, by session token ("folder:/path",
    // "resolve:host"); the agent is told again without asking again.
    QHash<QString, QSet<QString>> m_agentAccessDeclined;
    QString lastClaudeSessionId(int sessionId) const;
    QString lastCodexThreadId(int sessionId) const;
    QString agentCliConversationId(int sessionId);
    struct AgentTerminalHandoff {
        bool ok = false;
        QString program;
        QStringList args;
        QString cwd;
        QMap<QString, QString> env;
        QString conversationId;
        QString blocker;
        QString code;
    };
    AgentTerminalHandoff agentTerminalHandoff(int sessionId);
    void popOutAgentSessionToTerminal(int sessionId);
    // "Move to cloud": what this session would need for another of the
    // account's nodes to carry it on, and whether this one can hand it over.
    // The transcript is not part of it -- it already reaches the account's
    // other devices owner-sealed on the agent snapshot transport, and the work
    // itself travels in Git on the published branch.
    struct AgentCloudHandoff {
        bool ok = false;
        QString blocker;
        int repoIndex = -1;
        QString owner;       // repository owner (the signing account)
        QString name;        // repository name
        QString branch;
        QString baseBranch;
        QString baseRef;
        QString provider;
        QString model;
        QString prompt;      // the task the cloud agent carries on with
        QString title;
        QString worktree;    // this session's tree, committed before publishing
        QString mirrorPath;  // served bare mirror the branch is pushed into
        int issueNumber = 0;
        bool createPr = false;
    };
    // resolveWorktree=false stops before the one gate that shells git.
    AgentCloudHandoff agentCloudHandoff(int sessionId,
                                        bool resolveWorktree = true);
    bool publishAgentHandoffBranch(const AgentCloudHandoff &plan,
                                   int sessionId, QString *error);
    void moveAgentSessionToCloud(int sessionId);
    void finishAgentCloudHandoff(int sessionId, const QString &branch,
                                 const QJsonObject &payload);
    void showAgentListContextMenu(const QPoint &viewportPos);
    QMenu *buildAgentListContextMenu(int sessionId, QWidget *parent);
    void renderTranscriptForSession(int sessionId);
    void renderTranscriptSlice(quint64 generation);
    bool renderTranscriptTail(int sessionId);
    void openAgentCommandTerminal(const QString &command,
                                  const QString &recordedCwd);
    void loadEarlierTranscriptEvents();
    void reapplyTranscriptSearch();
    void refreshAgentFilesPanel(int sessionId);
    void populateAgentFilesPanel(int sessionId, const QStringList &diffFiles);
    void scheduleAgentFilesDiff(int sessionId);
    struct AgentDiffProbe {
        QByteArray patch;        // git diff <base>, limited to agent-owned paths
        bool patchOk = false;    // that read succeeded (else keep the old view)
        QSet<QString> uncommitted; // paths with working-tree changes / untracked
        QSet<QString> untracked;   // new files `git diff` cannot show
        QSet<QString> ownedPaths;  // paths from patch-unique, non-merge commits
        QStringList commitLines; // patch-unique "abc1234 subject" entries
        int behind = 0;          // commits the base branch has that we don't
        int pending = 0;         // async probes still in flight
    };
    AgentDiffProbe m_agentDiffLastProbe; // re-rendered when a Viewed pill flips
    void renderAgentDiff(int sessionId, const AgentDiffProbe &probe);
    void applyAgentDiff(int sessionId, const AgentDiffProbe &probe,
                        const QList<forkmesh::ui::DiffFileEntry> &files,
                        const QString &shown);
    void updateAgentFilesTabState(int sessionId);
    QString sessionBaseRef(int sessionId);
    QString sessionBaseBranch(int sessionId);
    QString sessionDiffBase(int sessionId, const QString &dir);
    QTimer *m_agentFilesDiffTimer = nullptr; // debounces the async working-tree diff
    void maybeCreatePullForStreamSession(int sessionId,
                                         std::function<void()> then = {});
    QHash<int, QList<std::function<void()>>> m_agentPullCaptures;
    QHash<int, QString> m_agentPullFailures;
    QSet<int> m_agentPullNotNeeded;
    QSet<int> m_agentPullSkipOnce;
    // Land a finished agent session's change as a pull request. On the
    // source of truth (we own the repo with a working tree) the PR is created and
    // committed locally. On a mirror node we can't write the owner's repo, so the
    // PR is signed and delivered to the owner's relay inbox instead — so a looper
    // running on a mirror still gets its work to the source of truth. `commits` is
    // the optional format-patch mbox the owner replays to preserve authorship;
    // pass empty when none is available (the owner synthesizes one from the patch).
    // Takes the session by value: creating the PR pumps the GUI event loop, and a
    // reloadAgents() during the pump would leave a reference dangling.
    void publishAgentPullRunnerRef(const AgentSession &session,
                                   const RepositoryRecord &repo, int pr);
    void landAgentPullForSession(AgentSession session, const QString &patch,
                                 const QString &commits);
    bool isStreamTranscriptSession(int sessionId) const;
    bool ensureStreamEventsLoadedAsync(int sessionId);
    QSet<int> m_streamEventsLoading; // async loads in flight
    QSet<int> m_streamEventsAbsent;  // probed: nothing persisted on disk
    void stopErroredStreamTransport(int sessionId);
    void stopStreamSession(
        int sessionId, bool refreshUi = true,
        const QString &reason = QStringLiteral("Stop requested by user."));
    void purgeSessionState(int sessionId);
    QString sessionWorkdir(int sessionId);
    // A session's dedicated worktree path ("" when its branch has no separate
    // worktree / is the main checkout), resolved without re-shelling `git worktree
    // list` on every click. Sessions launched this run know it from
    // m_streamWorktree; reloaded ones resolve once via git and cache it for the
    // session's lifetime (m_sessionWorkdirCache). See the definition for why the
    // click path leaned on this.
    QString cachedSessionWorktree(int sessionId, const QString &repoLocal,
                                  const QString &branch);
    QString cachedSessionWorktreeForUi(int sessionId,
                                       const QString &repoLocal,
                                       const QString &branch);
    QSet<int> m_sessionWorkdirUiReads;
    void cleanupStreamWorktree(int sessionId);
    // A worktree directory can only have one remover: cleanupStreamWorktree()
    // tears one down from a background thread, removeWorktree() from this one,
    // and when both ran for the same path the loser reported a failure for a
    // worktree that had just been removed. Whoever is about to remove a path
    // releases every session's record of it first (returning the sessions that
    // held it, so a cleanup that bails before removing anything can retrack it).
    QList<int> releaseTrackedWorktree(const QString &worktreePath);
    void retrackWorktree(const QList<int> &sessionIds, const QString &worktreePath);
    void maybeAutoMergeForSession(int sessionId);

    // ---- Organization tasks for prompted runs ------------------
    // A prompt typed here starts an agent locally; with the composer's "Task"
    // toggle on it also opens a task in the organization so the run is visible
    // beyond this desktop. The task carries the run's provenance — the bot that
    // launched it, the bot that reported it finished, and the model, permission
    // mode, and reasoning strength it used.

    QString agentBotLabel(const QString &provider) const;
    QString composerAgentStrength() const;
    // Authenticate one task write: the account session token when a password
    // login produced one, otherwise a proof signed with this node's account key
    // appended to `url` as node/ts/sig. The silent-auth launch path has no
    // token at all, so without the signed fallback the Task toggle would do
    // nothing for most desktops. `resource` names the task a proof is bound to
    // (empty for the collection). False => this node can present neither.
    bool authenticateOrgTaskRequest(QUrl &url, QNetworkRequest &request,
                                    const QString &proofPrefix,
                                    const QString &resource) const;
    // The node/ts/sig triple behind that signature, for a write that has no
    // URL to hang it on — the batched agent-status report travels as a node
    // event socket frame. Same canonical string, so the relay verifies it with
    // the same code either way. False => this node cannot sign.
    bool signOrgTaskProof(const QString &proofPrefix, const QString &resource,
                          QString *node, QString *ts, QString *sig) const;
    void openOrgTaskForSession(const AgentSession &session);
    bool postOrgTaskForSession(const AgentSession &session, const QString &title,
                               bool manual, bool completeIfFinished);
    bool canLinkAgentSessionToOrgTask(int sessionId);
    void addAgentTaskLinkActions(QMenu *menu, int sessionId);
    void createOrgTaskForAgentSession(int sessionId);
    void attachAgentSessionToOrgTask(int sessionId);
    void showAgentSessionOrgTaskPicker(int sessionId, const QJsonArray &tasks);
    void linkAgentSessionToOrgTask(int sessionId, const QString &taskId,
                                   bool completeIfFinished);
    void detachAgentSessionFromOrgTask(int sessionId);
    QString unlinkAgentSessionFromOrgTask(int sessionId);
    void resolveAgentSessionTaskWorktree(int sessionId);
    void syncAgentViewTaskButton(int sessionId);
    void showAgentTaskLinkMenu(int sessionId);
    // Report the board task the line above just opened: into the "Prompt sent"
    // card raised for this session when it is still up, and into the log
    // regardless.
    void announceOrgTaskOpened(int sessionId);
    // Queue one session's live state for the board. The write itself is
    // batched: every queued session goes out together in one signed
    // agent-status report, so a restart publishes the whole fleet at once
    // instead of one request per session. That report rides the
    // node event socket this desktop already holds — no HTTP request at all —
    // and falls back to POST /api/tasks/agent-status when the socket is down
    // or the relay is too old to accept the frame.
    void syncOrgTaskAgentStatus(int sessionId);
    void flushOrgTaskAgentStatus();
    bool agentPresenceWanted(const AgentSession &session) const;
    void syncOfficeAgentProse();
    void applyOrgTaskAgentStatusResult(const QList<QPair<int, QString>> &sent,
                                       bool ok, const QJsonArray &results);
    void onOrgTaskAgentStatusFrame(bool ok, const QJsonArray &results);
    void requeueOrgTaskAgentStatusInFlight();
    void completeOrgTaskForSession(int sessionId,
                                   const QString &followUp = QString(),
                                   bool evenIfAttached = false);
    void recordOrgTaskFields(int sessionId, const QString &taskId,
                             const QString &finishedByBot,
                             bool completeIfFinished = true);

    static constexpr int kExternalIdBase = -1000;
    QTimer *m_externalClaudeTimer = nullptr;
    QList<ExternalClaudeSession> m_externalClaude;        // detected for the open repo
    QHash<QString, int> m_externalTempId;                 // uuid -> synthetic id
    QHash<int, ExternalClaudeSession> m_externalSurfaced; // synthetic id -> session
    QHash<int, QString> m_externalSurfacedRepo;           // synthetic id -> "owner/name"
    QHash<int, qint64> m_externalReadOffset;              // synthetic id -> tail offset
    QSet<int> m_externalTranscriptReads;
    QHash<int, bool> m_externalTranscriptReadFull;
    QSet<int> m_externalTranscriptFullQueued;
    quint64 m_externalTranscriptRenderGeneration = 0;
    int m_externalTranscriptRenderSession = -1;
    QList<QJsonObject> m_externalTranscriptRenderEvents;
    int m_externalTranscriptRenderCursor = 0;
    bool m_externalTranscriptRenderFull = false;
    qint64 m_externalTranscriptRenderTokens = 0;
    QString m_externalTranscriptRenderRaw;
    QString m_externalSig;          // last list/spinner signature, to skip no-op rebuilds
    int m_nextExternalTempId = kExternalIdBase;
    bool m_externalClaudeScanInFlight = false;
    quint64 m_externalClaudeScanGeneration = 0;
    void scanExternalClaudeSessions();
    void applyExternalClaudeScan();
    void onExternalClaudeTick();
    void injectExternalSessions();          // append surfaced temp entries to the list
    int registerExternalSession(const QString &uuid); // auto-create a temp list entry
    void fileExternalSessionInFolder(const QString &repoKey, const QString &uuid);
    void surfaceExternalSession(const QString &uuid); // click -> jump to its entry
    void unsurfaceExternalSession(int sessionId);     // remove a temp entry
    void deleteExternalSession(int sessionId); // kill the CLI process, then unsurface
    int externalTempIdFor(const QString &uuid);
    bool isExternalSession(int sessionId) const { return sessionId <= kExternalIdBase; }
    // ---- Cloud agent sessions (MainWindowAgentCloud.cpp) ------------------
    // Runs the account queued on its mirrors, shown in the Agents list beside
    // this desktop's own. Like an external session a cloud row is not backed by
    // the store, so its id sits inside the external band — every guard that
    // keeps an external row away from the store, git and the local queue covers
    // it too — but far enough down that the two never meet.
    static constexpr int kCloudIdBase = -1000000;
    bool isCloudSession(int sessionId) const { return sessionId <= kCloudIdBase; }
    QHash<int, forkmesh::agents::CloudAgentSession> m_cloudAgentSessions;
    QHash<QString, int> m_cloudAgentIds; // CloudAgentSession::key() -> row id
    int m_nextCloudAgentId = kCloudIdBase;
    // The account's queue is read whole, so both of these hold one entry
    // (kCloudAccountKey in MainWindowAgentCloud.cpp), not one per repository.
    QSet<QString> m_cloudAgentFetchesInFlight;
    QHash<QString, qint64> m_cloudAgentFetchedAtMs;
    QSet<int> m_cloudAgentWritesInFlight;
    // Cloud runs whose full conversation has been read from their own route,
    // the account list having carried only its tail.
    QSet<int> m_cloudAgentDetailFetched;
    QSet<QString> m_cloudAgentImagesInFlight;
    QSet<QString> m_cloudAgentImagesGone;
    bool m_cloudAgentRefreshHooked = false; // Agents-tab open hook connected
    QTimer *m_cloudAgentWatchTimer = nullptr;
    // The first answer is this desktop learning what already exists; only the
    // runs that turn up after it are announced as new.
    bool m_cloudAgentAnswered = false;
    int m_renderedCloudSession = 0;
    int m_renderedCloudTurns = -1;
    qsizetype m_renderedCloudTailLength = -1;
    forkmesh::agents::CloudAgentStreamWatch m_cloudAgentStream;
    QList<QJsonObject> m_cloudAgentStreamEvents;
    int m_renderedCloudStreamEvents = 0;
    QUrl cloudAgentStreamUrl(
        const forkmesh::agents::CloudAgentSession &cloud) const;
    int cloudAgentStreamShownEvents(int sessionId) const;
    void pumpCloudAgentStream(bool rang = false);
    void readCloudAgentStream();
    void applyCloudAgentStreamBatch(
        int sessionId, const forkmesh::agents::CloudAgentStreamBatch &batch);
    QUrl cloudAgentSessionsUrl(const QString &owner, const QString &name,
                               const QString &sessionId = QString()) const;
    QUrl cloudAgentSessionUrl(
        const forkmesh::agents::CloudAgentSession &cloud) const;
    QUrl cloudAgentAccountSessionsUrl() const;
    QUrl cloudAgentBranchWebUrl(int sessionId) const;
    QHash<QString, QString> m_cloudBranchSyncSig;
    QHash<QString, qint64> m_cloudBranchSyncAtMs;
    QSet<QString> m_cloudBranchSyncInFlight;
    bool m_cloudBranchSyncRetryQueued = false;
    void syncCloudAgentBranches();
    void syncCloudAgentBranchesIn(
        int repoIndex, const QString &relayOwner, const QStringList &branches,
        std::function<void(const QStringList &present, bool reached)> done = {});
    void openCloudAgentBranch(int sessionId);
    bool authenticateCloudAgentRequest(QUrl &url, QNetworkRequest &request,
                                       const QString &proof,
                                       const QString &prompt = QString()) const;
    void refreshCloudAgentSessions(bool force = false);
    void applyCloudAgentSessions(
        const QString &owner, const QString &name,
        const QList<forkmesh::agents::CloudAgentSession> &sessions);
    void applyCloudAgentAccountSessions(
        const QList<forkmesh::agents::CloudAgentSession> &sessions,
        qint64 finishedBeforeMs = 0);
    void applyCloudAgentSessionSet(
        const QList<forkmesh::agents::CloudAgentSession> &sessions,
        const QString &repoKey, qint64 finishedBeforeMs = 0);
    void announceCloudAgentArrivals(
        const QList<forkmesh::agents::CloudAgentSession> &arrivals);
    void flushCloudAgentArrivals();
    QList<int> movableCloudAgentSessionIds() const;
    bool cloudRunsMoveToDesktop() const;
    void moveAllCloudAgentsToDesktop();
    void moveQueuedCloudAgentsHere();
    void pumpCloudAgentMoves();
    void updateMoveToDesktopButton();
    QList<forkmesh::agents::CloudAgentSession> m_pendingCloudArrivals;
    bool m_cloudArrivalFlushScheduled = false;
    void injectCloudAgentSessions();
    void fetchCloudAgentDetail(int sessionId);
    QString cloudAgentImageDirectory() const;
    void fetchCloudAgentImages();
    void renderCloudAgentTranscript(int sessionId);
    void sendCloudAgentFollowUp(int sessionId, const QString &prompt);
    bool startQuickAddCloudAgent(int repoIndex, const QString &prompt,
                                 const QString &provider,
                                 const QString &model, bool createPr);
    void deleteCloudAgentSession(int sessionId);
    void requestCloudAgentSessionDelete(int sessionId);
    bool sendCloudAgentSessionDelete(
        const forkmesh::agents::CloudAgentSession &cloud,
        std::function<void(bool alreadyGone, const QString &error)> done);
    QSet<QString> m_cloudRetiredSessionIds;
    bool m_cloudRetiredLoaded = false;
    void loadRetiredCloudAgentSessions();
    int retireMergedCloudAgentSessions(const QString &owner, const QString &name,
                                       const QString &branch);
    // "Continue on this desktop": take a run the website queued and no node
    // has started, and run it here instead. The relay hands over the same
    // lease the periodic drain would have claimed.
    int localSessionForCloudSession(const QString &cloudSessionId) const;
    int cloudAgentLocalRepoIndex(
        const forkmesh::agents::CloudAgentSession &cloud) const;
    QString cloudAgentContinueHereBlocker(int sessionId) const;
    void continueCloudAgentHere(int sessionId);
    void continueFinishedCloudAgentHere(int sessionId);
    void continueCloudRunOnItsBranch(
        int sessionId, forkmesh::agents::CloudAgentSession cloud,
        const QString &stoppedOn);
    void stopCloudRunAndContinueHere(int sessionId);
    void postCloudRunStopThenContinue(int sessionId);
    void readCloudAgentSessionThen(
        const forkmesh::agents::CloudAgentSession &cloud,
        std::function<void(const forkmesh::agents::CloudAgentSession &)> done);
    QString cloudAgentContinueHereTip(int sessionId) const;
    QString cloudAgentRetryBlocker(int sessionId) const;
    void retryCloudAgentInCloud(int sessionId);
    void postCloudAgentBranchAction(
        int sessionId, const QJsonObject &body, int attempt,
        std::function<void(int httpStatus, const QJsonObject &payload)> done);
    QString cloudAgentBranchActionBlocker(int sessionId) const;
    void mergeCloudAgentBranch(int sessionId);
    void cleanUpCloudAgentBranch(int sessionId);
    void stopCloudAgentRun(int sessionId);
    void createCloudAgentPull(int sessionId);
    void linkCloudAgentPull(const QString &cloudKey, int number);
    void updateCloudAgentBranch(int sessionId);
    void openCloudAgentActions(int sessionId);
    void completeCloudAgentTask(const forkmesh::agents::CloudAgentSession &cloud,
                                const QString &note);
    void queueCloudAgentRetry(const forkmesh::agents::CloudAgentSession &cloud);
    void signInRunnerForCloudAgent(int sessionId);
    bool postCloudAgentQueue(const QString &owner, const QString &name,
                             const QString &runtime, const QString &model,
                             bool createPr, const QString &prompt,
                             std::function<void(bool, const QString &)> done);
    void finishCloudAgentClaim(
        int sessionId, const forkmesh::agents::CloudAgentSession &cloud,
        const QJsonObject &job);
    int quickAddCloudFollowUpSessionId() const;
    bool externalIsLive(const QString &uuid) const; // still in the detected set
    void renderExternalTranscript(int sessionId, bool full);
    void renderExternalTranscriptSlice(quint64 generation);
    void stopExternalSession(int sessionId); // signal the external CLI process to quit
    void killExternalSessionPids(const QList<qint64> &pids); // Graceful POSIX stop; native Windows termination.
    QSet<QString> m_externalStopped;         // uuids we've stopped — keep them idle
    QSet<QString> ownStreamCwds() const;    // dirs ForkMesh's own streams drive
    qint64 m_claudeModelsFetchedMs = 0;
    QJsonArray m_liveClaudeModels;
    // composerModelCatalog() memo — the choices and the inputs they were
    // built from (see the function); mutable because the builder is const.
    mutable QString m_composerModelCatalogInputs;
    mutable QList<ComposerModelChoice> m_composerModelCatalog;
    bool m_cloudflareAiModelsFetched = false;
    int resolveAgentRepositoryIndex(int preferred) const;
    int startAdHocAgentForRepo(int repoIndex, const QString &task,
                               const QString &provider, bool createPr,
                               const QString &model = QString(),
                               const QString &titleOverride = QString(),
                               bool genie = false, bool switchToTab = true,
                               const QString &orgTaskId = QString(),
                               const QString &modeOverride = QString(),
                               const QString &baseBranch = QString(),
                               int priority = 50,
                               bool directCheckout = false,
                               const QString &strengthOverride = QString(),
                               bool vibe = false, bool held = false);
    // Start an agent on a branch that already exists (a pull request's head)
    // rather than forking a new one off the base, so the work lands on the
    // branch it is about.
    int startAgentOnExistingBranch(int repoIndex, const QString &branch,
                                   const QString &baseBranch, int prNumber,
                                   const QString &title, const QString &task,
                                   const QString &provider,
                                   const QString &model, bool held = false);
    void requestCloudflareWorkspaceName(
        const QString &prompt, const QString &model,
        std::function<void(QString)> completion);
    void requestAgentTitle(const QString &provider, const QString &prompt,
                           std::function<void(QString)> completion);
    void applyAgentTitle(int sessionId, const QString &provider,
                         const QString &title);
    QString saveNewAgentPromptImage(const QImage &image);
    QPushButton *m_agentStopButton = nullptr;
    QPushButton *m_agentStartButton = nullptr;
    QComboBox *m_agentTaskGradeCombo = nullptr;
    QPushButton *m_agentStopAllButton = nullptr;
    QPushButton *m_agentStartAllButton = nullptr;
    QPushButton *m_agentUpdateAllButton = nullptr;
    ActivityRailButton *m_agentQueueButton = nullptr; // "3/10" + limit menu
    ActivityRailButton *m_agentUsageButton = nullptr;
    QString m_agentUsageProvider;
    QPushButton *m_agentDeleteAllButton = nullptr; // delete agent + worktree + branch
    QPushButton *m_agentBranchButton = nullptr;
    QPushButton *m_agentWorktreeButton = nullptr;
    QLabel *m_agentSyncLabel = nullptr;
    ActivityRailButton *m_agentSyncButton = nullptr;
    bool m_agentSyncPullPending = false;
    bool m_agentSyncPollQueued = false;
    QString m_agentSyncPullWorktree;
    QPushButton *m_agentDeleteMergedButton = nullptr;
    QWidget *m_agentRecycleBinBar = nullptr;
    QLabel *m_agentRecycleBinLabel = nullptr;
    QList<AgentSession> m_agentRecycled; // every repo's bin, newest first
    ActivityRailButton *m_agentDeleteSelectedButton = nullptr;
    QPushButton *m_agentTestApiKeyButton = nullptr;
    QLabel *m_agentOpenAiSpend = nullptr;
    QLabel *m_agentApiKeyStatus = nullptr;
    QLabel *m_agentClaudeSpend = nullptr;
    QLabel *m_agentClaudeStatus = nullptr;
    QLabel *m_agentOpenAiCredit = nullptr;
    QLabel *m_agentClaudeCredit = nullptr;
    QLabel *m_agentTotalSpend = nullptr;
    double m_openAiSpendUsd = std::numeric_limits<double>::quiet_NaN();
    double m_claudeSpendUsd = std::numeric_limits<double>::quiet_NaN();
    void updateAgentTotalSpend();
    QLabel *m_agentLimitsLabel = nullptr;
    QTimer *m_agentLimitsTimer = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QTimer *m_refreshSpinTimer = nullptr;
    int m_refreshAngle = 0;
    QPushButton *m_commitsBranchButton = nullptr;
    QPushButton *m_commitsFetchButton = nullptr;
    QPushButton *m_commitsPullButton = nullptr;
    // Infinite-scroll paging for the commit list: how many commits are currently
    // loaded, whether older history remains, and a re-entrancy guard.
    int m_commitsLimit = 300;
    bool m_commitsHasMore = false;
    bool m_commitsLoadingMore = false;
    bool m_commitsShowingAll = false;
    QTimer *m_nodeSwitchSpinTimer = nullptr;
    int m_nodeSwitchAngle = 0;
    QProgressBar *m_nodeSwitchProgress = nullptr;
    void positionNodeSwitchProgress();
    QTimer *m_repoSwitchSpinTimer = nullptr;
    int m_repoSwitchAngle = 0;
    QTimer *m_issueSpinTimer = nullptr;
    int m_issueSpinFrame = 0;
    qreal m_issueSpinAngle = 0.0; // fireball head on the issue rows' assignee badge
    void tickIssueListSpinners();
    bool m_nodeSwitching = false;      // a node switch's heavy load is running
    bool m_repoDetailLoading = false;  // re-entrancy guard for openRepoDetail
    bool m_branchesPanelLoading = false;
    bool m_branchesPanelReloadQueued = false;
    int m_branchesPanelGen = 0;
    QString m_branchesPanelDir;
    QString m_branchesPanelPendingSelect;
    QString m_branchMergedFlashBranch;
    QString m_branchMergedFlashDir;
    int m_branchMergedFlashRow = -1;
    QString m_branchMergedFlashHtml;
    static constexpr int kBranchMergedFlashMs = 20000;
    int m_mergeCelebrationGen = 0;
    bool m_worktreeMergeInProgress = false;
    QSet<QString> m_branchCleanupInProgress;
    // The day's merge stats drawn under an empty range or clean working tree.
    // Re-rendering that pane (a font step, a theme swap, a second read of the
    // same state) must not re-run the day's git reads, so the page is kept for
    // a few seconds.
    // Rendered "no changes" panes by input key (branch/base/range/message).
    // The SCM pane (branch empty) and the branch pane (branch set) alternate
    // through here; a single slot evicted on every pane switch, so the two
    // git log walks behind it ran on each switch.
    struct BranchNoChangesEntry {
        QString html;
        qint64 atMs = 0;
    };
    QHash<QString, BranchNoChangesEntry> m_branchNoChangesPanes;
    // Keys whose merge-history read is running on a worker, and the repaint the
    // newest caller for each wants once it lands.
    QSet<QString> m_branchNoChangesLoading;
    QHash<QString, std::function<void(QString)>> m_branchNoChangesReady;
    static constexpr int kBranchNoChangesStatsMs = 15000;
    // How far back those stats look, as an index into mergeRangeSpecs(): the
    // last 24 hours until a window button on the page says otherwise (adhoc
    // #32). The page each button repaints is remembered with it, because the
    // celebration and the empty-range pane are built by different calls.
    int m_mergeRange = 0;
    struct MergeRangePane {
        bool celebration = false;
        QString branch;
        QString base;
        QString dir;
        QString message;
    };
    MergeRangePane m_mergeRangePane;
    // Re-entrancy guard for loadMirrorNodesPanel: its synchronous git reads pump
    // the event loop, so a queued roster/mirror callback could start a second
    // pass that appends its own rows on top of the half-built table — every node
    // listed twice.
    bool m_mirrorNodesPanelLoading = false;
    bool m_agentMergeStateRefreshing = false; // refreshAgentMergeState worker in flight
    // Shared re-entrancy guard for the two heavy periodic refreshes
    // (refreshOpenRepoDetail + refreshRepositoryList): each runs synchronous git
    // reads under a GitKeepAlive that pumps the event loop, so a second one firing
    // during that pump would nest its git work and compound into a GUI stall
    // The second one defers instead.
    bool m_heavyRefreshInFlight = false;
    bool m_repoListRefreshQueued = false; // a refreshRepositoryList deferred past a pump
    // Signature of the inputs to outbound per-repo mirror adverts
    // (mirrorPath|lastSyncMs|localPath|worktrees-dir mtime, per non-preview repo).
    // Skip the expensive rebuild while unchanged. Reset by attachBackend so a
    // freshly attached relay client receives current local state.
    QString m_mirrorAdvertSig;
    QString m_mirrorAdvertInputSig;
    bool m_mirrorAdvertRefreshInFlight = false;
    qint64 m_mirrorAdvertCompletedAtMs = 0;
    void refreshMirrorAdverts();
    int m_repoOpenPending = -1;        // repo index queued by openRepoDetailDeferred
    bool m_repoLoadActive = false;
    bool m_loadStatusShowing = false;
    QElapsedTimer m_loadStepTimer;
    QString m_loadStepName;
    QLabel *m_issueTitle = nullptr;
    // "Sub-issue of" pills under the status pill, from relationship lines;
    // a FlowLayout whose first item is the caption.
    QWidget *m_issueParentRef = nullptr;
    QLineEdit *m_issueTitleEditor = nullptr;
    QPushButton *m_issueTitleEditButton = nullptr;
    QPushButton *m_issueTitleSaveButton = nullptr;
    QPushButton *m_issueTitleCancelButton = nullptr;
    QLabel *m_issueMeta = nullptr;
    QLabel *m_issueComposerAvatar = nullptr;
    QLabel *m_issueComposerTitle = nullptr;
    QLabel *m_issueReadonlyNote = nullptr;
    QLabel *m_issueInlineNotice = nullptr;
    QLabel *m_issueAssigneesValue = nullptr;
    QLabel *m_issueReporterValue = nullptr;
    QLabel *m_issueLabelsValue = nullptr;
    QWidget *m_issueLabelsView = nullptr;
    QLayout *m_issueLabelsPillsLayout = nullptr; // FlowLayout: pills wrap
    QLabel *m_issueMilestoneValue = nullptr;
    QLabel *m_issuePriorityValue = nullptr;
    QWidget *m_issueProgressSlider = nullptr;
    QLabel *m_issueEstimateValue = nullptr; // derived OpenAI coding cost estimate
    QLabel *m_issueBountyValue = nullptr;
    QLabel *m_issueCommentsValue = nullptr;
    QStackedWidget *m_issueAssigneesStack = nullptr;
    QStackedWidget *m_issueReporterStack = nullptr;
    QStackedWidget *m_issueLabelsStack = nullptr;
    QStackedWidget *m_issueMilestoneStack = nullptr;
    QStackedWidget *m_issuePriorityStack = nullptr;
    QLineEdit *m_issueAssigneesEdit = nullptr;
    QStringListModel *m_issueAssigneesSuggestionModel = nullptr; // its completer's names
    QPushButton *m_issueAssigneesSelfButton = nullptr; // "Assign yourself"
    QPushButton *m_issueAssigneesSaveButton = nullptr;
    QStringList m_issueAssigneesShown; // last rendered list, re-read on a repo change
    QString m_issueAssigneesRenderKey;  // list + writability that value label shows
    QLineEdit *m_issueReporterEdit = nullptr;
    QPushButton *m_issueReporterSelfButton = nullptr; // "Credit yourself"
    QPushButton *m_issueReporterSaveButton = nullptr;
    QString m_issueReporterShown;      // last rendered name, re-read on a repo change
    QString m_issueReporterRenderKey;  // name + writability that value label shows
    QLineEdit *m_issueLabelsEdit = nullptr;
    QComboBox *m_issueMilestoneEdit = nullptr;
    QSpinBox *m_issuePriorityEdit = nullptr;
    QLabel *m_issueDatesValue = nullptr;
    QStackedWidget *m_issueDatesStack = nullptr;
    QPushButton *m_issueDatesButton = nullptr;
    QDateEdit *m_issueStartDateEdit = nullptr;
    QDateEdit *m_issueEndDateEdit = nullptr;
    QCheckBox *m_issueStartDateEnable = nullptr;
    QCheckBox *m_issueEndDateEnable = nullptr;
    QScrollArea *m_issueThreadScroll = nullptr;
    QWidget *m_issueThreadContainer = nullptr;
    QVBoxLayout *m_issueThreadLayout = nullptr;
    QWidget *m_issueComposerRow = nullptr;
    QWidget *m_issueAiTypingRow = nullptr; // transient "AI is answering…" card
    QTimer *m_issueAiTypingTimer = nullptr;
    MarkdownEditor *m_issueComposer = nullptr;
    QPushButton *m_issueNewButton = nullptr;
    QPushButton *m_issueListNewButton = nullptr;
    QPushButton *m_issueSyncButton = nullptr;
    QPushButton *m_issuePrioritizeButton = nullptr;
    QComboBox *m_issuePrioritizeAgentCombo = nullptr;
    bool m_prioritizeInFlight = false;
    QPushButton *m_issueCompletenessButton = nullptr;
    bool m_completenessInFlight = false;
    bool m_looperActive = false;
    int m_looperSessionId = 0;
    QString m_looperProvider;
    int m_looperCurrentIssue = 0;
    QString m_looperCurrentTitle;
    QWidget *m_looperToggle = nullptr;
    QString m_looperRepoSlug;
    QPushButton *m_issueCopyButton = nullptr;
    QPushButton *m_issueCopyAllButton = nullptr;
    QPushButton *m_issueMcpPromptButton = nullptr;
    QPushButton *m_issueVoteButton = nullptr;
    VerticalIconButton *m_issueCreditsButton = nullptr;
    QPushButton *m_issueCommentButton = nullptr;
    QPushButton *m_issueAskAiButton = nullptr;
    QPushButton *m_issueAttachButton = nullptr;
    QPushButton *m_issueCloseButton = nullptr;
    QPushButton *m_issueCloseCommentButton = nullptr;
    QPushButton *m_issueLabelsButton = nullptr;
    QPushButton *m_issueMilestoneButton = nullptr;
    QPushButton *m_issuePriorityButton = nullptr;
    QPushButton *m_issuePriorityRaiseButton = nullptr;
    QPushButton *m_issuePriorityLowerButton = nullptr;
    QPushButton *m_issueProgressButton = nullptr;
    QPushButton *m_issueProgressBoostButton = nullptr;
    QPushButton *m_issueBountyButton = nullptr;
    QPushButton *m_issueAssigneesButton = nullptr;
    QPushButton *m_issueReporterButton = nullptr;
    QLabel *m_issueDevelopmentValue = nullptr;    // linked pull requests list
    QPushButton *m_issueLinkPullButton = nullptr; // "Link pull request"
    QTabWidget *m_issueDetailTabs = nullptr;
    int m_issueFilesTabIndex = -1;                // tab index of "Files changed"
    QListWidget *m_issueFilesList = nullptr;      // changed files in the linked diff
    QTextBrowser *m_issueDiffView = nullptr;      // diff viewer in the files tab
    forkmesh::ui::DiffFileNavigator *m_issueDiffNav = nullptr;  // sticky header + scroll<->select
    QLabel *m_issueFilesChangedSummary = nullptr; // "N files changed" line
    QPushButton *m_issueDeleteButton = nullptr;
    QPushButton *m_issueMoveToTaskButton = nullptr;
    QLabel *m_issueAgentValue = nullptr;
    QCheckBox *m_issueAgentCreatePrCheck = nullptr;
    QComboBox *m_issueAgentProvider = nullptr;   // Codex | OpenAI API | Claude API | Claude Code
    QComboBox *m_issueAgentModel = nullptr;      // model for the picked provider
    QComboBox *m_issueAgentSelector = nullptr;   // the two above as one visible row
    QPushButton *m_issueAssignAgentButton = nullptr;
    QPushButton *m_issueAgentViewButton = nullptr;
    QList<Issue> m_currentIssues;
    QList<IssueLabel> m_currentLabels;
    QList<IssueMilestone> m_currentMilestones;
    int m_currentIssueNumber = -1;
    QString m_currentIssueTitle;
    bool m_issueDeleteConfirmPending = false;
    bool m_issueHistoryDeleteInProgress = false;
    bool m_issueTaskMoveInFlight = false;
    bool m_keepCurrentOnReload = false;
    int m_selectIssueOnReload = -1;
    // Content signature (.forkmesh/issues/ subtree oid + repo path) of the data the issue
    // list was last built from. reloadIssues() fires on every push/sync — i.e.
    // every agent commit — but a code-only commit doesn't touch issue metadata, so this
    // lets it skip re-reading git and tearing down/rebuilding the issue rows when
    // nothing changed (a rebuild mid-interaction drops the click/keystroke the user
    // aimed at a row or the search box). Empty = "unknown", never skip.
    QString m_issuesLoadedSig;
    int m_issueLoadGen = 0;
    bool m_issueBackgroundLoadInFlight = false;
    bool m_issueBackgroundReloadQueued = false;
    QStringList m_pendingIssueAttachments; // images queued for the next comment

    QTableWidget *m_projectTable = nullptr;
    QStackedWidget *m_projectViewStack = nullptr; // 0 list table, 1 Gantt
    QButtonGroup *m_projectViewTabs = nullptr;    // its List / Gantt toggle
    QComboBox *m_projectStatusFilter = nullptr;   // Open | Closed | All
    QWidget *m_projectGantt = nullptr;
    QWidget *m_projectDetailPane = nullptr; // right-hand detail beside the list
    QLabel *m_projectDetailTitle = nullptr;
    QLabel *m_projectDetailStatus = nullptr; // Open/Closed status pill
    QLabel *m_projectDetailBody = nullptr;
    QLabel *m_projectDetailProgress = nullptr;
    QLabel *m_projectInlineNotice = nullptr;
    QDateEdit *m_projectStartEdit = nullptr;
    QDateEdit *m_projectEndEdit = nullptr;
    QCheckBox *m_projectStartEnable = nullptr;
    QCheckBox *m_projectEndEnable = nullptr;
    QComboBox *m_projectMilestoneCombo = nullptr;
    QListWidget *m_projectIssuesList = nullptr; // linked issues in the detail pane
    QPushButton *m_projectNewButton = nullptr;
    QPushButton *m_projectCloseButton = nullptr; // Close/Reopen the project
    QPushButton *m_projectDeleteButton = nullptr;
    QList<Project> m_currentProjects;
    int m_currentProjectNumber = -1;
    bool m_projectDeleteConfirmPending = false;

    QWidget *m_nodeProfilePanel = nullptr;
    QWidget *m_nodeProfileSectionHost = nullptr; // full-page home (section 10)
    QPushButton *m_profileCloseButton = nullptr; // hidden while shown as a tab
    QWidget *m_repoDetailSection = nullptr; // hidden while node profile is full-page
    QLabel *m_profileAvatar = nullptr;
    QPixmap m_profileAvatarSource; // raw avatar, re-scaled to a banner on resize
    QLabel *m_profileName = nullptr;
    QLabel *m_profileStatus = nullptr;
    QLabel *m_profileDetails = nullptr;
    QLabel *m_profileMirrorsLabel = nullptr;
    QLabel *m_profileMirrors = nullptr;
    QWidget *m_profileAccountSection = nullptr;
    QLabel *m_profileAccountLabel = nullptr;  // "NODES (n)" / "USER ACCOUNT" header
    QLabel *m_profileAccountStatus = nullptr; // link-state text; hidden once linked
    QListWidget *m_profileUserNodesList = nullptr;
    QPushButton *m_profileLinkUserButton = nullptr;
    // "Link this node to your account" next to the node ID: browser-based
    // linking via a node-signed grant URL.
    QPushButton *m_profileLinkBrowserButton = nullptr;
    int m_linkGrantPollsLeft = 0; // post-browser-link polling countdown
    QString m_linkGrantBaselineOwner;
    QString m_nodeOwnerUser;
    QStringList m_profileLinkedNodes;
    bool m_profileIsUserAccount = false; // this account has login creds (a user)
    QLabel *m_profileNote = nullptr;
    QWidget *m_profileStatGrid = nullptr;
    QLabel *m_profileTileRepos = nullptr;
    QLabel *m_profileTileMirrored = nullptr;
    QLabel *m_profileTileOnline = nullptr;
    QLabel *m_profileTileChats = nullptr;
    QLabel *m_profileHostingLabel = nullptr;
    QLabel *m_profileHosting = nullptr;
    bool m_profileIsSelf = false;
    QLabel *m_profileNodeKey = nullptr;
    QLabel *m_profileSolanaAddr = nullptr;
    QLabel *m_profileQr = nullptr;
    QWidget *m_profileSolanaSection = nullptr;
    QLabel *m_profileBalance = nullptr;
    QPushButton *m_profileBalanceButton = nullptr;
    QPushButton *m_profileVerifyButton = nullptr;
    QLabel *m_profileEligibility = nullptr;
    QPushButton *m_profileMessageButton = nullptr;
    // "Send tip" on another member's profile, plus the linked-wallet caption
    // and link/unlink controls shown on your own.
    QPushButton *m_profileTipButton = nullptr;
    QWidget *m_profileTipWalletSection = nullptr;
    QLabel *m_profileTipWalletLabel = nullptr;
    QPushButton *m_profileTipLinkButton = nullptr;
    QPushButton *m_profileTipExportButton = nullptr;
    QPushButton *m_profileTipUnlinkButton = nullptr;
    // One transfer at a time: the flow spans network round trips and a single
    // saved reconciliation receipt, and a second run would race the first for
    // the vault. A payment approved meanwhile waits in m_tipTransferQueue.
    bool m_tipInFlight = false;
    QString m_tipInFlightRewardId; // meaningful only while m_tipInFlight
    // Rewards the relay confirmed paid from here this session. A payment
    // approved while one of them was still in flight must not sign it again
    // once it lands, before the page has refreshed to say so.
    QSet<QString> m_tipPaidRewardIds;
    QList<TipTransferRequest> m_tipTransferQueue;
    // A manually submitted vault passphrase lasts only until the next local
    // signature. It is never persisted or transmitted, and is wiped when the
    // transfer completes or fails.
    QString m_tipOneShotPassphrase;
    // Public work context for one payment started from the Ledger. A second
    // tip cannot start while one is in flight, so these values remain bound to
    // its retries until the next entry point explicitly replaces them.
    QString m_tipTaskDescription;
    QUrl m_tipOutputUrl;
    QString m_tipEnteredCurrency = QStringLiteral("SOL");
    qint64 m_tipEnteredUsdMicros = 0;
    qint64 m_tipSolUsdPriceMicros = 0;
    qint64 m_tipPriceObservedAt = 0;
    // Admin-only "Take ownership" request on another node's profile.
    QPushButton *m_profileTakeOwnershipButton = nullptr;
    QPushButton *m_profileGetPaidButton = nullptr;
    QWidget *m_profileSelfActions = nullptr;
    // Side by side on the Nodes page; stacked on the narrower Settings page.
    QBoxLayout *m_profileColumnsRow = nullptr;
    QPushButton *m_profileSettingsButton = nullptr; // hidden inside Settings
    QGridLayout *m_profileActionGrid = nullptr;
    QList<QPushButton *> m_profileActionButtons; // "THIS MACHINE", in order
    QPushButton *m_profileRebuildButton = nullptr;
    QPushButton *m_profileUpdateButton = nullptr;
    QString m_profileNodeId;
    QString m_profileNodeName;
    QString m_profileSolanaValue;

    QStringList m_channels;
    QStringList m_officeConversations;
    QStringList m_officeDirectConversations;
    QString m_pendingOfficeConversation;
    QHash<QString, QString> m_pendingDirectInvites;
    QPointer<QLabel> m_projectInviteStatus;
    QStringList m_discordConversations;
    ChannelGroups::State m_channelGroups;        // persisted sidebar groups
    QList<ChannelGroups::Row> m_chatSidebarRows; // the layout last rendered
    QSet<QString> m_privateChannels;
    bool m_restoringPrivateChannels = false;
    QList<RepositoryRecord> m_repositories;
    mutable bool m_repositorySyncTimesQueued = false;
    // Conditional-request state for the periodic pin re-attest read: the
    // relay's weak catalog ETag plus the pins seen in the last full response,
    // so a 304 still detects local ref drift without a full catalog body.
    // Private route setup is permitted only after this client idempotently
    // registers its public hybrid key and repository privacy policy with an
    // authenticated owner session. Catalog publication remains service-owned.
    QSet<QString> m_privateControlReady;
    QSet<QString> m_privateControlInFlight;
    NodeEventSocket *m_nodeEventSocket = nullptr; // relay push -> repo/inbox sync
    // The streamed provider sign-in this node is currently running for the
    // account owner's browser, if any. The replay buffer is what a reconnected
    // tab is repainted with; it is bounded, never written to disk, and dropped
    // with the session.
    AgentLoginPty *m_nodeTerminalPty = nullptr;
    QString m_nodeTerminalSession;
    QByteArray m_nodeTerminalReplay;
    QTimer *m_nodeTerminalLifetime = nullptr;
    QHash<QString, std::shared_ptr<PrivateMirrorMaterialization>>
        m_privateMirrorMaterializations; // opaque replica id -> temp repo
    QHash<QString, std::shared_ptr<PublicMirrorMaterialization>>
        m_publicMirrorMaterializations; // legacy public-age migration only
    QList<MemberInfo> m_homeRoster;
    QHash<QString, MemberInfo> m_chatDirectoryUsers; // lowercased user -> profile
    QHash<QString, QByteArray> m_chatDirectoryAvatarDigests;
    bool m_chatDirectoryFetchInFlight = false;
    qint64 m_chatDirectoryFetchedMs = 0;
    bool m_chatDirectoryLoaded = false; // first fill done (may legitimately be empty)
    QSet<QString> m_removedPeerIds;  // IDs explicitly removed via removeChatMember
    QHash<QString, qint64> m_peerLastSeenMs;
    bool m_welcomeAnnounced = false;
    QString m_catalogMirrorsSource;        // "owner/name" the cache holds
    QJsonArray m_catalogMirrorsCache;      // last /mirrors payload's "mirrors"
    QString m_catalogMirrorsFetchSource;   // source the last fetch was kicked for
    qint64 m_catalogMirrorsFetchedMs = 0;  // throttle: last fetch kick time
    // Authenticated SSH fallback for managed Hosts.  Direct HTTPS proves the
    // repository and reachability; this cache supplies the richer local facts
    // that /health deliberately does not expose.
    QHash<QString, QJsonObject> m_sshMirrorMetadataCache; // source|node -> row
    QSet<QString> m_sshMirrorMetadataInFlight;
    QHash<QString, QJsonObject> m_mirrorReachabilityCache;
    QSet<QString> m_directMirrorDiscoveryInFlight;
    QHash<QString, QJsonObject> m_mirrorPendingCache;
    QString m_releaseDownloadsSource;      // "owner/name" the cache holds
    QHash<QString, int> m_releaseDownloadsCache; // sha256 -> download count
    QString m_releaseDownloadsFetchSource; // source the last fetch was kicked for
    qint64 m_releaseDownloadsFetchedMs = 0; // throttle: last fetch kick time
    QHash<int, bool> m_syncingRepos;
    QSet<int> m_pushingRepos;
    // Keyed "owner/name\x1f<ssh url>": the fan-out serializes per gateway, so a
    // node that is timing out cannot hold the fleet's healthy mirrors behind it.
    QSet<QString> m_sshMirrorPushing;
    QSet<QString> m_sshMirrorPushPending;
    // Retained release CAS reconciliation is deliberately independent from
    // immediate ref fan-out, but repeated commits must not start overlapping
    // full-store inventories on the same managed fleet.
    QSet<QString> m_managedArtifactReconciling;
    // Commit, mirror-refresh and safety-pass events all converge here. Keep
    // only one direct ref writer per managed host and replay the newest state
    // once after a burst, instead of racing Git's remote ref locks.
    MirrorRefPushGate m_managedRefPushGate;
    // repoKey -> single-shot wake-up for gateways skipped while cooling down.
    QHash<QString, QTimer *> m_sshMirrorPushRetry;
    MirrorGatewayHealth m_sshMirrorHealth;
    bool m_sshMirrorHealthLoaded = false;
    QSet<QString> m_mentionScanInFlight;
    QString m_currentConversation;
    QHash<QString, QList<ChatMessage>> m_history;
    QSet<QString> m_historyIds; // message ids already in m_history (dedup)
    QTimer *m_chatSaveTimer = nullptr;
    QTimer *m_chatExpiryTimer = nullptr;
    QPointer<QComboBox> m_chatRetentionCombo;
    QTimer *m_agentRecycleSweepTimer = nullptr; // periodic pruneExpiredChatHistory()
    QHash<QString, MessageRow *> m_visibleRows; // messageId -> row (current conv)
    QString m_activeChatThreadRootId;
    QSplitter *m_chatSplit = nullptr;
    QWidget *m_chatThreadPane = nullptr;
    QLabel *m_chatThreadTitle = nullptr;
    QVBoxLayout *m_chatThreadRowsLayout = nullptr;
    QPlainTextEdit *m_chatThreadInput = nullptr;
    QLabel *m_chatThreadCountLabel = nullptr;
    QHash<QString, QMap<QString, QStringList>> m_reactions;
    QHash<QString, QList<QJsonObject>> m_pendingOfficeMutations;
    QStringList m_pendingOfficeMutationOrder;
    QHash<QString, QPixmap> m_avatars;          // senderId -> avatar
    QHash<QString, QString> m_dmNames;          // peerId -> display name
    QHash<QString, QHash<QString, QString>> m_typing; // conversation -> peerId -> name
    QStringList m_networkLog;
    QHash<QString, int> m_logFilterCounts;
    int m_networkLogDiskLines = 0;              // lines in the live on-disk file
    qint64 m_networkLogClearedAtMs = 0;
    // Bucketed line counts for the whole 30-day retention window, including the
    // sessions and the rotated segments the in-memory buffer no longer covers.
    forkmesh::netlog::Digest m_networkLogDigest;
    qint64 m_networkLogDigestBucket = -1;       // bucket the last line landed in
    bool m_networkLogDigestDirty = false;
    bool m_deferRichLogRendering = false;       // watchdog reports stay off QTextDocument
    QStringList m_openDms;                      // peerIds in sidebar order
    QSet<QString> m_unread;
    QSet<QString> m_knownAdminPubkeys;
    QHash<QString, int> m_unreadCounts;
    QHash<QString, qint64> m_readMarks;
    QHash<QString, qint64> m_pendingReadMarks;
    QTimer *m_readMarkTimer = nullptr;
    QString m_userName;
    QByteArray m_userAvatar;
    QString m_lastChatDisplayName;
    QByteArray m_lastChatAvatar;
    QString m_typingConversation;
    QTimer *m_typingStopTimer = nullptr;
    QTimer *m_homeStatsTimer = nullptr;
    qint64 m_connectedAtMs = 0;
    qint64 m_totalConnectionMs = 0;
    qint64 m_uptimePersistedAtMs = 0;
    qint64 m_nodeAlertGraceUntilMs = 0;

    bool m_accountAuthenticated = false;
    QString m_accountName;
    QString m_accountEmail;
    // Session token minted by /api/accounts/login, used to authenticate
    // profile writes (e.g. persisting the chosen avatar to the account record
    // so the web dashboard shows the same picture the desktop app does).
    QString m_accountSessionToken;
    QString m_accountSessionInstanceUrl;
    forkmesh::accounts::SessionCache m_accountSessionCache;
    bool m_accountSolanaVerified = false;
    bool m_accountDesktopCapable = false;
    QString m_accountTier = QStringLiteral("free");
    QTimer *m_heartbeatTimer = nullptr;
    // Exactly one heartbeat delivery is active. If a socket reply is lost,
    // its ack timeout reuses this same signed body over HTTPS; optional credit
    // and mirror-request fields remain queued until a 2xx verdict arrives.
    bool m_nodeHeartbeatDeliveryActive = false;
    bool m_nodeHeartbeatViaSocket = false;
    bool m_nodeHeartbeatSendPending = false;
    bool m_nodeHeartbeatSocketFailed = false;
    qint64 m_lastHeartbeatOkMs = 0;
    // True once this session's node socket has dropped. A reconnect
    // then means "pushes may have been missed", while the very first
    // connect is already covered by the launch pulls.
    bool m_nodeEventSocketDropped = false;
    QString m_nodeHeartbeatRequestId;
    QString m_nodeHeartbeatCreditsRefilled;
    QStringList m_nodeHeartbeatMirrorAcks;
    QJsonObject m_nodeHeartbeatBody;
    QTimer *m_nodeHeartbeatAckTimer = nullptr;
    bool m_isAdmin = false;
    // Pending email confirmations are fetched once per signed-in admin, not
    // on a timer; reset on logout/rename so the next admin session checks.
    bool m_adminPendingPolled = false;
    QStringList m_seenPendingUsers;
    QString m_roomPassphrase;
    QString m_lastClaimCodeShown;
    // Admin claiming this node last shown for an ownership-transfer prompt
    // so the per-minute heartbeat doesn't reopen the dialog
    // while the request is still pending a decision.
    QString m_lastOwnershipTransferAdminShown;
    QSet<QString> m_handledMirrorRequests;
    QStringList m_pendingMirrorRequestAcks;
    QPushButton *m_userAvatarNavButton = nullptr;
#ifdef FORKMESH_WINDOW_TESTS
    bool m_testUseAccountFlowResult = false;
    bool m_testAccountFlowResult = true;
    bool m_testAccountFlowDesktopCapable = true;
    int m_testEnsureNodeAccountCalls = 0;
    bool m_testBypassServerStart = false;
    TestIssueHistoryDeleteRunner m_testIssueHistoryDeleteRunner;
    std::function<void()> m_testBeforeOffThreadDrainHook;
    std::atomic_bool *m_testTeardownBodyFinished = nullptr;
    std::function<int(const QString &, const QString &)>
        m_testTeammateStartHook;
    std::function<void(int, const QString &)> m_testTeammateContinueHook;
#endif
};

#endif // FORKMESH_MAIN_WINDOW_H
