// "Just send it to the agent": the composer starts a run the moment Send is
// tapped, with no second form. Where it runs is decided here from what the
// person did last:
//
//   repository  the current context, else the last one used, else the
//               account's own most recently updated one, else one already on
//               this device, else the most recently updated
//   target      the last one used for that repository, else the first online
//               node that runs the chip's provider, else this device, else
//               the preferred node (queued until it connects)
//   model       the chip (Opus 5.5 unless changed)
//
// The composer shows the decision as one tappable line ("owner/repo ·
// mirror35") that opens a picker sheet; changing it is remembered per
// repository and never blocks a send. Only when nothing can take the run does
// a send come back with a one-tap fix, and the typed text stays.
//
// The repository list, the relay probe and the queued launch are
// AgentStartModel's; this is the policy on top of it.

import FMApi
import FMBase
import FMChat
import FMCode
import FMControl
import FMLocalAI
import Foundation
import Observation

/// Where a quick send runs.
public enum QuickLaunchDestination: Equatable, Hashable, Sendable {
    /// Queued for this node (a mirror or a desktop).
    case node(String)
    /// On this phone, against its mirror of the repository.
    case device
    case desktop(String)

    var stored: String {
        switch self {
        case .node(let name): return "node:\(name)"
        case .device: return "device"
        case .desktop(let name): return "desktop:\(name)"
        }
    }

    init?(stored: String) {
        if stored == "device" {
            self = .device
        } else if stored.hasPrefix("desktop:") {
            self = .desktop(String(stored.dropFirst(8)))
        } else if stored.hasPrefix("node:"), stored.count > 5 {
            self = .node(String(stored.dropFirst(5)))
        } else {
            return nil
        }
    }
}

/// The choices a quick send remembers: the last repository, and per
/// repository the last place it ran.
public struct QuickLaunchMemory {
    public static let lastRepoKey = "agent/quick/last-repo"
    public static func destinationKey(_ repoKey: String) -> String { "agent/quick/target/\(repoKey.lowercased())" }

    public let store: KeyValueStore?

    public init(store: KeyValueStore?) { self.store = store }

    public var lastRepo: String { store?.string(forKey: Self.lastRepoKey) ?? "" }

    public func destination(for repoKey: String) -> QuickLaunchDestination? {
        guard let raw = store?.string(forKey: Self.destinationKey(repoKey)) else { return nil }
        return QuickLaunchDestination(stored: raw)
    }

    public func remember(repoKey: String) {
        if !repoKey.isEmpty { store?.set(repoKey.lowercased(), forKey: Self.lastRepoKey) }
    }

    public func remember(repoKey: String, destination: QuickLaunchDestination) {
        if repoKey.isEmpty { return }
        store?.set(destination.stored, forKey: Self.destinationKey(repoKey))
    }
}

/// The one thing standing between a send and a run, with its one-tap fix.
public enum QuickLaunchFix: Equatable, Sendable {
    /// Signed out: the queue answers accounts only.
    case signInAccount
    /// No repository to point the agent at.
    case chooseRepository
    /// The only node has no signed-in CLI for the model's provider.
    case signInProvider(node: String, provider: String)
    case signInRunner(runner: String, provider: String)
    /// A run here needs a provider (an API key, or Gemma on this iPhone).
    case setUpDevice
    case setUpGemma
    /// A run here needs the repository mirrored to this device first.
    case mirrorToDevice(owner: String, name: String)

    /// The button's words.
    public var action: String {
        switch self {
        case .signInAccount: return "Sign in"
        case .chooseRepository: return "Choose a repository"
        case .signInProvider(let node, let provider): return "Sign in to \(agentProviderLabel(provider)) on \(node)"
        case .signInRunner(let runner, let provider): return "Sign in to \(agentProviderLabel(provider)) on \(runner)"
        case .setUpDevice: return "Set up the agent on this iPhone"
        case .setUpGemma: return "Set up Gemma"
        case .mirrorToDevice: return "Mirror to this iPhone"
        }
    }

