#include "InstanceTheme.h"
#include "ForkMeshVersion.h"
#include "ActionStore.h"
#include "HiddenTerms.h"
#include "ProcessReaper.h"
#include "AgentCapabilityBroker.h"
#include "BackgroundFileWriter.h"
#include "AgentCapabilityStore.h"
#include "AgentPermissionController.h"
#include "Tooltips.h"
#include "DeployLogs.h"
#include "AgentPromptImages.h"
#include "MainWindow.h"
#include "AgentActivityFeed.h"
#include "GenieSpiderWidget.h"
#include "BackupHostFeed.h"
#include "CleanupHostFeed.h"
#include "PetHostFeed.h"
#include "CrashHandler.h"
#include "CaptureFeature.h"
#include "ForkMeshPackageRegistry.h"
#include "LedgerFeature.h"
#include "LocalMeshNode.h"
#include "DiscordClient.h"
#include "MainWindowInternal.h"
#include "MainWindowNextAccess.h"
#include "SettingsSectionsView.h"
#include "SettingsWrite.h"
#include "SingleInstance.h"
#include "BackgroundMode.h"
#include "StatsDashboard.h"
#include "NetworkReplyError.h"
#include "NodeEventSocket.h"
#include "OfficeChannelMirror.h"
#include "QtRuntimeDiagnostics.h"
#include "RewardPoolSigner.h"
#include "StallWatchdog.h"
#include "SystemPressure.h"
#include "StartupSplash.h"
#include "UserAvatarStore.h"
#include "WindowFade.h"
#include "WindowFrameControl.h"
#include "WorldSpeechBridge.h"

#include <QNetworkProxyFactory>

using namespace forkmesh::ui;

namespace {

// "10s" / "5min" / "750ms": how a timer's interval reads in a routine name.
QString timerCadenceText(int intervalMs)
{
    if (intervalMs <= 0)
        return QStringLiteral("unknown interval");
    if (intervalMs % 60000 == 0)
        return QStringLiteral("%1min").arg(intervalMs / 60000);
    if (intervalMs % 1000 == 0)
        return QStringLiteral("%1s").arg(intervalMs / 1000);
    return QStringLiteral("%1ms").arg(intervalMs);
}

QString networkRequestEventFor(const QUrl &url)
{
    const QString path = url.path();
    struct Route {
        const char *needle;
        const char *event;
    };
    static const Route routes[] = {
        {"/api/sync", "relay sync"},
        {"/api/repositories", "catalog fetch"},
        {"/agents", "agent poll"},
        {"/issues", "issue fetch"},
        {"/pulls", "pull fetch"},
        {"/releases", "release fetch"},
        {"/mirrors", "mirror sync"},
        {"/api/repo/", "repo fetch"},
        {"/api/version", "version check"},
        {"/api/metrics", "traffic metrics"},
        {"/api/network", "network stats"},
        {"/api/security", "security report"},
        {"/api/forkbot", "forkbot chat"},
        {"/api/chat", "chat"},
        {"/api/notifications", "ping inbox"},
        {"/api/accounts", "account"},
        {"/api/oauth", "account"},
    };
    for (const Route &r : routes) {
        if (path.contains(QLatin1String(r.needle)))
            return QString::fromLatin1(r.event);
    }
    return QStringLiteral("request");
}

void stopSupervisingUserServiceAfterWindowClose()
{
    const QString unit = supervisingSystemdUserServiceUnit();
    const bool restart = supervisingUserServiceRestartRequested();
    const QStringList arguments = systemdUserServiceCloseArguments(unit, restart);
    if (arguments.isEmpty())
        return;
    if (!forkmesh::background::sessionEnding() &&
        closeHoldsSourceGuard(
            unit, restart,
            QSettings().value(kReopenAfterCloseSetting, false).toBool()) &&
        !writeDesktopCloseHold(sourceGuardHoldPath(),
                               QDateTime::currentSecsSinceEpoch())) {
        qWarning().noquote()
            << "ForkMesh closed, but could not hold the source guard at"
            << sourceGuardHoldPath();
    }
    if (!forkmesh::processreaper::startReleased(QStringLiteral("systemctl"), arguments)) {
        qWarning().noquote()
            << "ForkMesh closed, but could not hand its user service back to systemd"
            << unit;
    }
}

} // namespace

MainWindow::~MainWindow()
{
    forkmesh::teammates::setRecentAgentChoiceSource({});
    // The collector is a QObject child, but its update callback reaches C++
    // members that are destroyed before QObject deletes children. Tear it down
    // explicitly while those members and the application event filter are
    // still valid.
    if (m_qtDiagnostics) {
        m_qtDiagnostics->onSnapshot = {};
        delete m_qtDiagnostics;
        m_qtDiagnostics = nullptr;
    }
    delete m_railEventHook;
    m_railEventHook = nullptr;
    delete m_genieStage.data();
    // Once destruction starts the event loop cannot heartbeat again. Leaving
    // the watchdog armed makes normal teardown look like a stall and lets its
    // SIGUSR2 backtrace interrupt Qt/glibc while they free the widget backing
    // store. Stop and join it while this window is still fully alive.
    if (m_stallWatchdog)
        m_stallWatchdog->stop();

    forkmesh::agentfeed::clearHost(this);
    forkmesh::backupfeed::clearHost(this);
    forkmesh::cleanupfeed::clearHost(this);
    forkmesh::petfeed::clearHost(this);

    // The local mesh node is a QObject child. Its stop() announces the
    // membership change (peersChanged -> refreshChatMembers ->
    // refreshMentionCandidates), which reads MainWindow's rosters and account
    // strings. QObject deletes children only after this destructor has
    // destroyed those members, so stop it here, silenced, while everything the
    // announcement reaches is still alive.
    if (m_localMesh) {
        QObject::disconnect(m_localMesh, nullptr, this, nullptr);
        m_localMesh->stop();
    }

    // A debounced ledger save may still be pending; flush it now while the
    // ledger and its callback are both alive, then detach the callback so
    // nothing notifies into a half-destroyed window.
    if (m_agentToolLedger) {
        m_agentToolLedger->changed = {};
        if (m_agentToolLedger->hasSamples() ||
            QFile::exists(m_agentToolLedger->persistPath()))
            m_agentToolLedger->saveLater();
    }
    forkmesh::diskwrite::flush();
    ActionStore::flushWrites();
    flushRepositorySyncTimes();
    flushDeferredSettings();

    clearDesktopPackageExtensions();

    m_acceptOffThreadResults.store(false, std::memory_order_release);
    forkmesh::BackgroundActivity::setListener(nullptr);

    // These top-level child overlays can still own queued paint work and active
    // timers.  Do not leave them alive through the potentially long worker and
    // process drain below and then ask QObject's base destructor to tear their
    // QWidget backing state down after MainWindow's members are gone.
    dockPoppedOutPages();
    destroyTransientOverlays();

#ifdef FORKMESH_WINDOW_TESTS
    if (m_testBeforeOffThreadDrainHook)
        m_testBeforeOffThreadDrainHook();
#endif
    drainOffThreadWorkers();

    m_captureFeature.reset();
    m_ledgerFeature.reset();
    clearAccountSessionMemory();

    const QList<QProcess *> processChildren = findChildren<QProcess *>();
    for (QProcess *process : processChildren)
        process->disconnect();

    if (m_worldSpeechBridge)
        m_worldSpeechBridge->stop();

    QList<QPointer<QProcess>> processes;
    const QList<QProcess *> childProcesses = findChildren<QProcess *>();
    processes.reserve(childProcesses.size());
    for (QProcess *process : childProcesses)
        processes.append(process);
    for (const QPointer<QProcess> &guardedProcess : std::as_const(processes)) {
        QProcess *process = guardedProcess.data();
        if (!process)
            continue;
        QObject::disconnect(process, nullptr, nullptr, nullptr);
        if (process->state() != QProcess::NotRunning) {
            // code-health: allow(blocking-wait) destructor shutdown; each still-running child gets at most 2 s after SIGTERM, then 1 s after SIGKILL
            process->terminate();
            if (!process->waitForFinished(2000)) {
                process->kill();
                process->waitForFinished(1000);
            }
        }
    }

    for (QProcess *process :
         {m_cloudflareBootstrapProcess,
          m_cloudflareTunnelBootstrapProcess}) {
        if (!process)
            continue;
        process->setProcessEnvironment(QProcessEnvironment());
    }
    m_cloudflareActiveSecret.fill(QChar(u'\0'));
    m_cloudflareActiveSecret.clear();
    for (QString &password : m_hostSessionPasswords)
        password.fill(QChar(u'\0'));
    m_hostSessionPasswords.clear();
    forkmesh::rewards::secureErase(m_microjobAutoPayoutPassphrase);
    m_microjobAutoPayoutPassphrase.clear();
    forkmesh::rewards::secureErase(m_contributorAutoPayoutPassphrase);
    m_contributorAutoPayoutPassphrase.clear();
    m_contributorAutoPayoutWallet.clear();
    m_contributorAutoPayoutScope.clear();
    forkmesh::rewards::secureErase(m_tipOneShotPassphrase);
    m_tipOneShotPassphrase.clear();
    for (TipTransferRequest &queued : m_tipTransferQueue)
        forkmesh::rewards::secureErase(queued.passphrase);
    m_tipTransferQueue.clear();

    forkmesh::setCrashContext(
        QStringLiteral("MainWindow teardown\nregistered diff views: %1")
            .arg(m_diffViews.size()));

    const QList<QTextEdit *> views = m_diffViews;
    for (QTextEdit *view : views) {
        if (!view)
            continue;
        if (QWidget *vp = view->viewport())
            vp->removeEventFilter(this);
        QObject::disconnect(view, nullptr, this, nullptr);
    }
    m_diffViews.clear();

    // Everything below this line runs in the base destructors, and by then
    // this window is no longer whole: C++ destroys a class's members before
    // any of its bases, so ~QWidget deletes the remaining children — the
    // long-lived dialogs among them — after every MainWindow member is gone.
    // The connections into this window are still live at that point, because
    // QObject only severs those later still, in its own destructor. QWidget
    // emits destroyed() for each child it deletes, so a handler bound to
    // `this` runs against members that no longer exist: the dialog handlers
    // clear QStringList/QHash members whose heap blocks were released moments
    // earlier, which frees them a second time and aborts the process inside
    // free() while Qt is midway through tearing that dialog down. Cut every
    // remaining child's path back into this window while the window is still
    // whole, so the base teardown can only free widgets.
    const QList<QObject *> descendants = findChildren<QObject *>();
    for (QObject *descendant : descendants)
        QObject::disconnect(descendant, nullptr, this, nullptr);

#ifdef FORKMESH_WINDOW_TESTS
    // Past here no MainWindow member survives; the teardown probe reads this
    // to tell a delivery during base destruction from an ordinary one.
    if (m_testTeardownBodyFinished)
        m_testTeardownBodyFinished->store(true, std::memory_order_release);
#endif
}

