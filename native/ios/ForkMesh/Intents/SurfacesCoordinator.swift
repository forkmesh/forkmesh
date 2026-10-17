import ActivityKit
import FMAccount
import FMAgents
import FMApi
import FMBase
import FMControl
import FMPackages
import FMPlanning
import FMSurfaces
import FMTeammates
import Foundation
import Observation
import UIKit
import WidgetKit

@MainActor
final class SurfacesCoordinator {
    static var shared: SurfacesCoordinator? { Services.get(SurfacesCoordinator.self) }

    static let staleActivityAfter: TimeInterval = 15 * 60
    static let dismissAfter: TimeInterval = 15 * 60
    static let refreshAfterMs: Int64 = 2 * 60 * 1_000
    static let workRefreshAfterMs: Int64 = 5 * 60 * 1_000

    private let services: AppServices
    private let store: SurfaceSnapshotStore?
    let spotlight: SpotlightIndexer
    private var timer: TimerActivitySnapshot?
    private var publishTask: Task<Void, Never>?
    private var notificationsToken: ListenerToken?
    private var authToken: ListenerToken?
    private var lifecycleToken: UUID?
    private var scope = ""
    private var firstSeenActive: [String: Int64] = [:]
    private var lastRefreshMs: Int64 = 0
    private var lastAgents: AgentsSurface = .empty
    private var work: WorkSurface = .empty
    private var lastWorkRefreshMs: Int64 = 0
    private var workTask: Task<Void, Never>?
    private var pickingUpShare = false
    private var observingShortcuts = false

    init(services: AppServices) {
        self.services = services
        self.store = SurfaceSnapshotStore.shared()
        self.spotlight = SpotlightIndexer()
    }

    private var now: Int64 { SystemClock().nowMillis }

    private var signedIn: Bool { !(services.auth.session?.nodeName ?? "").isEmpty }

    func start() {
        SurfaceCommandCenter.shared.handler = { [weak self] (command: SurfaceCommand) in
            guard let self else { throw SurfaceCommandError.appNotReady }
            try await self.handle(command)
        }
        PlanningTimersRuntime.shared()?.onActivityChange = { [weak self] (snapshot: TimerActivitySnapshot?) in
            self?.timerChanged(snapshot)
        }
        notificationsToken = services.notifications.addListener { [weak self] in
            MainActor.assumeIsolated { self?.schedulePublish() }
        }
        authToken = services.auth.addListener { [weak self] in
            MainActor.assumeIsolated { self?.sessionChanged() }
        }
        lifecycleToken = services.lifecycle.observe { [weak self] (state: AppLifecycleState) in
            if state == .resumed {
                self?.appResumed()
            }
        }
        observeAgents()
        observeApprovals()
        observeShortcuts()
        watchActivities()
        sessionChanged()
        appResumed()
    }