    public static func signIn(_ target: AgentLaunchTarget, provider: String) -> QuickLaunchFix {
        target.isRunner
            ? .signInRunner(runner: target.signInNode, provider: provider)
            : .signInProvider(node: target.node, provider: provider)
    }
}

/// What a quick send did.
public enum QuickLaunchOutcome: Equatable, Sendable {
    /// Nothing to send.
    case ignored
    /// Queued on the relay: open this session's transcript.
    case queued(repo: Repository, session: AgentLaunchSession?, message: String)
    /// Queued, but the relay named no session (an older relay): say so.
    case queuedWithoutSession(message: String)
    /// Runs on this phone: open the repository's agent with the prompt.
    case device(record: LocalRepoRecord, prompt: String, images: [PromptAttachment], credentials: AgentCredentials?)
    case desktop(name: String, agentId: Int, message: String)
    /// Not sent; the text stays and this fix is offered.
    case needsFix(QuickLaunchFix, message: String)
    /// Not sent; the relay or the network said why.
    case failed(String)
    case done(String)
}

@MainActor
public protocol QuickLaunchDesktop: AnyObject {
    var desktopName: String { get }
    var desktopDetail: String { get }
    func start(
        owner: String, name: String, task: String, provider: String, model: String, mode: String,
        options: QuickLaunchRunOptions, images: [PromptAttachment]
    ) async throws -> Int
}

public struct QuickLaunchRunOptions: Equatable, Sendable {
    public static let yoloKey = "agent/desktop/yolo"

    public var yolo: Bool
    public var worktree: Bool

    public init(yolo: Bool = false, worktree: Bool = true) {
        self.yolo = yolo
        self.worktree = worktree
    }

    public static func read(_ store: KeyValueStore?) -> QuickLaunchRunOptions {
        QuickLaunchRunOptions(yolo: store?.bool(forKey: yoloKey) ?? false)
    }
}

@MainActor
public final class HubQuickLaunchDesktop: QuickLaunchDesktop {
    private let hub: @MainActor () -> DesktopControlHub?

    public init(hub: @escaping @MainActor () -> DesktopControlHub?) { self.hub = hub }

    public var desktopName: String {
        guard let desktop = hub()?.desktop else { return "" }
        return desktop.name.isEmpty ? "Desktop" : desktop.name
    }

    public var desktopDetail: String {
        guard let reach = hub()?.reach else { return "" }
        return reach.isConnected ? reach.label : "connects when you send"
    }

    public func start(
        owner: String, name: String, task: String, provider: String, model: String, mode: String,
        options: QuickLaunchRunOptions, images: [PromptAttachment]
    ) async throws -> Int {
        guard let hub = hub() else { throw LocalControlException("not_connected") }
        var params: JSONObject = ["owner": .string(owner), "name": .string(name), "task": .string(task)]
        if !provider.isEmpty { params["provider"] = .string(provider) }
        if !model.isEmpty { params["model"] = .string(model) }
        if !mode.isEmpty { params["mode"] = .string(mode) }
        if options.yolo { params["vibe"] = .bool(true) }
        if !options.worktree { params["worktree"] = .bool(false) }
        let link = try await hub.link()
        params = try await DesktopPromptImages.params(params, images: images, over: link)
        return try await link.request("agents.start", params, timeout: 30).int("id")
    }
}

@MainActor
@Observable
public final class QuickAgentLauncher {
    /// The repository list, the relay probe and the queued launch.
    public let form: AgentStartModel
    @ObservationIgnored private let memory: QuickLaunchMemory
    @ObservationIgnored private let prefs: KeyValueStore?
    @ObservationIgnored private let account: () -> String
    @ObservationIgnored public var attachmentStore: AgentAttachmentStore = .shared
    @ObservationIgnored public var sentPhotos: AgentSentPhotos = .shared
    @ObservationIgnored public var desktop: QuickLaunchDesktop?
    @ObservationIgnored public var actions: QuickPromptActions?