void MainWindow::destroyTransientOverlays()
{
    auto destroyTimer = [](QTimer *&timer) {
        QTimer *owned = timer;
        timer = nullptr;
        if (!owned)
            return;
        owned->stop();
        delete owned;
    };
    auto destroyOverlay = [](QWidget *&overlay) {
        QWidget *owned = overlay;
        overlay = nullptr;
        if (!owned)
            return;
        owned->hide();
        delete owned;
    };

    // A timer callback must not be able to address an overlay while that
    // overlay is being destroyed.
    destroyTimer(m_errorBorderTimer);
    destroyTimer(m_celebrationBorderTimer);
    destroyTimer(m_restartCautionBorderTimer);
    destroyOverlay(m_errorBorderOverlay);
    destroyOverlay(m_celebrationBorderOverlay);
    destroyOverlay(m_restartCautionBorderOverlay);
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testStartStallWatchdogForTeardown()
{
    if (!m_stallWatchdog)
        m_stallWatchdog = new StallWatchdog(this);
    m_stallWatchdog->start(/*stallThresholdMs=*/500);
}

bool MainWindow::testStallWatchdogRunning() const
{
    return m_stallWatchdog && m_stallWatchdog->isRunning();
}

void MainWindow::testInstallTeardownDeliveryProbe(
    std::atomic_bool *bodyFinished, std::atomic_bool *deliveredAfterBody,
    std::atomic_bool *probeDestroyed)
{
    m_testTeardownBodyFinished = bodyFinished;

    // The dialogs the crash reports named are children of this window that a
    // session can leave open: a QDialog holding a status QLabel and a log
    // QPlainTextEdit, deleted by ~QWidget rather than by a click.
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("teardownDeliveryProbeDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(QStringLiteral("teardown probe"), dialog));
    layout->addWidget(new QPlainTextEdit(dialog), 1);

    // Bound to `this`, exactly as the production dialog handlers are. The
    // capture is caller-owned storage, so a delivery after this window's
    // members are gone records the fault instead of compounding it.
    connect(dialog, &QObject::destroyed, this,
            [bodyFinished, deliveredAfterBody] {
                if (bodyFinished->load(std::memory_order_acquire))
                    deliveredAfterBody->store(true, std::memory_order_release);
            });
    // Bound to qApp, which outlives the window either way, so the probe can
    // still tell "never delivered" from "never destroyed".
    connect(dialog, &QObject::destroyed, qApp, [probeDestroyed] {
        probeDestroyed->store(true, std::memory_order_release);
    });
}
#endif

void MainWindow::drainOffThreadWorkers()
{
    m_acceptOffThreadResults.store(false, std::memory_order_release);
    const QList<QThread *> workers = m_offThreadWorkers.values();
    m_offThreadWorkers.clear();

    // finished() normally queues the set cleanup on this window's thread.
    // Teardown owns that cleanup synchronously instead: no callback may retain
    // a worker that is about to be deleted below.
    for (QThread *worker : workers)
        QObject::disconnect(worker, nullptr, this, nullptr);
    for (QThread *worker : workers)
        worker->wait();

    // A worker can have passed the accepting check just before teardown set
    // it false. Waiting makes that post complete; removing its MetaCall keeps
    // the stale apply callback from reaching widgets during base destruction.
    QCoreApplication::removePostedEvents(this, QEvent::MetaCall);
    for (QThread *worker : workers) {
        // Drop the last guaranteed apply-functor owner here on the GUI thread.
        // A worker that returned during teardown must never destroy captures
        // such as QPixmap/QIcon from its QThread::create callable.
        m_offThreadApplyHolders.remove(worker);
        delete worker;
    }
}

void MainWindow::restartMirrorSyncTimer()
{
    if (!m_mirrorSyncTimer)
        return;
    const qint64 configuredIntervalMs = mirrorSyncIntervalMs();
    // A healthy event socket makes the configured poll a safety pass, but it
    // must never make an operator's chosen cadence slower. Collaboration refs
    // can advance on a mirror without a code-commit frame, so the configured
    // one-minute default still needs to inspect them while connected.
    const qint64 baseIntervalMs =
        m_nodeEventSocket && m_nodeEventSocket->isConnected()
            ? qMin(configuredIntervalMs, kEventDrivenMirrorSafetyIntervalMs)
            : configuredIntervalMs;
    const qint64 jitterSpanMs = baseIntervalMs * kMirrorSyncJitterPercent / 100;
    const qint64 nextIntervalMs =
        baseIntervalMs +
        QRandomGenerator::global()->bounded(-jitterSpanMs, jitterSpanMs + 1);
    m_mirrorSyncTimer->start(int(qMax<qint64>(1000, nextIntervalMs)));
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    forkmesh::hiddenterms::setTerms(
        SavedSearchStore::loadHiddenTerms(SavedSearchStore::hiddenTermsPath()));
    forkmesh::hiddenterms::refreshVariableValues(ActionStore::variables());
    m_debugSessionUptime.start();
    logStartup(QStringLiteral("MainWindow ctor begin"));
    forkmesh::teammates::setRecentAgentChoiceSource(
        [this] { return promptAreaAgentChoice(); });
    if (supervisingSystemdUserServiceUnit() == QLatin1String(kSourceGuardUnit))
        releaseDesktopCloseHold(sourceGuardHoldPath());
    m_agentCapabilityBroker = std::make_unique<AgentCapabilityBroker>();
    m_agentPermissionController =
        std::make_unique<AgentPermissionController>(m_agentCapabilityBroker.get());
    QString capabilityStoreError;
    if (!AgentCapabilityStore::load(AgentCapabilityStore::defaultPath(),
                                    m_agentCapabilityBroker.get(),
                                    &capabilityStoreError)) {
        qWarning().noquote()
            << "Agent capability grants were not loaded:"
            << capabilityStoreError;
    }
    m_packageRegistry =
        std::make_unique<forkmesh::market::PackageRegistry>();
    const auto traceStep = [](const QString &name, auto &&work) {
        forkmesh::StartupTraceStep step(
            QStringLiteral("MainWindow: %1").arg(name));
        std::forward<decltype(work)>(work)();
    };
#ifndef FORKMESH_WINDOW_TESTS
    traceStep(QStringLiteral("start durable action telemetry"), [] {
        forkmesh::ActionTelemetry::initialize(
            forkmesh::paths::diagnosticsDir() + QStringLiteral("/actions.jsonl"));
    });
#endif
    QElapsedTimer chromeTimer;
    chromeTimer.start();
    startupStep(QStringLiteral("Installing shortcuts and window chrome"));
    if (qApp)
        qApp->installEventFilter(this);
    if (qApp)
        new forkmesh::ui::WindowFrameControl(this);
    Tooltips::install();
    auto *searchShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_K), this);
    searchShortcut->setContext(Qt::ApplicationShortcut);
    connect(searchShortcut, &QShortcut::activated, this,
            &MainWindow::openGlobalSearch);
    auto *findShortcut = new QShortcut(QKeySequence::Find, this);
    findShortcut->setObjectName(QStringLiteral("pageFindShortcut"));
    findShortcut->setContext(Qt::WindowShortcut);
    connect(findShortcut, &QShortcut::activated, this,
            &MainWindow::openPageFind);
    auto *viewShortcut =
        new QShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_L), this);
    viewShortcut->setObjectName(QStringLiteral("cycleViewShortcut"));
    viewShortcut->setContext(Qt::WindowShortcut);
    connect(viewShortcut, &QShortcut::activated, this,
            &MainWindow::cycleAppViewMode);
    setWindowTitle("ForkMesh v" FORKMESH_VERSION);
    setWindowIcon(QIcon(QStringLiteral(":/app/forkmesh.png")));
    setWindowFlag(Qt::FramelessWindowHint, true);
    if (forkmesh::ui::WindowFade::platformNeedsFade()) {
        forkmesh::ui::WindowFade::prepareHost(this);
        m_windowFade = new forkmesh::ui::WindowFade(this);
    }
    applyWindowOpacity(windowOpacityPercent());
    if (qApp && qApp->styleSheet().isEmpty())
        applyTheme(m_packageRegistry.get());
    resize(1060, 700);
    const QByteArray savedGeometry =
        QSettings().value(kWindowGeometrySetting).toByteArray();
    if (!savedGeometry.isEmpty())
        restoreGeometry(savedGeometry);

    startupDetail(savedGeometry.isEmpty()
                      ? QStringLiteral("No saved geometry; opening at 1060x700")
                      : QStringLiteral("Restored the last window geometry"));

    startupStep(QStringLiteral("Registering the system tray icon"));
    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("MainWindow: register the system tray icon"));
        m_trayIcon = new QSystemTrayIcon(this);
        m_trayIcon->setIcon(QIcon(QStringLiteral(":/app/forkmesh.png")));
        m_trayIcon->setToolTip("ForkMesh");
        const auto bringForward = [this] {
            setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            show();
            raise();
            activateWindow();
        };
        auto *trayMenu = new QMenu(this);
        connect(trayMenu->addAction(QStringLiteral("Open ForkMesh")), &QAction::triggered, this,
                bringForward);
        m_trayIcon->setContextMenu(trayMenu);
        connect(m_trayIcon, &QSystemTrayIcon::activated, this,
                [bringForward](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::Trigger)
                        bringForward();
                });
        if (QSystemTrayIcon::isSystemTrayAvailable())
            m_trayIcon->show();
    }
    // The tray only reports that *a* balloon was clicked, never which one, so a
    // click resolves against the newest notification's target.
    connect(m_trayIcon, &QSystemTrayIcon::messageClicked, this,
            [this] { activateDesktopNotification(m_desktopNotificationHref); });
    logStartup(
        QStringLiteral("DONE  MainWindow: configure window chrome, shortcuts, "
                       "geometry and tray icon (%1ms)")
            .arg(chromeTimer.elapsed()));

    connect(qApp, &QCoreApplication::aboutToQuit, this,
            &MainWindow::saveChatHistory);
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            &MainWindow::saveQuickAddDraft);
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            &MainWindow::clearAccountSessionMemory);

    QElapsedTimer networkTimer;
    networkTimer.start();
    startupStep(QStringLiteral("Bringing up networking and the request firewall"));
    auto *network = new BackoffNetworkAccessManager(this);
    // A build older than the minimum supported release is kept off the network
    // entirely. The version is handed to the manager here because this is one
    // of the few files that include the generated version header; the manager
    // then refuses every off-machine request, and the WebSocket transports ask
    // it the same question through authorizeFirewallConnection().
    network->setClientVersion(QStringLiteral(FORKMESH_VERSION));
    if (network->blockedByClientVersion()) {
        startupDetail(QStringLiteral("Network blocked: %1")
                          .arg(forkmesh::outdatedClientSummary(
                              network->clientVersion(),
                              network->minimumNetworkVersion())));
    }
    connect(network, &BackoffNetworkAccessManager::outdatedClientBlockedRequest,
            this, [this](const QString &, const QUrl &) {
                warnOutdatedClientVersion();
            });
    const bool firewallOn =
        QSettings().value(kRequestFirewallEnabledSetting, true).toBool();
    startupDetail(firewallOn
                      ? QStringLiteral("Request firewall: on")
                      : QStringLiteral("Request firewall: off"));
    network->setFirewallEnabled(firewallOn);
    network->setFirewallRules(requestFirewallWhitelist());
    network->setFirewallDenyRules(requestFirewallDenylist());
    network->setFirewallPrompt(
        [this](const QString &method, const QUrl &url, QString *ruleOut) {
            return promptFirewallRequest(method, url, ruleOut);
        });
    connect(network, &BackoffNetworkAccessManager::firewallRequestDecided, this,
            [this, network](const QString &method, const QUrl &url,
                            const QString &rule, bool allowed) {
                // Allow and Deny both leave a rule behind; persist both lists
                // so the decision survives a restart.
                QSettings settings;
                settings.setValue(kRequestFirewallWhitelistSetting,
                                  network->firewallRules());
                settings.setValue(kRequestFirewallDenylistSetting,
                                  network->firewallDenyRules());
                recordFirewallRequest(method, url, rule, allowed);
            });
    connect(network, &BackoffNetworkAccessManager::endpointStatsChanged, this,
            [this, network] {
        qint64 calls = 0;
        for (const BackoffNetworkAccessManager::EndpointStats &stats :
             network->endpointStats()) {
            calls += stats.calls;
        }
        m_debugNetworkRequestTotal = calls;
        if (m_sectionStack &&
            m_sectionStack->currentIndex() == kNetworkDiagnosticsSectionIndex)
            refreshNetworkDiagnostics();
        // The Stats page charts requests per destination with their firewall
        // status live. Coalesce a burst into one repaint a second; repaint
        // only, so the sampled per-refresh history keeps its cadence.
        if (m_statsDashboard && m_statsDashboard->isVisible() &&
            !m_statsNetworkRefreshQueued) {
            m_statsNetworkRefreshQueued = true;
            QTimer::singleShot(1000, this, [this] {
                m_statsNetworkRefreshQueued = false;
                if (m_statsDashboard && m_statsDashboard->isVisible())
                    m_statsDashboard->refresh();
            });
        }
    });
    // The Firewall Rules counts are live. A busy page decides several requests
    // a second, so a burst coalesces into one update a second.
    connect(network, &BackoffNetworkAccessManager::firewallRuleHitsChanged, this,
            [this] {
        if (m_firewallRuleCountsQueued || !m_sectionStack ||
            m_sectionStack->currentIndex() != kNetworkDiagnosticsSectionIndex)
            return;
        m_firewallRuleCountsQueued = true;
        QTimer::singleShot(1000, this, [this] {
            m_firewallRuleCountsQueued = false;
            updateFirewallRuleCounts();
        });
    });
    m_networkAccess = network;
    initOrganizationTaskStore();
    static bool proxyLookupWarmed = false;
    if (!std::exchange(proxyLookupWarmed, true)) {
        runOffThread<bool>(
            [] {
                QNetworkProxyFactory::systemProxyForQuery(
                    QNetworkProxyQuery(QUrl(kDefaultServerUrl)));
                return true;
            },
            [](bool) {});
    }
    connect(m_networkAccess, &QNetworkAccessManager::finished, this,
            [this](QNetworkReply *reply) {
                if (!reply)
                    return;
                // The shared manager records these in endpoint diagnostics,
                // but they never left this machine.  Logging every synthetic
                // reply as "net GET ERR ... backing off" made the cooldown
                // itself look like an outbound retry storm.
                if (BackoffNetworkAccessManager::isBackoffSuppressed(reply))
                    return;
                const bool offlineBlocked =
                    BackoffNetworkAccessManager::isOfflineBlocked(reply);
                const bool transportOffline =
                    !offlineBlocked && forkmesh::networkReplyLooksOffline(reply);
                const bool verbose =
                    QSettings().value(kVerboseNetworkLogSetting, false).toBool();
                // A closed failed reply used to produce several opaque Qt
                // QIODevice warnings.  Keep one useful line even when verbose
                // request logging is off: its method and URL identify the call
                // that could not reach the network.
                //
                // An offline-mode reply is different: the manager made it
                // locally after offline mode was already announced.  Do not
                // turn every paused poll into a connection failure; show its
                // underlying call only when verbose request logging is on.
                if (!verbose && !transportOffline)
                    return;
                // Favicon probes are speculative decoration with a designed
                // fallback to a generated letter mark, so a failed one is not
                // a connectivity event.  Both probes must be skipped: matching
                // only the root path let the relay-asset fallback
                // (/favicon/favicon.ico) log "Connection unavailable - you may
                // be offline" for an unreachable host, and an ERROR log line
                // raises a ping -- so every launch that tailed a CLOUD line
                // naming a synthetic host pinged the user about being offline
                // while the network was fine.  The probe is known by the mark
                // on its request, not by the reply's URL: a redirected probe
                // finishes on whatever page the host sent it to.
                if (forkmesh::networkReplyIsFaviconProbe(reply))
                    return;
                static const char *const verbs[] = {"HEAD",   "GET", "PUT",
                                                     "POST",   "DELETE",
                                                     "CUSTOM"};
                const int op = static_cast<int>(reply->operation());
                const QString verb = (op >= 1 && op <= 6)
                                         ? QLatin1String(verbs[op - 1])
                                         : QStringLiteral("REQ");
                QString status;
                if (reply->error() != QNetworkReply::NoError) {
                    if (offlineBlocked) {
                        status =
                            QStringLiteral("OFFLINE (not sent - offline mode is "
                                           "active)");
                    } else {
                        // A 5xx the Worker marked expected-degraded is a
                        // deliberate answer, not a fault: a public-snapshot
                        // barrier during a node deletion or a repo going private
                        // fail-closes these routes for a couple of minutes on
                        // purpose. Say so, so the line reads (and badges) as the
                        // transient it is rather than as a broken request. A
                        // status the caller named as an answer (a lookup's
                        // 404 "no such account") reads the same way.
                        const bool transportExpected =
                            forkmesh::networkReplyTransportFailureIsExpected(reply);
                        status = (transportExpected ||
                                  forkmesh::networkReplyIsExpectedDegraded(reply) ||
                                  forkmesh::networkReplyStatusIsExpected(reply))
                                     ? QStringLiteral("EXPECTED ")
                                     : QStringLiteral("ERR ");
                        status += transportOffline && !transportExpected
                                      ? forkmesh::networkReplyFailureSummary(reply)
                                      : forkmesh::networkFailureText(reply);
                    }
                    const QString snippet =
                        forkmesh::networkResponseSnippet(
                            forkmesh::networkReplyBodyPreview(reply, 512), 0);
                    if (!snippet.isEmpty())
                        status += QStringLiteral(" [body: ") + snippet +
                                  QStringLiteral("]");
                } else {
                    const QVariant code = reply->attribute(
                        QNetworkRequest::HttpStatusCodeAttribute);
                    status = code.isValid() ? code.toString()
                                            : QStringLiteral("done");
                    // Successful payloads include account and room data. Their
                    // contents are not needed to diagnose transport health and
                    // must never be copied into the append-only network log.
                }
                logSystem(QString::fromUtf8("net %1 %2 %3 \xC2\xB7 %4")
                              .arg(verb, status,
                                   forkmesh::networkLogUrl(reply->url()),
                                   networkRequestEventFor(reply->url())));
            });
    logStartup(
        QStringLiteral("DONE  MainWindow: configure shared network manager and "
                       "firewall (%1ms)")
            .arg(networkTimer.elapsed()));
    traceStep(QStringLiteral("read connection state and cached model list"),
              [this] {
                  m_totalConnectionMs =
                      QSettings().value(kConnectionTotalSetting).toLongLong();
                  // A node taken offline stays offline across restarts on
                  // purpose: it is a serving choice (no repo transport, no
                  // reward heartbeat), not a network switch, and re-opting in
                  // at every launch would publish availability the owner
                  // withdrew. Offline mode is the switch that pauses requests,
                  // and it has its own restore rule below.
                  m_nodeOffline =
                      QSettings().value(kNodeOfflineSetting, false).toBool();
                  restorePersistedOfflineMode();
                  m_liveClaudeModels =
                      QJsonDocument::fromJson(
                          QSettings()
                              .value(kClaudeModelsCacheSetting)
                              .toByteArray())
                          .array();
              });
    network->setOfflineMode(m_offlineMode);
    // Raised once the window is up rather than on the first blocked poll: a
    // retired build should say why nothing works before the reader watches it
    // fail.
    if (network->blockedByClientVersion())
        warnOutdatedClientVersion();
    if (m_offlineMode)
        startupDetail(QStringLiteral("Offline mode restored"));
    else if (m_offlineModeWasAutomaticLastSession)
        startupDetail(QStringLiteral(
            "Automatic offline mode not restored - retesting the relay"));

    startupStep(QStringLiteral("Loading relays, favicons and the network log"));
    traceStep(QStringLiteral("load relay list"), [this] { loadServers(); });
    traceStep(QStringLiteral("load cached relay favicons"),
              [this] { loadCachedFavicons(); });
    traceStep(QStringLiteral("restore persisted network log"),
              [this] { loadNetworkLog(); });
    traceStep(QStringLiteral("restore filed pings"),
              [this] { loadNotificationJournal(); });
    traceStep(QStringLiteral("schedule saved search pings"),
              [this] { scheduleSavedSearchAlerts(); });
    logSystem(QStringLiteral("════════════════════════════════════════════════════════════"));
    logSystem(QStringLiteral("Session started - ForkMesh v" FORKMESH_VERSION "."));
    logSystem(QStringLiteral("════════════════════════════════════════════════════════════"));
    logStartup(QStringLiteral("servers + favicons loaded"));
    startupDetail(QStringLiteral("%1 relay(s) known").arg(m_servers.size()));

    startupStep(QStringLiteral("Restoring the profile and node name"));

    traceStep(QStringLiteral("restore profile name and avatar"), [this] {
        m_userAvatar = UserAvatarStore::load();
        {
            const QString envName =
                accountNameFromInput(qEnvironmentVariable("FORKMESH_NODE_NAME"),
                                     QString());
            if (isValidNodeName(envName) &&
                envName.compare(savedProfileName(), Qt::CaseInsensitive) != 0)
                QSettings().setValue(kAccountNameSetting, envName);
        }
        if (savedProfileName().isEmpty()) {
            m_freshInstall = true;
            const QString generated = randomFunNodeName();
            QSettings().setValue(kAccountNameSetting, generated);
            QSettings().setValue(kGeneratedNodeNameSetting, generated);
        }
        if (const QString saved = savedProfileName().toLower(); !saved.isEmpty())
            m_userName = saved;
    });
    initializeFirstRunOnboardingState();
    startupDetail(m_freshInstall
                      ? QStringLiteral("First run — this node is now \"%1\"")
                            .arg(savedProfileName())
                      : QStringLiteral("Node name: %1").arg(savedProfileName()));

    startupStep(QStringLiteral("Building the account surface"));
    // A frameless window gets no compositor border. Keep one logical pixel of
    // this theme-coloured parent visible around every page instead.
    auto *appWindowFrame = new QWidget(this);
    appWindowFrame->setObjectName(QStringLiteral("appWindowFrame"));
    auto *appWindowLayout = new QVBoxLayout(appWindowFrame);
    appWindowLayout->setContentsMargins(1, 1, 1, 1);
    appWindowLayout->setSpacing(0);

    m_stack = new QStackedWidget(appWindowFrame);
    traceStep(QStringLiteral("build the account/setup page"),
              [this] { m_stack->addWidget(buildSetupPage()); });
    logStartup(QStringLiteral("setup page built"));

    appWindowLayout->addWidget(m_stack);
    setCentralWidget(appWindowFrame);
    applyWindowBackdrop();

    auto initializeApplication = [this, traceStep] {
    startupStep(QStringLiteral("Building the application shell"));
    installAgentActivityHost();
    installBackupHost();
    installCleanupHost();
    QWidget *applicationPage = nullptr;
    traceStep(QStringLiteral("build the application shell page"),
              [this, &applicationPage] {
                  applicationPage = buildChatPage();
              });
    if (!applicationPage || !m_stack)
        return;
    m_stack->addWidget(applicationPage);
#ifdef FORKMESH_WINDOW_TESTS
    syncNativePackagePages();
#else
    m_nativePackagePagesDeferred = true;
    logStartup(QStringLiteral(
        "installed package pages deferred until after first paint"));
#endif
    m_stack->setCurrentWidget(applicationPage);
    restoreAppViewMode();
    logStartup(QStringLiteral("chat/app page built"));
    startupStep(QStringLiteral("Deferring repository surfaces until opened"));
    // openRepoDetail() owns the shell and landing tab; the tab-change handler
    // owns every other page. A user who opens Chat, Tasks or Notes therefore
    // never pays to construct the Code/Issues widget graph.
    logStartup(QStringLiteral(
        "repository detail shell, Code and Issues deferred until opened"));
    startupStep(QStringLiteral("Loading repositories"));
    traceStep(QStringLiteral("initialise the world speech bridge"),
              [this] { initializeWorldSpeechBridge(); });
    traceStep(QStringLiteral("read the repository records"),
              [this] {
                  loadRelayRepositoryLists();
                  loadRepositories();
              });
    traceStep(QStringLiteral("refresh the repository list"),
              [this] { refreshRepositoryList(); });
    logStartup(QStringLiteral("repositories loaded"));
    startupDetail(
        QStringLiteral("%1 repositor%2 on this node")
            .arg(m_repositories.size())
            .arg(m_repositories.size() == 1 ? QStringLiteral("y")
                                            : QStringLiteral("ies")));
    startupStep(QStringLiteral("Restoring actions and agent sessions"));
    // Only the store and its runners here; reading the run records off disk is
    // the expensive half and waits for warmDeferredStartupSurfaces().
    traceStep(QStringLiteral("find the TMP disk"), [this] { initFastScratchDisk(); });
    traceStep(QStringLiteral("create the Actions store"),
              [this] { initActionStore(); });
    traceStep(QStringLiteral("restore agent sessions"), [this] { initAgents(); });
    // A bin whose window elapsed while ForkMesh was closed must not survive
    // the next launch, and a long-running session needs the sweep to keep
    // happening (issue #578).
    purgeExpiredAgentRecycleBinInBackground();
    m_agentRecycleSweepTimer = new QTimer(this);
    connect(m_agentRecycleSweepTimer, &QTimer::timeout, this,
            &MainWindow::purgeExpiredAgentRecycleBinInBackground);
    m_agentRecycleSweepTimer->start(60 * 60 * 1000);
#ifndef FORKMESH_WINDOW_TESTS
    startAgentParityWatch();
#endif
    logStartup(QStringLiteral("actions + agents initialized"));
    if (!m_agentQueue.isEmpty())
        startupDetail(QStringLiteral("%1 agent session(s) queued to resume")
                          .arg(m_agentQueue.size()));
    startupStep(QStringLiteral("Choosing the repository to reopen"));
    const QString lastRepository =
        QSettings().value(kLastRepositorySetting).toString();
    if (!lastRepository.isEmpty()) {
        const int slash = lastRepository.indexOf('/');
        if (slash > 0) {
            const int index =
                repoIndexFor(lastRepository.left(slash),
                             lastRepository.mid(slash + 1));
            if (index >= 0) {
                m_pendingRestoreRepoIndex = index;
            }
        }
    }
    logStartup(QStringLiteral("last repository restore scheduled"));
    startupDetail(m_pendingRestoreRepoIndex >= 0
                      ? QStringLiteral("Will reopen %1").arg(lastRepository)
                      : QStringLiteral("Nothing to reopen"));
    startupStep(QStringLiteral("Selecting the active relay"));
    traceStep(QStringLiteral("select the active relay"), [this] {
        loadActiveServerIntoEdits();
        // Repository records were loaded before the active relay controls so
        // the switcher needs one scoped rebuild after those controls are set.
        refreshRepositoryList();
        updateBreadcrumb();
    });
    logStartup(QStringLiteral("active server + breadcrumb"));
    startupStep(QStringLiteral("Fetching relay favicons"));
    traceStep(QStringLiteral("start relay favicon fetches"), [this] {
        for (int i = 0; i < m_servers.size(); ++i)
            fetchFavicon(i);
    });
    logStartup(QStringLiteral("favicons fetched"));

    startupStep(QStringLiteral("Arming background timers"));
    QElapsedTimer timerArmClock;
    timerArmClock.start();
    m_typingStopTimer = new QTimer(this);
    m_typingStopTimer->setSingleShot(true);
    connect(m_typingStopTimer, &QTimer::timeout, this, [this] {
        sendTypingState(false);
    });
    // Each periodic routine names itself for the footer's background outcome
    // lines (adhoc #186), so the reader can tell which timer asked for work.
    m_homeStatsTimer = new QTimer(this);
    connect(m_homeStatsTimer, &QTimer::timeout, this, [this] {
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("home/repository stats timer (every 60s)"));
        // Optional work. While the machine is paging, skip the tick rather
        // than add this pass's ref walks and git reads to a GUI thread that is
        // already waiting on the disk: on 2026-09-12 this timer's refresh was
        // the sampled frame inside a 95 s freeze (stalls.log 0020). Every
        // event-driven refresh still runs; only the periodic one yields.
        const forkmesh::SystemPressureSnapshot pressure =
            forkmesh::cachedSystemPressure();
        if (forkmesh::systemUnderPressure(pressure)) {
            if (forkmesh::announceDeferredWork(
                    QStringLiteral("home/repository stats"),
                    pressure.sampledAtMs)) {
                logSystem(QStringLiteral(
                              "Deferring the periodic repository stats refresh "
                              "while the machine is under pressure (%1).")
                              .arg(forkmesh::describeSystemPressure(pressure)));
            }
            return;
        }
        refreshRepositoryList();
    });
    m_homeStatsTimer->start(60000);
    logStartup(QStringLiteral("  timer armed: home/repository stats every 60s"));

    QTimer::singleShot(0, this, [this] {
        forkmesh::StartupTraceStep step(
            QStringLiteral("delayed startup: diagnostics and UI-stall watchdog"));
        startDiagnostics();
    });
