// The iPhone side of the watch app (docs/native/PARITY.md, 4.2 option A).
//
// The phone stays the only ForkMesh client: it reads the account's agent
// sessions (owner-sealed desktop snapshots and the relay queue) and, when a
// desktop is on this network, the local control channel; unseals the running
// sessions' transcripts; turns them into compact `AgentFrame`s; and hands them
// to the watch over WatchConnectivity:
//
//   - `updateApplicationContext` with the latest list, always;
//   - `sendMessage` as well while the watch app is in front (reachable);
//   - `transferCurrentComplicationUserInfo` when what a complication shows
//     changes (the budget is 50 a day).
//
// Commands come back as messages (`WatchCommand`); iOS wakes this app in the
// background to take them, and the reply says what happened.
//
// Nothing here polls. Work happens on the account's `agents` doorbell (the
// list model's ring subscription), on a local-network change, on a command,
// and only while a paired watch has the app installed.

import FMAccount
import FMAgentWatch
import FMAgents
import FMApi
import FMBase
import FMChat
import FMWatchWire
import Foundation
import Observation
import WatchConnectivity

@MainActor
final class WatchBridge: NSObject {
    private let api: ApiService
    private let auth: AuthService
    private let control: LocalControlService?
    private let model: AgentsListModel
    private let session: WCSession?

    /// What each published key refers to, for carrying out commands.
    private var targets: [String: Target] = [:]
    /// Transcript beats per session key, and how long the transcript was
    /// when read (a transcript that has not grown is not parsed again).
    private let beatCache = TranscriptBeatCache()
    /// The agent whose live view is open on the watch, if any.
    private var watched: String?
    private var listening = false
    private var started = false
    private var lastSummary: ComplicationSummary?
    private var publishTask: Task<Void, Never>?
    private var controlToken: ListenerToken?
    private var authToken: ListenerToken?

    /// At most this many running sessions have their transcripts read per
    /// publish (the watched one always is).
    private static let transcriptReads = 3
    /// At most this many rows go to the watch.
    private static let maxFrames = 24

    init(api: ApiService, auth: AuthService, signal: AgentListSignal, control: LocalControlService?) {
        self.api = api
        self.auth = auth
        self.control = control
        self.session = WCSession.isSupported() ? WCSession.default : nil
        self.model = AgentsListModel(
            api: api,
            signal: signal,
            ownerAccountName: { auth.session?.nodeName ?? "" },
            hasDesktopNode: {
                guard let session = auth.session else { return true }
                return !session.desktopDevices.isEmpty
            },
            refusals: AgentReadRefusals.shared
        )
        super.init()
        // Nothing is read until a watch with the app turns up.
        model.enabled = false
    }

    /// Activates the WatchConnectivity session. Idempotent.
    func start() {
        guard !started, let session else { return }
        started = true
        session.delegate = self
        session.activate()
        observeModel()
        controlToken = control?.addListener { [weak self] in
            MainActor.assumeIsolated { self?.schedulePublish() }
        }
        authToken = auth.addListener { [weak self] in
            MainActor.assumeIsolated { self?.authChanged() }
        }
    }

    // MARK: When to work

    /// A paired watch with the app installed, on an activated session.
    private var watchReady: Bool {
        guard let session, session.activationState == .activated else { return false }
        return session.isPaired && session.isWatchAppInstalled
    }

    /// The list model runs (and rings are answered) only while a watch can
    /// use what it reads.
    private func watchStateChanged() {
        let ready = watchReady
        if ready != model.enabled {
            model.enabled = ready
        }
        if ready {
            model.start()
            schedulePublish()
        } else {
            model.stop()
        }
    }

    private func authChanged() {
        beatCache.removeAll()
        if watchReady {
            model.reload()
            schedulePublish()
        }
    }

    /// Re-armed after every change: `AgentsListModel` is @Observable and has
    /// no listeners of its own.
    private func observeModel() {
        withObservationTracking {
            _ = model.groups
            _ = model.loadError
        } onChange: { [weak self] in
            Task { @MainActor [weak self] in
                self?.schedulePublish()
                self?.observeModel()
            }
        }
    }