    /// Where the next send goes; nil until known, or when nowhere can take it.
    public private(set) var destination: QuickLaunchDestination?
    /// The chip's model.
    public private(set) var model: AgentModelChoice
    public private(set) var mode: AgentPermissionMode
    public private(set) var runOptions: QuickLaunchRunOptions
    /// Loading the repositories or asking the relay about one.
    public private(set) var preparing = false
    @ObservationIgnored private var prepared = false
    public private(set) var chosenAction: QuickPromptAction?
    public private(set) var actionChosen: Bool
    public private(set) var defaultsToBugReport = false
    public private(set) var taskPeople: QuickTaskPeople?
    public private(set) var taskAssignee: String
    public private(set) var loadingTaskPeople = false

    public init(form: AgentStartModel, prefs: KeyValueStore?, account: @escaping () -> String = { "" }) {
        self.form = form
        self.prefs = prefs
        self.account = account
        self.memory = QuickLaunchMemory(store: prefs)
        self.model = NodeModelPreference.read(prefs)
        self.mode = AgentPermissionMode.read(prefs)
        self.runOptions = QuickLaunchRunOptions.read(prefs)
        let storedAction = prefs?.string(forKey: QuickPromptAction.storeKey)
        self.actionChosen = storedAction != nil
        self.chosenAction = storedAction.flatMap { QuickPromptAction(rawValue: $0) }
        self.taskAssignee = prefs?.string(forKey: QuickPromptAction.assigneeKey) ?? ""
    }

    public var action: QuickPromptAction? {
        guard actions != nil else { return nil }
        if actionChosen { return chosenAction }
        return defaultsToBugReport ? .bugReport : nil
    }

    public var chip: AgentModelChoice { action?.choice ?? model }

    public var hasAgent: Bool {
        if !form.signedIn { return false }
        switch destination {
        case .none: return false
        case .some(.device):
            return model.runsOnDevice ? form.onDeviceGemmaReadiness.isReady : form.deviceCredentials != nil
        case .some(.node), .some(.desktop): return true
        }
    }

    func settleDefault() {
        if form.plan?.loading ?? false { return }
        defaultsToBugReport = !hasAgent
    }

    public func choose(action: QuickPromptAction) {
        rememberAction(action)
        if action == .task && taskPeople == nil {
            Task { await loadTaskPeople() }
        }
    }

    private func rememberAction(_ action: QuickPromptAction?) {
        chosenAction = action
        actionChosen = true
        prefs?.set(action?.rawValue ?? "", forKey: QuickPromptAction.storeKey)
    }

    public func loadTaskPeople() async {
        guard let actions, !loadingTaskPeople else { return }
        loadingTaskPeople = true
        defer { loadingTaskPeople = false }
        if let people = try? await actions.taskPeople() {
            taskPeople = people
        }
    }

    public var taskAssignees: [String] { [""] + (taskPeople?.names ?? (taskAssignee.isEmpty ? [] : [taskAssignee])) }

    public func taskAssigneeLabel(_ name: String) -> String {
        if name.isEmpty { return "Unassigned" }
        return name == taskPeople?.actor ? "@\(name) (you)" : "@\(name)"
    }

    public func choose(assignee: String) {
        taskAssignee = assignee
        prefs?.set(assignee, forKey: QuickPromptAction.assigneeKey)
    }

    // MARK: Context

    public var repository: AgentStartTarget? { form.plan?.target }

    /// "owner/repo · mirror35", for the line on the composer.
    public var contextLine: String {
        switch action {
        case .some(.bugReport): return "Bug report · ForkMesh team"
        case .some(.task): return "Task for \(taskAssigneeLabel(taskAssignee))"
        case .some(.note): return "Note · your account"
        case .some(.issue), .some(.activityPub):
            if form.targets == nil || (preparing && repository == nil) { return "Finding a repository…" }
            return repository?.label ?? "Choose a repository"
        case .none: break
        }
        if form.targets == nil || (preparing && repository == nil) { return "Finding a repository…" }
        guard let target = repository else { return "Choose a repository" }
        if form.plan?.loading ?? false { return "\(target.label) · checking…" }
        return "\(target.label) · \(destinationLabel)"
    }