#ifdef FORKMESH_WINDOW_TESTS
    // Functional fixtures install an Actions worker hook immediately after
    // construction and expect the historical first-idle restore trigger.
    QTimer::singleShot(0, this, &MainWindow::warmDeferredStartupSurfaces);
#endif
    logStartup(QStringLiteral(
        "  startup job scheduled: diagnostics on first event-loop turn; "
        "repository tabs stay lazy"));

    m_mirrorSyncTimer = new QTimer(this);
    connect(m_mirrorSyncTimer, &QTimer::timeout, this, [this] {
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("mirror safety sync timer (every %1)")
                .arg(timerCadenceText(m_mirrorSyncTimer
                                          ? m_mirrorSyncTimer->interval()
                                          : 0)));
        startMirrorSyncSweep();
    });
    restartMirrorSyncTimer();
    logStartup(QStringLiteral("  timer armed: mirror safety sync every %1ms")
                   .arg(m_mirrorSyncTimer->interval()));
#ifndef FORKMESH_WINDOW_TESTS
    QTimer::singleShot(1000, this, [this] {
        forkmesh::StartupTraceStep step(
            QStringLiteral("delayed startup: initial mirror synchronization"));
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("delayed startup: initial mirror synchronization"));
        startMirrorSyncSweep();
    });