    /// A burst of changes (a ring, then the model's load, then the LAN list)
    /// is one publish.
    private func schedulePublish(delay: TimeInterval = 0.4) {
        guard watchReady else { return }
        publishTask?.cancel()
        publishTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard !Task.isCancelled else { return }
            await self?.publish()
        }
    }

    // MARK: Building frames

    private enum Target {
        case desktop(Repository, AgentSession, lan: LocalAgentRow?, launch: AgentLaunchSession?)
        case queued(Repository, AgentLaunchSession)
        case lan(LocalAgentRow)
    }

    private func publish() async {
        guard watchReady, let session else { return }
        let snapshot = await buildSnapshot(reachable: session.isReachable)
        let fitted = PerformanceMonitorService.section("watch.fit") { WatchWire.fitted(snapshot, keep: watched) }
        guard let envelope = try? WatchWire.envelope(.snapshot, fitted) else { return }
        do {
            try PerformanceMonitorService.section("watch.context") { try session.updateApplicationContext(envelope) }
        } catch {
            Log.warning("watch", "Application context refused: \(error)")
        }
        if session.isReachable {
            session.sendMessage(envelope, replyHandler: nil) { (error: Error) in
                Log.debug("watch", "Snapshot message not delivered: \(error)")
            }
        }
        updateComplication(fitted.summary)
    }

    private func updateComplication(_ summary: ComplicationSummary) {
        guard let session else { return }
        var comparable = summary
        comparable.generatedAtMs = lastSummary?.generatedAtMs ?? 0
        if comparable == lastSummary { return }
        lastSummary = summary
        guard let envelope = try? WatchWire.envelope(.summary, summary) else { return }
        if session.isComplicationEnabled && session.remainingComplicationUserInfoTransfers > 0 {
            session.transferCurrentComplicationUserInfo(envelope)
        } else {
            // No complication on the face (or out of budget): an ordinary
            // user-info transfer still refreshes the Smart Stack widget.
            session.transferUserInfo(envelope)
        }
    }

    private func buildSnapshot(reachable: Bool) async -> WatchSnapshot {
        let now = Int64(Date().timeIntervalSince1970 * 1000)
        guard auth.session != nil else {
            targets = [:]
            return WatchSnapshot(generatedAtMs: now, signedIn: false, note: "Sign in to ForkMesh on your iPhone.")
        }
        var frames: [AgentFrame] = []
        var next: [String: Target] = [:]
        let lanRows = (control?.connected == true) ? (control?.agents?.sessions ?? []) : []
        var lanUsed = Set<Int>()

        for group in model.groups {
            let repo = group.repo
            for item in group.sessions {
                let lan = lanRows.first { row in row.session.id == item.id && WatchBridge.sameRepo(row, repo) }
                if let lan { lanUsed.insert(lan.id) }
                let launch = group.launched.first { $0.localAgentId == item.id }
                let key = "desktop:\(repo.fullName):\(item.id)"
                next[key] = .desktop(repo, item, lan: lan, launch: launch)
                frames.append(frame(key: key, repo: repo.fullName, session: item, lan: lan, launch: launch))
            }
            for launched in group.launched {
                // A claimed run already shows as its desktop session.
                if launched.localAgentId > 0 && group.sessions.contains(where: { $0.id == launched.localAgentId }) {
                    continue
                }
                let key = "queued:\(repo.fullName):\(launched.sessionId)"
                next[key] = .queued(repo, launched)
                frames.append(frame(key: key, repo: repo.fullName, launched: launched))
            }
        }
        for row in lanRows where !lanUsed.contains(row.id) {
            let key = "lan:\(row.repoLabel):\(row.id)"
            next[key] = .lan(row)
            frames.append(frame(key: key, row: row))
        }
        targets = next
        frames = Array(AgentFrame.ordered(frames).prefix(WatchBridge.maxFrames))

        // Transcripts: the watched agent always; the other running ones only
        // while the watch app is open to show them.
        var reads = frames.filter { $0.key == watched }.map(\.key)
        if reachable {
            reads += frames.filter { $0.status.isActive && $0.key != watched && $0.source != .queued }
                .prefix(WatchBridge.transcriptReads).map(\.key)
        }
        for key in reads {
            guard let target = next[key], let beats = await readBeats(key: key, target: target) else { continue }
            if let index = frames.firstIndex(where: { $0.key == key }) {
                frames[index].beatCount = beats.count
                frames[index].beats = Array(beats.suffix(AgentFrame.maxBeats))
                frames[index].lastReply = beats.last { !$0.said.isEmpty }?.said ?? ""
            }
        }
        // Frames not read this time keep the beats read last time.
        for index in frames.indices where !reads.contains(frames[index].key) {
            if let cached = beatCache.cached(frames[index].key) {
                frames[index].beatCount = cached.count
                frames[index].beats = Array(cached.suffix(AgentFrame.maxBeats))
                frames[index].lastReply = cached.last { !$0.said.isEmpty }?.said ?? ""
            }
        }
        let note: String
        if let error = model.loadError, frames.isEmpty {
            note = error
        } else if frames.isEmpty {
            note = model.hasDesktopNode ? "No agent work yet." : "Agents run on a desktop. Enroll one to start."
        } else {
            note = ""
        }
        return WatchSnapshot(generatedAtMs: now, signedIn: true, note: note, frames: frames)
    }

    private func readBeats(key: String, target: Target) async -> [WireBeat]? {
        let text: String
        do {
            switch target {
            case .desktop(let repo, let item, let lan, _):
                if let lan, let control, control.connected {
                    text = try await control.transcript(lan.id)
                } else {
                    let detail = try await api.agentTranscript(
                        repo.owner, repo.name, item.id, ownerAccount: model.ownerAccount(for: repo))
                    if detail.sealedForOtherDevice { return nil }
                    text = detail.transcript
                }
            case .lan(let row):
                guard let control, control.connected else { return nil }
                text = try await control.transcript(row.id)
            case .queued:
                return nil
            }
        } catch {
            Log.debug("watch", "Transcript read failed for \(key): \(error)")
            return beatCache.cached(key)
        }
        return await beatCache.beats(key, transcript: text)
    }

    private static func sameRepo(_ row: LocalAgentRow, _ repo: Repository) -> Bool {
        let name = row.name.lowercased()
        let owner = row.owner.lowercased()
        guard name == repo.name.lowercased() else { return false }
        return owner.isEmpty || owner == repo.owner.lowercased() || owner == repo.displayOwner.lowercased()
    }

    private func frame(
        key: String, repo: String, session item: AgentSession, lan: LocalAgentRow?, launch: AgentLaunchSession?
    ) -> AgentFrame {
        var controls: WireControls = []
        if let lan, control?.connected == true {
            if lan.stoppable { controls.insert(.stop) }
            if lan.canContinue { controls.insert(.continue) }
            controls.insert(.prompt)
        } else if let launch {
            if launch.isPromptable && item.isActive { controls.insert(.prompt) }
            if launch.isRetryable { controls.insert(.retry) }
        }
        if diffReachable { controls.insert(.diff) }
        let status = WireStatus.from(item.status, merged: item.merged)
        return AgentFrame(
            key: key, title: lan?.title ?? item.displayTitle, repo: repo, status: status,
            provider: item.provider, model: item.model, turns: item.numTurns, tokens: item.totalTokens,
            contextTokens: item.contextTokens, contextWindow: item.contextWindow,
            startedAtMs: Int64(item.startedAtMs),
            updatedAtMs: Int64(max(item.finishedAtMs, item.startedAtMs, item.createdAtMs)),
            source: lan != nil ? .lan : .desktop, controls: controls)
    }

    private func frame(key: String, repo: String, launched: AgentLaunchSession) -> AgentFrame {
        var controls: WireControls = []
        if launched.isActive && launched.localAgentId == 0 { controls.insert(.stop) }
        if launched.isPromptable && launched.isActive { controls.insert(.prompt) }
        if launched.isRetryable { controls.insert(.retry) }
        if !launched.branchName.isEmpty { controls.insert(.diff) }
        var status: WireStatus = launched.isStopped ? .stopped : WireStatus.from(launched.status, merged: launched.merged)
        // "security-pending" and the like: waiting for a node.
        if status == .unknown && launched.isActive { status = .queued }
        let reply = launched.history.last { !$0.isUser }?.text ?? ""
        return AgentFrame(
            key: key, title: launched.displayTitle, repo: repo, status: status,
            provider: launched.provider, model: launched.requestedModel,
            startedAtMs: Int64(launched.createdAtMs),
            updatedAtMs: Int64(max(launched.updatedAtMs, launched.completedAtMs, launched.createdAtMs)),
            source: .queued, controls: controls, lastReply: plainProse(reply))
    }

    private func frame(key: String, row: LocalAgentRow) -> AgentFrame {
        var controls: WireControls = [.prompt]
        if row.stoppable { controls.insert(.stop) }
        if row.canContinue { controls.insert(.continue) }
        if diffReachable { controls.insert(.diff) }
        let item = row.session
        return AgentFrame(
            key: key, title: row.title, repo: row.repoLabel, status: WireStatus.from(item.status, merged: item.merged),
            provider: item.provider, model: item.model, turns: item.numTurns, tokens: item.totalTokens,
            contextTokens: item.contextTokens, contextWindow: item.contextWindow,
            startedAtMs: Int64(item.startedAtMs),
            updatedAtMs: Int64(max(item.finishedAtMs, item.startedAtMs, item.createdAtMs)),
            source: .lan, controls: controls)
    }

    // MARK: Commands

    fileprivate func handle(_ command: WatchCommand) async -> WatchReply {
        switch command {
        case .refresh:
            if !watchReady { return .refused("The watch app is not set up on this iPhone yet.") }
            if let control, control.connected { await control.refreshAgents() }
            await model.refresh()
            await publish()
            return .done()
        case .watch(let key):
            watched = key
            schedulePublish(delay: 0.05)
            return .done()
        case .listen(let on):
            listening = on
            if on { schedulePublish(delay: 0.05) }
            return .done()
        case .stop(let key):
            return await run(key) { target in try await self.stop(target) }
        case .continue(let key):
            return await run(key) { target in try await self.resume(target) }
        case .retry(let key):
            return await run(key) { target in try await self.retry(target) }
        case .prompt(let key, let text):
            if let problem = WatchCommand.promptProblem(text) { return .refused(problem) }
            let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
            return await run(key) { target in try await self.prompt(target, trimmed) }
        case .diff:
            return .done()
        case .start(let text, let repo):
            if let problem = WatchCommand.promptProblem(text) { return .refused(problem) }
            return await start(text.trimmingCharacters(in: .whitespacesAndNewlines), repository: repo)
        }
    }

    private func start(_ text: String, repository: String?) async -> WatchReply {
        guard auth.session != nil else { return .refused("Sign in to ForkMesh on your iPhone.") }
        guard let composer = Services.get(ShellComposer.self) else {
            return .refused("Open ForkMesh on your iPhone once, then try again.")
        }
        if composer.busy { return .refused("Your iPhone is starting another agent. Try again in a moment.") }
        composer.busy = true
        let outcome = await composer.launcher.send(prompt: text, repository: repository, onDevice: false)
        composer.busy = false
        let line = GenieFeature.outcomeLine(outcome)
        switch outcome {
        case .queued, .queuedWithoutSession, .desktop, .done:
            if let control, control.connected { await control.refreshAgents() }
            await model.refresh()
            await publish()
            return .done(line)
        case .device, .ignored, .needsFix, .failed:
            return .refused(line)
        }
    }

    private var diffReachable: Bool { HostsFeature.hub != nil }

    fileprivate func diff(_ key: String) async -> WatchDiff {
        guard let target = targets[key] else {
            return .refused(key, "That agent is no longer in the list. Pull down to refresh.")
        }
        switch target {
        case .desktop(_, let item, _, _):
            return await desktopDiff(key, sessionId: item.id)
        case .lan(let row):
            return await desktopDiff(key, sessionId: row.session.id)
        case .queued(let repo, let launched):
            guard !launched.branchName.isEmpty else { return .refused(key, "This run has no branch yet.") }
            do {
                let page = try await api.runnerAgentDiff(
                    repo.owner, repo.name, launched.sessionId, page: 0, pageSize: 100, orgScope: launched.orgScope)
                guard let built = RunnerAgentOps.diff(from: [page]) else { return .refused(key, "No changes yet.") }
                return AgentWatchDiff.make(key: key, diff: built)
            } catch {
                return .refused(key, RunnerAgentOps.describe(error))
            }
        }
    }

    private func desktopDiff(_ key: String, sessionId: Int) async -> WatchDiff {
        guard sessionId > 0, let client = AgentOpsFeature.client else {
            return .refused(key, "Pair this iPhone with your desktop to see an agent's changes.")
        }
        do {
            return AgentWatchDiff.make(key: key, diff: try await client.diff(sessionId: sessionId))
        } catch {
            return .refused(
                key,
                AgentOpsClient.isUnsupported(error)
                    ? "Update ForkMesh on the desktop to see an agent's changes here." : AgentOpsClient.describe(error))
        }
    }

    private func run(_ key: String, _ action: (Target) async throws -> WatchReply) async -> WatchReply {
        guard let target = targets[key] else {
            return .refused("That agent is no longer in the list. Pull down to refresh.")
        }
        let reply: WatchReply
        do {
            reply = try await action(target)
        } catch let error as AgentLaunchException {
            reply = .refused(error.message)
        } catch {
            reply = .refused(dartErrorText(error))
        }
        if reply.ok {
            if let control, control.connected { await control.refreshAgents() }
            await model.refresh()
            await publish()
        }
        return reply
    }

    private var lanControl: LocalControlService? {
        guard let control, control.connected else { return nil }
        return control
    }

    private static let noLan = "Only a desktop on this network can do that. Open ForkMesh on the desktop's Wi-Fi."

    private func stop(_ target: Target) async throws -> WatchReply {
        switch target {
        case .desktop(_, _, let lan, _):
            guard let lan, lan.stoppable, let control = lanControl else { return .refused(WatchBridge.noLan) }
            try await control.stopAgent(lan.id)
            return .done("Stopping.")
        case .lan(let row):
            guard row.stoppable, let control = lanControl else { return .refused(WatchBridge.noLan) }
            try await control.stopAgent(row.id)
            return .done("Stopping.")
        case .queued(let repo, let launched):
            guard launched.isActive, launched.localAgentId == 0 else {
                return .refused("A node already took this run; stop it from its desktop.")
            }
            try await api.cancelAgentSession(repo.owner, repo.name, launched.sessionId, orgScope: launched.orgScope)
            return .done("Cancelled.")
        }
    }

    private func resume(_ target: Target) async throws -> WatchReply {
        switch target {
        case .desktop(let repo, _, let lan, let launch):
            if let lan, lan.canContinue, let control = lanControl {
                try await control.continueAgent(lan.id)
                return .done("Continuing.")
            }
            if let launch, launch.isRetryable {
                return try await retry(.queued(repo, launch))
            }
            return .refused(WatchBridge.noLan)
        case .lan(let row):
            guard row.canContinue, let control = lanControl else { return .refused(WatchBridge.noLan) }
            try await control.continueAgent(row.id)
            return .done("Continuing.")
        case .queued:
            return try await retry(target)
        }
    }

    private func retry(_ target: Target) async throws -> WatchReply {
        let repo: Repository
        let launched: AgentLaunchSession
        switch target {
        case .queued(let r, let l):
            repo = r
            launched = l
        case .desktop(let r, _, _, let l?):
            repo = r
            launched = l
        default:
            return try await resume(target)
        }
        guard launched.isRetryable else { return .refused("This run cannot be run again from here.") }
        let outcome = try await api.retryAgentSession(repo.owner, repo.name, launched)
        return .done(outcome.label)
    }

    private func prompt(_ target: Target, _ text: String) async throws -> WatchReply {
        switch target {
        case .desktop(let repo, _, let lan, let launch):
            if let lan, let control = lanControl {
                try await control.sendPrompt(lan.id, text)
                return .done("Sent.")
            }
            guard let launch, launch.isPromptable else { return .refused(WatchBridge.noLan) }
            _ = try await api.steerAgentSession(
                repo.owner, repo.name, launch.sessionId, prompt: text, orgScope: launch.orgScope)
            return .done("Sent.")
        case .lan(let row):
            guard let control = lanControl else { return .refused(WatchBridge.noLan) }
            try await control.sendPrompt(row.id, text)
            return .done("Sent.")
        case .queued(let repo, let launched):
            guard launched.isPromptable else {
                return .refused("No node has picked this run up yet, so it cannot take a follow-up.")
            }
            _ = try await api.steerAgentSession(
                repo.owner, repo.name, launched.sessionId, prompt: text, orgScope: launched.orgScope)
            return .done("Sent.")
        }
    }
}