    public var destinationLabel: String {
        switch destination {
        case .some(.node(let name)): return name
        case .some(.device): return "this iPhone"
        case .some(.desktop(let name)): return name.isEmpty ? "your desktop" : "\(name) (desktop)"
        case .none: return "nowhere to run yet"
        }
    }

    /// Every place this repository's run could go, for the picker.
    public var destinations: [QuickLaunchDestination] {
        var all: [QuickLaunchDestination] = []
        if form.plan?.options?.canQueue ?? false {
            all += (form.plan?.options?.targets ?? []).map { .node($0.node) }
        }
        if let linked = linkedDesktop { all.append(linked) }
        if form.canRunOnDevice { all.append(.device) }
        if model.runsOnDevice { return all.filter { $0 == .device } }
        return all
    }

    private var linkedDesktop: QuickLaunchDestination? {
        guard repository?.isOnMesh ?? false, let name = desktop?.desktopName, !name.isEmpty else { return nil }
        return .desktop(name)
    }

    /// A node's line in the picker.
    public func detail(_ destination: QuickLaunchDestination) -> String {
        switch destination {
        case .device:
            if model.runsOnDevice { return form.onDeviceGemmaLabel }
            return form.deviceCredentials == nil ? "Needs a model or an API key" : form.deviceModelLabel
        case .desktop:
            return ["Your desktop, directly", desktop?.desktopDetail ?? ""].filter { !$0.isEmpty }.joined(separator: " · ")
        case .node(let name):
            guard let node = form.plan?.options?.targetNamed(name) else { return "" }
            let kind = node.kindLabel
            let runs = node.runs(model.provider) ? "" : " · no \(agentProviderLabel(model.provider))"
            return "\(kind) · \(node.statusLabel)\(runs)"
        }
    }

    /// The models the chip offers.
    public var modelChoices: [AgentModelChoice] {
        (actions == nil ? [] : QuickPromptAction.allCases.map(\.choice)) + form.nodeModelChoices + (form.offersOnDeviceGemma ? [AgentModelCatalog.onDeviceGemma] : [])
    }

    /// Load the repositories once and settle the defaults. `currentRepo` is
    /// the context the composer was opened from ("owner/name"), if any.
    public func prepare(currentRepo: String? = nil) async {
        if prepared && currentRepo == nil && !form.targetsNeedLoading {
            if form.optionsOlder(than: Self.optionsFreshFor) { await refresh() }
            return
        }
        prepared = true
        preparing = true
        defer { preparing = false }
        if form.targetsNeedLoading { await form.loadTargets() }
        let targets = form.targets ?? []
        let wanted = [currentRepo ?? "", memory.lastRepo].map { $0.lowercased() }.filter { !$0.isEmpty }
        let chosen = wanted.lazy.compactMap { key in targets.first { Self.matches($0, key) } }.first
            ?? Self.mostRecent(targets, account: account())
        if let chosen, chosen != form.plan?.target {
            await form.selectTarget(chosen)
        } else if form.optionsOlder(than: Self.optionsFreshFor) {
            await form.refreshOptions()
        }
        destination = resolveDestination()
        settleDefault()
        if action == .task && taskPeople == nil { await loadTaskPeople() }
    }

    public static let optionsFreshFor: TimeInterval = 15

    public func refresh() async {
        guard await form.refreshOptions() else { return }
        let kept: Bool
        switch destination {
        case .none: kept = false
        case .some(.node(let name)): kept = form.plan?.options?.targetNamed(name) != nil
        default: kept = true
        }
        if !kept { destination = resolveDestination() }
        settleDefault()
    }