#endif
    logStartup(QStringLiteral(
        "  startup job scheduled: initial mirror synchronization in 1000ms"));
#ifndef FORKMESH_WINDOW_TESTS
    QTimer::singleShot(20000, this, [this] {
        forkmesh::StartupTraceStep step(
            QStringLiteral("delayed startup: initial relay inbox sync"));
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("delayed startup: initial relay inbox sync"));
        performRelaySync();
    });
#endif
    logStartup(QStringLiteral(
        "  startup job scheduled: initial relay inbox sync in 20000ms"));
    m_autoUpdateTimer = new QTimer(this);
    connect(m_autoUpdateTimer, &QTimer::timeout, this, [this] {
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("automatic update timer (about hourly)"));
        maybeAutoUpdate();
    });
    m_autoUpdateTimer->start(
        60 * 60 * 1000 +
        int(QRandomGenerator::global()->bounded(-10 * 60 * 1000,
                                                10 * 60 * 1000 + 1)));
    const int initialUpdateDelayMs =
        5 * 60 * 1000 +
        int(QRandomGenerator::global()->bounded(10 * 60 * 1000));
    QTimer::singleShot(initialUpdateDelayMs, this, [this] {
        forkmesh::StartupTraceStep step(
            QStringLiteral("delayed startup: automatic update check"));
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("delayed startup: automatic update check"));
        maybeAutoUpdate();
    });
    logStartup(QStringLiteral(
                   "  timer armed: automatic update every %1ms; first check in %2ms")
                   .arg(m_autoUpdateTimer->interval())
                   .arg(initialUpdateDelayMs));
    installChatRetentionKeyResolver();
    m_chatExpiryTimer = new QTimer(this);
    connect(m_chatExpiryTimer, &QTimer::timeout, this,
            &MainWindow::pruneExpiredChatHistory);
    m_chatExpiryTimer->start(int(forkmesh::chatretention::kPruneIntervalMs));
    logStartup(QStringLiteral("  timer armed: expired chat pruning every %1ms")
                   .arg(forkmesh::chatretention::kPruneIntervalMs));
    m_relayLatencyTimer = new QTimer(this);
    // Local repaint only. This minute tick used to carry the /api/version
    // latency probe, the /api/status read and the /dashboard + /status page
    // loads, so every running desktop polled the relay four-plus times a
    // minute for footer dots. Those reads now happen at launch, when the
    // window is activated (refreshFooterStatusOnActivation) and when a dot is
    // rechecked; the websocket pong supplies latency in between, and a failed
    // probe re-arms itself until the relay answers again.
    connect(m_relayLatencyTimer, &QTimer::timeout, this, [this] {
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("node status repaint timer (every 60s)"));
        updateNodeOnlineControls();
        refreshNodeDotMatrix();
        if (m_offlineMode && m_offlineModeAuto)
            probeRelayLatency();
    });
    m_relayLatencyTimer->start(60 * 1000);
    logStartup(QStringLiteral("  timer armed: node status repaint every 60000ms"));