// MARK: - WCSessionDelegate

/// WCSession's reply handler, carried onto the main actor.
private struct ReplySink: @unchecked Sendable {
    let reply: ([String: Any]) -> Void
}

extension WatchBridge: WCSessionDelegate {
    nonisolated func session(
        _ session: WCSession, activationDidCompleteWith activationState: WCSessionActivationState, error: Error?
    ) {
        if let error {
            Log.warning("watch", "WatchConnectivity activation failed: \(error)")
        }
        Task { @MainActor [weak self] in self?.watchStateChanged() }
    }

    nonisolated func sessionDidBecomeInactive(_ session: WCSession) {}

    /// A watch switch: activate again for the new watch.
    nonisolated func sessionDidDeactivate(_ session: WCSession) {
        session.activate()
    }

    nonisolated func sessionWatchStateDidChange(_ session: WCSession) {
        Task { @MainActor [weak self] in self?.watchStateChanged() }
    }

    nonisolated func sessionReachabilityDidChange(_ session: WCSession) {
        if session.isReachable {
            Task { @MainActor [weak self] in self?.schedulePublish(delay: 0.05) }
        }
    }

    nonisolated func session(
        _ session: WCSession, didReceiveMessage message: [String: Any], replyHandler: @escaping ([String: Any]) -> Void
    ) {
        let sink = ReplySink(reply: replyHandler)
        guard let command = try? WatchWire.open(.command, message, as: WatchCommand.self) else {
            let refusal = WatchReply.refused("Update ForkMesh on your iPhone to use this.")
            sink.reply((try? WatchWire.envelope(.reply, refusal)) ?? [:])
            return
        }
        if case .diff(let key) = command {
            Task { @MainActor [weak self] in
                let diff = await self?.diff(key) ?? .refused(key, "ForkMesh is not running on your iPhone.")
                sink.reply((try? WatchWire.envelope(.diff, diff)) ?? [:])
            }
            return
        }
        Task { @MainActor [weak self] in
            let reply = await self?.handle(command) ?? .refused("ForkMesh is not running on your iPhone.")
            sink.reply((try? WatchWire.envelope(.reply, reply)) ?? [:])
        }
    }

    nonisolated func session(_ session: WCSession, didReceiveMessage message: [String: Any]) {
        guard let command = try? WatchWire.open(.command, message, as: WatchCommand.self) else { return }
        Task { @MainActor [weak self] in _ = await self?.handle(command) }
    }

    nonisolated func session(_ session: WCSession, didReceiveUserInfo userInfo: [String: Any] = [:]) {
        guard let command = try? WatchWire.open(.command, userInfo, as: WatchCommand.self) else { return }
        Task { @MainActor [weak self] in _ = await self?.handle(command) }
    }
}