    static func mostRecent(_ targets: [AgentStartTarget], account: String) -> AgentStartTarget? {
        let me = account.dartTrimmed.lowercased()
        func mine(_ target: AgentStartTarget) -> Bool {
            guard !me.isEmpty, let repo = target.repo else { return false }
            return [repo.owner, repo.displayOwner, repo.organization].contains { $0.lowercased() == me }
        }
        func newest(_ list: [AgentStartTarget]) -> AgentStartTarget? {
            list.max { ($0.repo?.updatedMs ?? 0) < ($1.repo?.updatedMs ?? 0) }
        }
        return newest(targets.filter(mine)) ?? targets.first(where: \.isMirrored) ?? newest(targets)
    }

    static func matches(_ target: AgentStartTarget, _ key: String) -> Bool {
        target.key == key || "\(target.owner)/\(target.name)".lowercased() == key
            || (target.record?.name.lowercased() ?? "") == key
    }

    /// The picker chose a repository: probe it and remember it.
    public func choose(_ target: AgentStartTarget) async {
        memory.remember(repoKey: target.key)
        await form.selectTarget(target)
        destination = resolveDestination()
        settleDefault()
    }

    public var baseBranches: [String] { form.branchOptions }

    public var baseBranch: String { form.baseBranchLabel }

    public var offersBaseBranch: Bool {
        if action != nil { return false }
        if case .some(.node) = destination { return form.branchOptions.count > 1 }
        return false
    }

    public func choose(baseBranch: String) {
        form.baseBranch = baseBranch == form.defaultBranch ? "" : baseBranch
    }

    /// The picker chose where this repository runs.
    public func choose(_ destination: QuickLaunchDestination) {
        self.destination = destination
        if let key = repository?.key { memory.remember(repoKey: key, destination: destination) }
        settleDefault()
    }

    public func prefer(_ destination: QuickLaunchDestination) {
        self.destination = destination
    }

    /// The chip chose a model; a node that cannot run its provider is
    /// reconsidered.
    public func choose(model: AgentModelChoice) {
        if let action = QuickPromptAction(choice: model) {
            choose(action: action)
            return
        }
        rememberAction(nil)
        let wasOnDevice = self.model.runsOnDevice
        self.model = model
        NodeModelPreference.write(model, to: prefs)
        if model.runsOnDevice || wasOnDevice {
            destination = resolveDestination()
            return
        }
        if case .some(.node(let name)) = destination,
            let node = form.plan?.options?.targetNamed(name), !node.runs(model.provider),
            memory.destination(for: repository?.key ?? "") != destination
        {
            destination = resolveDestination()
        }
    }

    public var takesMode: Bool {
        if action != nil { return false }
        if case .some(.desktop) = destination { return AgentPermissionMode.providerTakesMode(model.provider) }
        return false
    }

    public func choose(mode: AgentPermissionMode) {
        self.mode = mode
        mode.write(to: prefs)
    }

    public var takesRunOptions: Bool {
        if action != nil { return false }
        if case .some(.desktop) = destination { return true }
        return false
    }

    public var takesNoWorktree: Bool { takesRunOptions && AgentPermissionMode.providerTakesMode(model.provider) }

    public var sendsYolo: Bool { runOptions.yolo && (runOptions.worktree || !takesNoWorktree) }

    public func toggleYolo() {
        runOptions.yolo.toggle()
        prefs?.set(runOptions.yolo, forKey: QuickLaunchRunOptions.yoloKey)
    }

    public func toggleWorktree() {
        runOptions.worktree.toggle()
    }

    /// The provider setup sheet saved: a run here may now be possible.
    public func credentialsChanged() {
        form.reloadCredentials()
        if destination == nil { destination = resolveDestination() }
        settleDefault()
    }