#ifndef FORKMESH_WINDOW_TESTS
    QTimer::singleShot(2500, this, [this] {
        forkmesh::StartupTraceStep step(
            QStringLiteral("delayed startup: first relay latency probe"));
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("delayed startup: first relay latency probe"));
        probeRelayLatency();
    });
#endif
    logStartup(QStringLiteral(
        "  startup job scheduled: first relay latency probe in 2500ms"));
    traceStep(QStringLiteral("start operating-system reachability watch"),
              [this] { initRelayReachabilityWatch(); });
#ifndef FORKMESH_WINDOW_TESTS
    QTimer::singleShot(25000, this, [this] {
        forkmesh::StartupTraceStep step(
            QStringLiteral("delayed startup: refresh organization task badge"));
        const forkmesh::BackgroundRoutine routine(
            QStringLiteral("delayed startup: refresh organization task badge"));
        refreshOrganizationTaskBadge();
    });
#endif
    logStartup(QStringLiteral(
        "  startup job scheduled: organization task refresh in 25000ms"));
    // ForkMesh Ads: one very coarse 30-minute timer and a first ask 20s in. It
    // declines by itself in a window-test build or on an offscreen node.
    scheduleAdBanner();
    logStartup(QStringLiteral("timers started (%1ms)")
                   .arg(timerArmClock.elapsed()));
    startupDetail(QStringLiteral(
        "Mirror sync, relay sync, chat expiry, latency, auto-update"));

    startupStep(QStringLiteral("Loading the node signing key"));
    forkmesh::beginStartupTrace();
    QElapsedTimer identityTimer;
    identityTimer.start();
    if (!m_profileIdentity.load()) {
        startupStepFailed(m_profileIdentity.errorString());
        m_setupError->setText(m_profileIdentity.errorString());
        m_setupError->show();
    } else {
            if (m_pubkeyLabel) {
                m_pubkeyLabel->setText("Ed25519 public key: " +
                                       m_profileIdentity.shortPublicKey());
                m_pubkeyLabel->setToolTip(m_profileIdentity.publicKey());
            }
        // A first run already got a generated fun name saved above, so
        // m_nameEdit is never blank here — the deferred auto-connect below
        // treats every launch the same way instead of stopping first runs on
        // the welcome screen. Auto-connect, but only after the first frame is painted (see
        // runDeferredStartup) — authenticateSilently()/startSession() block the
        // GUI thread, so running them before the window is exposed shows a black
        // frame on launch.
        m_pendingSilentAuth = true;
        startupDetail(QStringLiteral("Ed25519 key %1")
                          .arg(m_profileIdentity.shortPublicKey()));
    }
    logStartup(QStringLiteral("identity loaded (%1ms)")
                   .arg(identityTimer.elapsed()));
    // The session's ServerNode derives the shared-room key in its constructor
    // (210k PBKDF2 rounds, ~0.2 s) on the GUI thread when startSession() runs
    // a few seconds from now. Derive it into the process-wide cache on a
    // worker in the meantime so the constructor finds it ready.
    {
        QThread *warm = QThread::create([] {
            RoomCrypto::warmDefaultKey(kDefaultRoomName);
        });
        connect(warm, &QThread::finished, warm, &QObject::deleteLater);
        warm->start();
    }
    startupStep(QStringLiteral("Computing home statistics"));
    traceStep(QStringLiteral("compute home statistics"),
              [this] { updateHomeStats(); });
    traceStep(QStringLiteral("populate Hosts navigation count"),
              [this] { refreshHostsTable(); });
    traceStep(QStringLiteral("probe relays for the Nodes page"),
              [this] { refreshRelayNodes(); });
    // Every saved host's logs stream into the main log from here on.
    QTimer::singleShot(15 * 1000, this, [this] {
        m_hostLogFeedsArmed = true;
        syncHostLogFeeds();
        m_coreLogFeedsArmed = true;
        syncCoreLogFeeds();
        startDeployLogFollower();
    });
    logStartup(QStringLiteral(
        "application shell and startup models initialized"));
    };

    initializeApplication();
    forkmesh::ui::NextAccess::install(*this);
    logStartup(QStringLiteral("MainWindow ctor complete"));
}

void MainWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_firstRunWelcomePending && !m_firstRunWelcomeScheduled) {
        m_firstRunWelcomeScheduled = true;
#ifdef FORKMESH_WINDOW_TESTS
        QTimer::singleShot(0, this, &MainWindow::maybeShowFirstRunWelcome);
#else
        // Let the first frame settle before placing the welcome card over the
        // real shell. If the startup splash is still narrating work, the helper
        // waits for it rather than competing for the first-run user's attention.
        QTimer::singleShot(450, this, &MainWindow::maybeShowFirstRunWelcome);
#endif
    }
    if (m_deferredStartupStarted)
        return;
    m_deferredStartupStarted = true;
    logStartup(QStringLiteral(
        "first window show observed; deferred authentication and repository "
        "restore intentionally scheduled in 5000ms to protect first paint"));
#ifndef FORKMESH_WINDOW_TESTS
    // An Expose event is not proof that Qt has painted the first frame. Give
    // the event loop a five-second interactive runway so authentication and
    // session restoration cannot collide with the user's first navigation.
    // runDeferredStartup contains the no-paint fallback for headless platforms.
    QTimer::singleShot(forkmesh::isBackgroundInstance() ||
                               forkmesh::tookOverFromBackgroundInstance()
                           ? 0
                           : 5000,
                       this, &MainWindow::runDeferredStartup);