    private func observeAgents() {
        guard let center = HostsFeature.agents else { return }
        withObservationTracking {
            _ = center.rows
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.schedulePublish()
                self?.observeAgents()
            }
        }
    }

    private func observeApprovals() {
        guard let queue = AgentOpsFeature.approvals else { return }
        withObservationTracking {
            _ = queue.approvals
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.schedulePublish()
                self?.observeApprovals()
            }
        }
    }

    private func observeShortcuts() {
        guard let store = TeammatesFeature.shortcuts else { return }
        observingShortcuts = true
        withObservationTracking {
            _ = store.runs
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.schedulePublish()
                self?.observeShortcuts()
            }
        }
    }

    func schedulePublish() {
        publishTask?.cancel()
        publishTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 400_000_000)
            guard !Task.isCancelled else { return }
            self?.publish()
        }
    }

    func currentAgents() -> AgentsSurface {
        guard signedIn else { return .empty }
        let rows: [UnifiedAgentRow] = HostsFeature.agents?.rows ?? []
        let clock = now
        var seen: [String: Int64] = [:]
        let inputs: [AgentRunInput] = rows.map { (row: UnifiedAgentRow) -> AgentRunInput in
            if row.isActive {
                seen[row.id] = firstSeenActive[row.id] ?? clock
            }
            return AgentRunInput(
                id: row.id, title: row.title, repoOwner: row.repoOwner, repoName: row.repoName, status: row.status,
                isActive: row.isActive, provider: row.provider, model: row.model,
                detail: row.node.isEmpty ? "" : "On \(row.node)", updatedAtMs: Int64(row.updatedAtMs),
                startedAtMs: seen[row.id] ?? 0, localId: row.localId, isDesktop: row.isDesktop, canStop: row.canStop)
        }
        firstSeenActive = seen
        let approvals: [AgentApprovalInput] = (AgentOpsFeature.approvals?.approvals ?? []).map {
            (approval: PendingApproval) -> AgentApprovalInput in
            AgentApprovalInput(
                id: approval.id, sessionId: approval.sessionId,
                question: approval.question.isEmpty ? approval.command : approval.question,
                askedAtMs: approval.askedAtMs)
        }
        return AgentsSurface.build(runs: inputs, approvals: approvals)
    }

    private func publish() {
        let generated: Int64 = now
        guard signedIn else {
            write(SurfaceSnapshot.signedOut(at: generated))
            return
        }
        let agents: AgentsSurface = currentAgents()
        lastAgents = agents
        if !observingShortcuts { observeShortcuts() }
        AgentVoiceSource.rowsChanged(HostsFeature.agents?.rows ?? [])
        let notifications: NotificationService = services.notifications
        let inbox: InboxSurface = InboxSurface.build(
            unread: notifications.unreadCount, notifications: notifications.notifications)
        write(SurfaceSnapshot(
            generatedAtMs: generated, signedIn: true, agents: agents, inbox: inbox, work: work, timer: timer))
        updateSummaryActivity(ActivitySummaryAttributes.ContentState.build(
            agents: agents, shortcuts: currentShortcuts(), timer: timer, unread: notifications.unreadCount))
    }

    private func currentShortcuts() -> [ActivityShortcutInput] {
        let runs: [ShortcutRun] = Services.get(ShortcutsStore.self)?.newestRuns ?? []
        return runs.map { (run: ShortcutRun) -> ActivityShortcutInput in
            ActivityShortcutInput(
                id: run.id, title: run.title, repo: run.repo, running: run.running, startedAtMs: run.startedAtMs,
                expectedMs: run.expectedMs)
        }
    }

    private func write(_ snapshot: SurfaceSnapshot) {
        guard let store else { return }
        for section in store.write(snapshot) {
            WidgetCenter.shared.reloadTimelines(ofKind: section.widgetKind)
        }
    }

    private var agentSourceReliable: Bool {
        guard let center = HostsFeature.agents else { return false }
        return !center.loading && center.desktopNote.isEmpty && center.cloudNote.isEmpty
    }

    private func updateSummaryActivity(_ next: ActivitySummaryAttributes.ContentState) {
        guard ActivityAuthorizationInfo().areActivitiesEnabled else { return }
        let live: [Activity<ActivitySummaryAttributes>] = Activity<ActivitySummaryAttributes>.activities.filter {
            $0.activityState == .active || $0.activityState == .stale
        }
        for extra in live.dropFirst() {
            Task { await extra.end(nil, dismissalPolicy: .immediate) }
        }
        let current: Activity<ActivitySummaryAttributes>? = live.first
        let canStart: Bool = UIApplication.shared.applicationState == .active && signedIn
        guard let action = ActivitySummaryPlanner.plan(
            next: next, current: current?.content.state, canStart: canStart, agentsReliable: agentSourceReliable)
        else { return }
        var stale: Date = Date().addingTimeInterval(SurfacesCoordinator.staleActivityAfter)
        if let ends = next.timer?.endsAt { stale = max(stale, ends.addingTimeInterval(60)) }
        switch action {
        case let .start(state):
            do {
                _ = try Activity.request(
                    attributes: ActivitySummaryAttributes(), content: ActivityContent(state: state, staleDate: stale),
                    pushType: nil)
            } catch {
                Log.warning("surfaces", "Live Activity not started: \(error)")
            }
        case let .update(state):
            guard let current else { return }
            Task { await current.update(ActivityContent(state: state, staleDate: stale)) }
        case let .end(state):
            guard let current else { return }
            Task {
                await current.end(
                    ActivityContent(state: state, staleDate: nil),
                    dismissalPolicy: .after(Date().addingTimeInterval(SurfacesCoordinator.dismissAfter)))
            }
        }
    }

    private func watchActivities() {
        Task { [weak self] in
            for await activity in Activity<ActivitySummaryAttributes>.activityUpdates {
                self?.activityAppeared(activity)
            }
        }
    }

    private func activityAppeared(_ activity: Activity<ActivitySummaryAttributes>) {
        if !signedIn {
            Task { await activity.end(nil, dismissalPolicy: .immediate) }
        }
    }

    private func timerChanged(_ snapshot: TimerActivitySnapshot?) {
        timer = snapshot
        schedulePublish()
    }

    private func sessionChanged() {
        let account: String = (services.auth.session?.nodeName ?? "").lowercased()
        let next: String = account.isEmpty ? "" : "\(services.api.gitOrigin)|\(account)"
        if next == scope { return }
        let previous: String = scope
        scope = next
        if !previous.isEmpty {
            forgetAccount()
        }
        if !next.isEmpty {
            refreshWorkIfStale()
            Task { [weak self] in await self?.pickUpShare() }
        }
        schedulePublish()
    }

    private func forgetAccount() {
        firstSeenActive.removeAll()
        lastAgents = .empty
        workTask?.cancel()
        workTask = nil
        work = .empty
        lastWorkRefreshMs = 0
        spotlight.clear()
        ShareHandoffStore.appGroup()?.removeAll()
        ShareKeychainDrop().clear()
        _ = SurfaceCommandCenter.shared.drainPending()
        for activity in Activity<ActivitySummaryAttributes>.activities {
            Task { await activity.end(nil, dismissalPolicy: .immediate) }
        }
        write(SurfaceSnapshot.signedOut(at: now))
    }

    private func appResumed() {
        for command in SurfaceCommandCenter.shared.drainPending() {
            Task { [weak self] in try? await self?.handle(command) }
        }
        Task { [weak self] in await self?.pickUpShare() }
        refreshAgentsIfStale()
        refreshWorkIfStale()
        schedulePublish()
    }

    private static func configuredWidgets() async -> [WidgetInfo] {
        await withCheckedContinuation { continuation in
            WidgetCenter.shared.getCurrentConfigurations { result in
                continuation.resume(returning: (try? result.get()) ?? [])
            }
        }
    }

    private func refreshAgentsIfStale() {
        guard signedIn, let center = HostsFeature.agents, now - lastRefreshMs > SurfacesCoordinator.refreshAfterMs else {
            return
        }
        lastRefreshMs = now
        Task {
            let configured: [WidgetInfo] = await SurfacesCoordinator.configuredWidgets()
            let live: Bool = !Activity<ActivitySummaryAttributes>.activities.isEmpty
            if configured.isEmpty && !live { return }
            await center.refresh()
            if live { await Services.get(ShortcutsStore.self)?.loadRun() }
        }
    }

    private func refreshWorkIfStale() {
        guard signedIn, workTask == nil, now - lastWorkRefreshMs > SurfacesCoordinator.workRefreshAfterMs else { return }
        lastWorkRefreshMs = now
        let api: ApiService = services.api
        let account: String = services.auth.session?.nodeName ?? ""
        let startedScope: String = scope
        workTask = Task { [weak self] in
            let configured: [WidgetInfo] = await SurfacesCoordinator.configuredWidgets()
            let wanted: Bool = configured.contains { $0.kind == SurfaceSection.overview.widgetKind }
            let counts: WorkSurface? = wanted ? await SurfacesCoordinator.loadWork(api: api, account: account) : nil
            guard let self, !Task.isCancelled else { return }
            self.workTask = nil
            if !wanted { self.lastWorkRefreshMs = 0 }
            guard let counts, self.scope == startedScope else { return }
            self.work = counts
            self.schedulePublish()
        }
    }

    static func loadWork(api: ApiService, account: String) async -> WorkSurface {
        let repos: [Repository]? = try? await api.repositories()
        let refs: [RepoRef] = repos.map { WorkSurface.repos($0, account: account) } ?? []
        var issueLists: [[Issue]?] = repos == nil ? [nil] : []
        var pullLists: [[PullRequest]?] = repos == nil ? [nil] : []
        for ref in refs {
            if Task.isCancelled { break }
            async let issues: [Issue]? = try? await api.publishedIssues(ref.owner, ref.name)
            async let pulls: [PublishedPull]? = try? await api.publishedPullHeads(ref.owner, ref.name)
            issueLists.append(await issues)
            pullLists.append(await pulls?.map { PullRequest(number: $0.number, title: $0.title, status: $0.status) })
        }
        var openTasks: Int?
        var moreTasks = false
        var offset: Int? = 0
        for _ in 0..<WorkSurface.maxTaskPages {
            guard let current = offset, !Task.isCancelled else { break }
            guard let data = try? await api.tasksApi("GET", current == 0 ? "" : "?offset=\(current)") else { break }
            let page: WorkSurface.TaskPage = WorkSurface.taskPage(data)
            openTasks = (openTasks ?? 0) + page.open
            offset = page.nextOffset
        }
        if offset != nil && openTasks != nil { moreTasks = true }
        return WorkSurface(
            openIssues: WorkSurface.openIssues(issueLists), openPulls: WorkSurface.openPulls(pullLists),
            openTasks: openTasks, moreTasks: moreTasks, updatedAtMs: SystemClock().nowMillis)
    }

    private func pickUpShare() async {
        guard !pickingUpShare, services.auth.hasAppAccess else { return }
        pickingUpShare = true
        defer { pickingUpShare = false }
        guard let drop = ShareKeychainDrop().take() else { return }
        let videos: [String] = drop.images.isEmpty
            ? DjStudio.sharedVideoIds(urls: drop.handoff.urls, text: drop.handoff.text) : []
        if !videos.isEmpty, PackageGate.allows("video-mix-dj-studio") {
            let studio: DjStudioModel = DjStudioHub.shared.studio()
            for id in videos {
                studio.enqueue(id: id, title: "", author: "")
            }
            SurfaceRouter.open(.videoMixDjStudio)
            return
        }
        var attachments: [PromptAttachment] = []
        for (index, data) in drop.images.enumerated() {
            if let attachment = try? await PromptPhotoPreparer.prepare(data, name: "shared-\(index + 1)") {
                attachments.append(attachment)
            }
        }
        if !attachments.isEmpty, let composer = Services.get(ShellComposer.self) {
            composer.attachments = attachments
        }
        SurfaceRouter.open(drop.handoff.route)
    }

    func handle(_ command: SurfaceCommand) async throws {
        switch command {
        case let .open(route):
            SurfaceRouter.open(route)
        case let .stopAgent(runId):
            try await stopAgent(runId)
        case let .approve(approvalId):
            try await approve(approvalId)
        case let .startTimer(seconds, label):
            try startTimer(seconds: seconds, label: label)
        }
    }

    private func stopAgent(_ runId: String) async throws {
        guard signedIn else { throw SurfaceCommandError("Sign in to ForkMesh first.") }
        guard let center = HostsFeature.agents else { throw SurfaceCommandError.appNotReady }
        let id: String = AgentRouteDestination.rowId(runId)
        if center.rows.first(where: { $0.id == id }) == nil {
            await center.refresh()
        }
        guard let row = center.rows.first(where: { $0.id == id }) else {
            throw SurfaceCommandError("That run is no longer listed.")
        }
        guard row.isActive else { return }
        try await center.stop(row)
        schedulePublish()
    }

    private func approve(_ approvalId: String) async throws {
        guard signedIn else { throw SurfaceCommandError("Sign in to ForkMesh first.") }
        guard let queue = AgentOpsFeature.approvals else { throw SurfaceCommandError.appNotReady }
        if queue.approval(id: approvalId) == nil {
            await queue.refresh()
        }
        guard queue.approval(id: approvalId) != nil else {
            throw SurfaceCommandError("That request was already answered.")
        }
        let accepted: Bool = await queue.approve(approvalId)
        if !accepted {
            throw SurfaceCommandError(queue.errorText.isEmpty ? "The desktop did not take the answer." : queue.errorText)
        }
        schedulePublish()
    }

    private func startTimer(seconds: Int, label: String) throws {
        guard let runtime = PlanningTimersRuntime.shared() else { throw SurfaceCommandError.appNotReady }
        do {
            try runtime.store.startCountdown(label: label, seconds: seconds)
        } catch let error as PlanningTimerStore.StoreError {
            throw SurfaceCommandError(error.description)
        }
        runtime.requestPermission()
    }
}