    /// The default for the chosen repository (see the header).
    func resolveDestination() -> QuickLaunchDestination? {
        guard let plan = form.plan, !plan.loading else { return nil }
        if model.runsOnDevice { return form.canRunOnDevice ? .device : nil }
        let options = plan.options
        let canQueue = options?.canQueue ?? false
        if let remembered = memory.destination(for: plan.target.key) {
            switch remembered {
            case .node(let name) where canQueue && options?.targetNamed(name) != nil: return remembered
            case .device where form.canRunOnDevice: return remembered
            case .desktop where linkedDesktop != nil: return linkedDesktop
            default: break
            }
        }
        if canQueue, let node = options?.targets.first(where: { $0.online && $0.runs(model.provider) }) {
            return .node(node.node)
        }
        if form.canRunOnDevice && form.deviceCredentials != nil { return .device }
        if canQueue, let node = options?.targets.first(where: { $0.runs(model.provider) }) ?? options?.preferredTarget {
            return .node(node.node)
        }
        if form.canRunOnDevice { return .device }
        return nil
    }

    // MARK: Send

    /// Start the run now. On anything but success the caller keeps the text.
    public func send(prompt: String, images: [PromptAttachment]) async -> QuickLaunchOutcome {
        if prompt.dartTrimmed.isEmpty && images.isEmpty { return .ignored }
        if action == nil && (!prepared || form.targetsNeedLoading) { await prepare() }
        if let action { return await send(action, prompt: prompt, images: images) }
        return await sendAgent(prompt: prompt, images: images)
    }

    private func send(_ action: QuickPromptAction, prompt: String, images: [PromptAttachment]) async -> QuickLaunchOutcome {
        guard let actions else { return .failed("\(action.label) is unavailable in this build.") }
        let text = prompt.dartTrimmed
        if text.isEmpty {
            return .failed(action == .bugReport ? "Describe what went wrong." : "Type something first.")
        }
        if !action.takesPhotos && !images.isEmpty {
            return .failed("\(action.label) cannot include photos yet. Remove them, or send a bug report.")
        }
        if action.needsAccount && !form.signedIn {
            return .needsFix(.signInAccount, message: "Sign in to use \(action.label).")
        }
        let parts = QuickPromptAction.titleAndBody(text)
        do {
            switch action {
            case .bugReport:
                try await actions.reportBug(text, photos: images)
                return .done("Sent to the ForkMesh team as a bug report.")
            case .task:
                let repo = repository.map { "\($0.owner)/\($0.name)" } ?? ""
                try await actions.createTask(title: parts.title, details: text, assignee: taskAssignee, repository: repo)
                return .done(taskAssignee.isEmpty ? "Created an unassigned task on General." : "Created a task for @\(taskAssignee).")
            case .note:
                try await actions.takeNote(title: parts.title, markdown: parts.body)
                return .done("Saved as a note.")
            case .issue, .activityPub:
                if !prepared || form.targetsNeedLoading { await prepare() }
                guard let repo = repository?.repo else {
                    return .needsFix(.chooseRepository, message: "Choose a published repository for \(action.label).")
                }
                if action == .issue {
                    try await actions.createIssue(owner: repo.owner, name: repo.name, title: parts.title, body: parts.body)
                    return .done("Filed an issue on \(repo.owner)/\(repo.name).")
                }
                try await actions.postToActivityPub(
                    owner: repo.owner, name: repo.name, title: String(parts.title.prefix(240)), body: String(parts.body.prefix(4000)))
                return .done("Published to ActivityPub for \(repo.owner)/\(repo.name).")
            }
        } catch {
            return .failed((error as? QuickActionError)?.message ?? "\(action.label) did not go through. Try again.")
        }
    }