#endif
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    QMainWindow::paintEvent(event);
    if (m_firstFramePainted)
        return;
    m_firstFramePainted = true;
    setProperty("forkmeshFirstFramePainted", true);
    forkmesh::logStartupTrace(QStringLiteral("first main-window paint"));
    finishPendingRestartTiming();
    startupWindowPainted(QStringLiteral("Main window painted"));
    // The shell is fully constructed before show(), while page-specific
    // surfaces remain lazy. Dismiss the splash immediately on the first paint;
    // there is no intermediate loading page to expose.
    finishStartupSplash(QStringLiteral("Workspace open"));
    dismissStartupSplash();
    // The global prompt is useful everywhere but its model controls are not
    // required to paint Home. Let the first frame reach the compositor, then
    // attach the real composer without showing an intermediate loader.
    QTimer::singleShot(100, this,
                       &MainWindow::initializeDeferredGlobalLogDock);
    QTimer::singleShot(150, this,
                       &MainWindow::buildDeferredNativePackagePages);
}

// Background state needed for automatic work after the interactive runway.
// Repository Code/Issues tabs deliberately do not warm here: openRepoDetail()
// and the tab-change handler build exactly the surface the user requested.
void MainWindow::warmDeferredStartupSurfaces()
{
    if (m_startupSurfacesWarmed)
        return;
    m_startupSurfacesWarmed = true;
    forkmesh::StartupTraceStep warmStep(
        QStringLiteral("deferred warm-up: Actions history worker"));
    // The scan itself is off-thread. Its queued/running workflows must be
    // restored even when the user never opens Actions, unlike visual tabs.
    restoreActionRuns();
}

void MainWindow::runDeferredStartup()
{
    // Restore durable Actions state before resuming queued work. Repository
    // panels remain on-demand and are built by the exact restore/navigation.
    warmDeferredStartupSurfaces();
    buildDeferredNativePackagePages();
    if (m_deferredStartupRun) {
        logStartup(QStringLiteral(
            "deferred startup requested again; ignored because it already ran"));
        return;
    }
    m_deferredStartupRun = true;
    forkmesh::StartupTraceStep deferredStep(
        QStringLiteral("deferred startup: restore repository, session and agents"));
    if (QWindow *handle = windowHandle())
        handle->removeEventFilter(this);

    auto startNetworking = [&] {
        forkmesh::StartupTraceStep networkingStep(
            QStringLiteral("deferred startup: authenticate and start networking"));
        if (m_pendingSilentAuth) {
            m_pendingSilentAuth = false;
            const QString name = m_nameEdit
                                     ? m_nameEdit->text().trimmed().toLower()
                                     : QString();
            if (!name.isEmpty() && isValidNodeName(name)) {
                {
                    forkmesh::StartupTraceStep step(
                        QStringLiteral("deferred startup: silent authentication"));
                    startupStep(QStringLiteral("Signing in as %1").arg(name));
                    authenticateSilently(name);
                }
                {
                    forkmesh::StartupTraceStep step(
                        QStringLiteral("deferred startup: start mesh session"));
                    startupStep(QStringLiteral("Connecting to the mesh"));
                    startSession();
                }
            } else {
                logStartup(QStringLiteral(
                    "  silent authentication skipped: no valid saved node name"));
            }
        } else {
            logStartup(QStringLiteral(
                "  silent authentication skipped: no pending authentication"));
        }
        m_startupAuthResolved = true;
        updateSignInButton();
        preparePackagePages();
    };

    if (m_pendingRestoreRepoIndex >= 0 &&
        m_pendingRestoreRepoIndex < m_repositories.size()) {
        const int index = m_pendingRestoreRepoIndex;
        logStartup(QStringLiteral("restoring saved repository index %1")
                       .arg(index));
        m_selectedNode = m_repositories.at(index).owner;
        startupStep(QStringLiteral("Reopening %1/%2")
                        .arg(m_repositories.at(index).owner,
                             m_repositories.at(index).name));
        {
            forkmesh::StartupTraceStep step(QStringLiteral(
                "deferred startup: refresh repository list before restore"));
            refreshRepositoryList();
        }
        {
            forkmesh::StartupTraceStep step(
                QStringLiteral("deferred startup: open saved repository detail"));
            openRepoDetail(index);
        }
        const int savedTab =
            QSettings().value(kLastRepoDetailTabSetting, -1).toInt();
        if (savedTab == kRetiredMirrorNodesDetailTab) {
            QSettings().setValue(kLastRepoDetailTabSetting, 0);
            showMirrorNodesOnNetwork(index);
        } else if (savedTab >= 0) {
            startupStep(QStringLiteral("Restoring the last open tab"));
            {
                forkmesh::StartupTraceStep step(QStringLiteral(
                    "deferred startup: build saved repository tab %1")
                                                    .arg(savedTab));
                ensureRepoDetailTabBuilt(savedTab);
            }
            NavPlace target;
            target.section = 0;
            target.repoIndex = index;
            target.detailTab = savedTab;
            {
                forkmesh::StartupTraceStep step(QStringLiteral(
                    "deferred startup: restore saved repository navigation"));
                applyNavDetailTab(target);
            }
        }
        logStartup(QStringLiteral("saved repository detail restore complete"));
        logStartup(QStringLiteral(
            "  scheduling background issue metadata reload after restore"));
        reloadIssuesInBackground();
    } else if (m_repoDetailIndex < 0) {
        int firstRepo = -1;
        for (const RepoMenuEntry &entry : std::as_const(m_repoMenuEntries))
            if (entry.index >= 0) {
                firstRepo = entry.index;
                break;
            }
        if (firstRepo >= 0) {
            forkmesh::StartupTraceStep step(
                QStringLiteral("deferred startup: open first available repository"));
            startupStep(QStringLiteral("Opening %1/%2")
                            .arg(m_repositories.at(firstRepo).owner,
                                 m_repositories.at(firstRepo).name));
            openRepoDetail(firstRepo);
        } else {
            logStartup(QStringLiteral(
                "  repository restore skipped: no saved or available repository"));
        }
    }
    m_pendingRestoreRepoIndex = -1;

    {
        forkmesh::StartupTraceStep step(QStringLiteral(
            "deferred startup: migrate legacy prompt image attachments"));
        startupStep(QStringLiteral("Checking prompt attachments"));
        AgentPromptImages::migrateLegacy();
    }

    if (!m_agentQueue.isEmpty()) {
        forkmesh::StartupTraceStep step(
            QStringLiteral("deferred startup: resume %1 queued agent session(s)")
                .arg(m_agentQueue.size()));
        startupStep(QStringLiteral("Resuming %1 agent session(s)")
                        .arg(m_agentQueue.size()));
        m_agentQuietResume = true;
        processAgentQueue();
        m_agentQuietResume = false;
    } else {
        logStartup(QStringLiteral(
            "  agent resume skipped: no queued sessions from the prior run"));
    }
    restoreUsageLimitAgentHolds();

    startNetworking();
    restorePendingPullSubmissions();

    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("deferred startup: refresh Claude Code usage"));
        startupStep(QStringLiteral("Refreshing Claude Code usage"));
        refreshClaudeCodeUsage();
    }

    {
        forkmesh::StartupTraceStep step(
            QStringLiteral("deferred startup: arm automatic backups"));
        startupStep(QStringLiteral("Arming automatic backups"));
        startAutoBackups();
    }

    // A hand-picked offline mode that survived the restart says so now that
    // there is a shell to say it on: otherwise the session opens with every
    // relay panel failing, which reads as a broken install (issue #712).
    if (projectInstancePending()) {
        m_offlineModeAnnouncementPending = false;
        QTimer::singleShot(0, this, &MainWindow::promptChooseProjectInstance);
    } else {
        announceRestoredOfflineMode();
    }

    // This is the end of startup as a user experiences it: signed in, session
    // up, last repository open on its last tab, agents resumed. The splash has
    // narrated the whole way here rather than bowing out at first paint, so
    // this is where it says Ready and fades. What is left below this line is
    // long-fuse background work (the sync timers armed in the constructor)
    // that the app is fully usable
    // without. No-op when no splash is up or the user turned it off.
    finishStartupSplash(QStringLiteral("Ready"));
}

void MainWindow::applyTheme(forkmesh::market::PackageRegistry *registry)
{
    if (!qApp)
        return;

    // applyTheme() can run before this window's package registry is ready, so
    // keep a short-lived startup fallback. Once it is ready, this window is the
    // authority: choosing an arbitrary top-level MainWindow made a second
    // window apply the first window's package state.
    std::unique_ptr<forkmesh::market::PackageRegistry> startupRegistry;
    if (!registry) {
        startupRegistry =
            std::make_unique<forkmesh::market::PackageRegistry>();
        registry = startupRegistry.get();
    }

    QSettings settings;
    QString preference =
        settings.value(kThemeSetting, QStringLiteral("system")).toString();
    const forkmesh::market::PackageManifest *marketTheme = nullptr;
    if (preference.startsWith(kMarketplaceThemePrefix) && registry &&
        registry->isReady()) {
        const QString id = preference.mid(kMarketplaceThemePrefix.size());
        const auto *candidate = registry->package(id);
        if (candidate && candidate->type == QLatin1String("theme") &&
            registry->isInstalled(id) &&
            !candidate->themeStyleSheetPath.isEmpty()) {
            marketTheme = candidate;
        }
    }
    if (preference.startsWith(kMarketplaceThemePrefix) && !marketTheme) {
        // A removed or damaged package must never strand the UI on a stale
        // stylesheet. System is the neutral, always-available fallback.
        preference = QStringLiteral("system");
        settings.setValue(kThemeSetting, preference);
    }

    if (marketTheme) {
        forkmesh::instancetheme::apply({});
    } else if (preference == kHalloweenTheme) {
        const QMap<QString, QString> halloween = Theme::halloweenRoleColors();
        Theme::setRoleOverrides(halloween, halloween);
    } else {
        forkmesh::instancetheme::applyStored();
    }
    bool dark = true;
    QMap<QString, QString> palette;
    QString overlay;
    QString activeThemeId;
    if (marketTheme) {
        dark = marketTheme->themeBase != QLatin1String("light");
        palette = marketTheme->themePalette;
        QFile styleSheetFile(marketTheme->themeStyleSheetPath);
        if (styleSheetFile.open(QIODevice::ReadOnly))
            overlay = QString::fromUtf8(styleSheetFile.readAll());
        activeThemeId = marketTheme->id;
        settings.setValue(kThemeBaseDarkSetting, dark);
    } else {
        settings.remove(kThemeBaseDarkSetting);
        invalidateThemeIsDarkCache();
        dark = currentThemeIsDark();
    }

    const Theme::Design design =
        marketTheme ? Theme::Design::Classic : Theme::designFromSettings();
    Theme::setActiveDesign(design, dark);
    if (design == Theme::Design::Next)
        palette = Theme::nextPalette(dark);
    else if (!marketTheme)
        palette = Theme::overridePalette(design, dark);
    Theme::setActivePalette(palette);
    invalidateThemeIsDarkCache(); // the only place the answer can have moved
    qApp->setPalette(Theme::paletteForDesign(design, dark));
    qApp->setProperty("forkmeshTheme", activeThemeId);
    qApp->setProperty("forkmeshDesign", Theme::designSettingValue(design));
    static const QFont classicFont = qApp->font();
    const QFont interfaceFont = Theme::interfaceFont(design, classicFont);
    if (qApp->font().families() != interfaceFont.families())
        qApp->setFont(interfaceFont);
    const QString styleSheet =
        design == Theme::Design::Next
            ? Theme::nextStyleSheet(dark)
            : Theme::styleSheetForPalette(dark, palette, overlay);
    if (qApp->styleSheet() != styleSheet) {
        if (!qApp->styleSheet().isEmpty())
            qApp->setStyleSheet(QString());
        qApp->setStyleSheet(styleSheet);
    }
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        widget->setProperty("forkmeshTheme", activeThemeId);
        if (auto *window = qobject_cast<MainWindow *>(widget)) {
            window->refreshAppThemeChoice();
            window->refreshThemedIcons();
            window->refreshAgentTranscriptTheme(dark);
            window->schedulePackageRailRefresh();
            window->styleFooterUpdateLog();
            window->renderPageNotices(true);
            if (window->m_messageLayout)
                window->scheduleConversationRebuild(window->m_stickToBottom);
            window->refreshDesktopPackageExtensions();
            forkmesh::ui::NextAccess::themeChanged(*window);
        }
        widget->update();
    }
}

void MainWindow::refreshThemedIcons()
{
    const auto buttons = findChildren<QPushButton *>();
    for (QPushButton *button : buttons)
        applyStoredOcticon(button);
    const auto labels = findChildren<QLabel *>();
    for (QLabel *label : labels)
        applyStoredLabelOcticon(label);
    const auto tabWidgets = findChildren<QTabWidget *>();
    for (QTabWidget *tabs : tabWidgets)
        refreshTabOcticons(tabs);
    refreshSettingsSectionIcons();
    refreshComposerIcons();
    refreshChatWorkspaceUser();
    if (m_chatMembersLayout) {
        m_chatMembersSignature.clear();
        refreshChatMembers();
    }
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.widget)
            page.widget->themeChanged();
    }
}

void MainWindow::flushDeferredSettings()
{
    if (m_lastRepoDetailTabPending >= 0) {
        forkmesh::settings::put(QSettings(), kLastRepoDetailTabSetting,
                                m_lastRepoDetailTabPending);
        m_lastRepoDetailTabPending = -1;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    m_closingDown = true;
    flushPendingAgentSessionSaves(); // token counts coalesced off the stream
    if (m_captureFeature)
        m_captureFeature->shutdown();
    setCloudLogMonitorEnabled(false);
    stopCoreLogFeeds();
    if (m_deployLogFollower)
        m_deployLogFollower->stop();
    // Stop WebSocket-backed services while the event loop can still drain
    // their close frames. Letting QObject destruction tear down their TCP
    // children produces code 1006 close events in the Durable Objects.
    stopNodeEventSocket();
    if (m_officeChannelMirror)
        m_officeChannelMirror->stop();
    if (m_discordClient)
        m_discordClient->stop();
    if (m_backend)
        m_backend->shutdown();
    if (!forkmesh::isBackgroundInstance()) {
        QSettings().setValue(kWindowGeometrySetting, saveGeometry());
        savePromptOverlayPlacement();
    }
    flushDeferredSettings();
    if (m_connectedAtMs > 0)
        QSettings().setValue(kConnectionTotalSetting,
                             m_totalConnectionMs +
                                 QDateTime::currentMSecsSinceEpoch() -
                                 m_connectedAtMs);
    saveChatHistory();
    saveNotificationJournal();
    logSystem(QStringLiteral("════════════════════════════════════════════════════════════"));
    logSystem(QStringLiteral("Session ended."));
    logSystem(QStringLiteral("════════════════════════════════════════════════════════════"));
    persistNetworkLogOnShutdown();
    forkmesh::background::handOverAfterWindowClose(supervisingUserServiceRestartRequested());
    // A user-service unit can deliberately use Restart=always for crash
    // recovery. Tell that manager an interactive close is intentional so it
    // does not immediately reopen the desktop.
    stopSupervisingUserServiceAfterWindowClose();
    QMainWindow::closeEvent(event);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange)
        refreshDirectFocus();
    // Names the refreshes below for the footer's outcome lines: a `git status`
    // that ran because the window came back to the front says so.
    const forkmesh::BackgroundRoutine routine(
        event->type() == QEvent::ActivationChange && isActiveWindow()
            ? QStringLiteral("window activation refresh")
            : QString());
    if (event->type() == QEvent::ActivationChange && isActiveWindow() &&
        m_scmTree && m_scmTree->isVisible()) {
        refreshSourceControl();
        refreshCommitMarkersIfStale();
    }
    if (event->type() == QEvent::ActivationChange && isActiveWindow() &&
        m_railGitButton && m_railGitButton->isVisible())
        refreshRepoChangeBadge();
    if (event->type() == QEvent::ActivationChange && isActiveWindow()) {
        clearActiveConversationUnread();
        if (m_agentTable && m_agentTable->isVisible())
            markAgentSessionRead(m_selectedAgentSessionId);
        refreshFooterStatusOnActivation();
        refreshAccountPackages();
        refreshAdBannerOnActivation();
        // The account event socket is what normally says a mirror started or
        // finished a run; this is the catch-up for the frames that were rung
        // while it was down. Throttled inside, and it costs one request.
        refreshCloudAgentSessions();
    }
}