    private func sendAgent(prompt: String, images: [PromptAttachment]) async -> QuickLaunchOutcome {
        if !prepared || form.targetsNeedLoading { await prepare() }
        if !form.signedIn {
            return .needsFix(.signInAccount, message: "Sign in and the prompt goes straight to a mirror of the repository.")
        }
        guard let plan = form.plan else {
            return .needsFix(.chooseRepository, message: "There is no repository to point the agent at yet.")
        }
        if plan.loading {
            await form.selectTarget(plan.target)
            destination = resolveDestination()
        }
        if destination == nil { await refresh() }
        guard let destination else {
            return noDestination(form.plan ?? plan)
        }
        memory.remember(repoKey: plan.target.key)
        if case .desktop(let name) = destination {
            return await sendToDesktop(plan.target, name: name, prompt: prompt, images: images)
        }
        form.attachments = images
        form.prompt = prompt
        switch destination {
        case .desktop:
            return .failed("The desktop did not take the prompt.")
        case .node(let name):
            if let node = form.plan?.options?.targetNamed(name), !node.runs(model.provider),
                isAgentLoginProvider(model.provider)
            {
                await form.refreshOptions()
            }
            if let node = form.plan?.options?.targetNamed(name), !node.runs(model.provider),
                isAgentLoginProvider(model.provider)
            {
                return .needsFix(
                    .signIn(node, provider: model.provider),
                    message: "\(node.signInNode) has no \(agentProviderLabel(model.provider)) signed in.")
            }
            form.mode = .node
            form.nodeChoice = name
            form.nodeModel = model
            if await form.startOnNode() {
                let result = form.lastQueued
                let message = result?.label ?? "Queued."
                guard let repo = plan.target.repo, let session = result?.session, !session.sessionId.isEmpty else {
                    return .queuedWithoutSession(message: message)
                }
                var opened = session
                opened.orgScope = plan.options?.orgScope ?? false
                await rememberSentPhotos(images, prompt: prompt, repo: repo, session: opened)
                return .queued(repo: repo, session: opened, message: message)
            }
            return .failed(form.error ?? "The relay did not take the prompt.")
        case .device:
            if model.runsOnDevice {
                let readiness = form.onDeviceGemmaReadiness
                if !readiness.isReady {
                    return readiness.canBeFixedInSettings
                        ? .needsFix(.setUpGemma, message: readiness.message) : .failed(readiness.message)
                }
            } else if form.deviceCredentials == nil {
                return .needsFix(.setUpDevice, message: "Choose a model for agents on this iPhone, then send again.")
            }
            form.mode = .device
            switch form.deviceStart() {
            case .some(.open(let record, let text)):
                let photos = form.attachments
                form.deviceRunStarted()
                return .device(
                    record: record, prompt: text, images: photos,
                    credentials: model.runsOnDevice ? form.onDeviceGemmaCredentials : nil)
            case .some(.clone(let owner, let name, _)):
                return .needsFix(
                    .mirrorToDevice(owner: owner, name: name),
                    message: "\(owner)/\(name) is not on this iPhone yet.")
            case .none:
                return .needsFix(.chooseRepository, message: "There is nothing to run here.")
            }
        }
    }

    public func send(prompt: String, repository: String?, onDevice: Bool) async -> QuickLaunchOutcome {
        if prompt.dartTrimmed.isEmpty { return .ignored }
        let previous = self.repository?.key
        let wanted = repository?.dartTrimmed.lowercased() ?? ""
        var outcome: QuickLaunchOutcome
        if wanted.isEmpty {
            await prepare()
            outcome = await sendNamed(prompt: prompt, onDevice: onDevice)
        } else {
            if form.targetsNeedLoading { await form.loadTargets() }
            if let target = Self.named(form.targets ?? [], wanted) {
                await prepare(currentRepo: target.key)
                outcome = self.repository?.key == target.key
                    ? await sendNamed(prompt: prompt, onDevice: onDevice) : Self.unknownRepository(wanted)
            } else {
                outcome = Self.unknownRepository(wanted)
            }
        }
        if let previous, previous != self.repository?.key {
            await prepare(currentRepo: previous)
        }
        return outcome
    }

    private func sendNamed(prompt: String, onDevice: Bool) async -> QuickLaunchOutcome {
        if !onDevice {
            if destination == nil { await refresh() }
            if destination == .device {
                return .failed("Agents for \(repository?.label ?? "this repository") run on your iPhone. Start this one there.")
            }
        }
        return await sendAgent(prompt: prompt, images: [])
    }

    static func named(_ targets: [AgentStartTarget], _ wanted: String) -> AgentStartTarget? {
        if let exact = targets.first(where: { matches($0, wanted) || "\($0.publicOwner)/\($0.name)".lowercased() == wanted }) {
            return exact
        }
        let spoken = wanted.replacingOccurrences(of: " ", with: "")
        let byName = targets.filter { target in
            [target.name, target.record?.name ?? ""].contains { name in
                let lower = name.lowercased()
                return !lower.isEmpty && (lower == wanted || lower.replacingOccurrences(of: "-", with: "") == spoken)
            }
        }
        return byName.max { ($0.repo?.updatedMs ?? 0) < ($1.repo?.updatedMs ?? 0) }
    }

    static func unknownRepository(_ wanted: String) -> QuickLaunchOutcome {
        .needsFix(.chooseRepository, message: "No repository called \(wanted) can take an agent from this iPhone.")
    }

    private func sendToDesktop(
        _ target: AgentStartTarget, name: String, prompt: String, images: [PromptAttachment]
    ) async -> QuickLaunchOutcome {
        if let problem = promptAttachmentsProblem(images) { return .failed(problem) }
        guard let desktop else { return .failed(ControlErrorText.describe(code: "not_connected")) }
        var owners: [String] = []
        for owner in [target.publicOwner, target.owner] where !owner.isEmpty && !owners.contains(owner) {
            owners.append(owner)
        }
        var failure: Error?
        for owner in owners {
            do {
                let id = try await desktop.start(
                    owner: owner, name: target.name, task: promptOrPhotoNote(prompt, images),
                    provider: model.provider, model: model.modelId, mode: takesMode ? mode.rawValue : "",
                    options: QuickLaunchRunOptions(
                        yolo: sendsYolo, worktree: runOptions.worktree || !takesNoWorktree),
                    images: images)
                runOptions.worktree = true
                let place = name.isEmpty ? "your desktop" : name
                return .desktop(
                    name: name, agentId: id, message: id > 0 ? "Started agent #\(id) on \(place)." : "Sent to \(place).")
            } catch {
                failure = error
                if (error as? LocalControlException)?.code != "no_checkout" { break }
            }
        }
        return .failed(failure.map { ControlErrorText.describe($0) } ?? "The desktop did not take the prompt.")
    }

    private func rememberSentPhotos(
        _ photos: [PromptAttachment], prompt: String, repo: Repository, session: AgentLaunchSession
    ) async {
        if photos.isEmpty { return }
        let text = promptOrPhotoNote(prompt, photos)
        sentPhotos.record(sessionId: session.sessionId, text: text, photos: photos)
        let loader = AgentAttachmentLoader(store: attachmentStore)
        await loader.seedLocal(photos)
        if let index = AgentAttachmentReconcile.turn(for: photos, in: session.history),
            let pairs = AgentAttachmentReconcile.pair(sent: photos, listed: session.history[index].attachments)
        {
            await loader.seed(
                pairs,
                origin: .relay(owner: repo.owner, repo: repo.name, sessionId: session.sessionId, orgScope: session.orgScope))
        }
    }

    private func noDestination(_ plan: AgentStartPlan) -> QuickLaunchOutcome {
        if model.runsOnDevice {
            return .needsFix(
                .chooseRepository,
                message: "Gemma on this iPhone cannot run \(plan.target.label). Pick another repository or model.")
        }
        if let node = plan.options?.targets.first, isAgentLoginProvider(model.provider) {
            return .needsFix(
                .signIn(node, provider: model.provider),
                message: "No node can run \(agentProviderLabel(model.provider)) for \(plan.target.label) yet.")
        }
        if !plan.error.isEmpty { return .failed(plan.error) }
        return .needsFix(
            .chooseRepository,
            message: "Nothing can run agents for \(plan.target.label). Pick another repository.")
    }
}
