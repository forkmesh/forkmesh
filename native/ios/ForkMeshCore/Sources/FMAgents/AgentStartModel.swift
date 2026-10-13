// The logic of the "Start an agent" form. Say what you want built, pick the repository, and the prompt is sent
// to a mirror through the relay queue, the same queue the web's Agents page
// writes to; it runs on this phone, against a working mirror, only when no
// node can take it. A run on this phone may use Gemma 4 on the phone itself
// instead of an API key.

import FMApi
import FMBase
import FMCode
import FMLocalAI
import Foundation
import Observation

/// Where an agent run happens.
public enum AgentRunMode: Sendable {
    /// Queued on the relay for a desktop or mirror node to execute. The node
    /// has the repository, the toolchain and the provider credentials, so this
    /// is the run that can build and test before it opens a pull request.
    case node
    /// Executed here, against a working mirror on this device, with the
    /// provider this phone is set up with.
    case device
}

/// A repository an agent can be pointed at: on the mesh, on this device, or
/// both. Which of those is true decides where the run can happen.
public struct AgentStartTarget: Equatable, Identifiable, Sendable {
    public let repo: Repository?
    public let record: LocalRepoRecord?

    public init(repo: Repository? = nil, record: LocalRepoRecord? = nil) {
        self.repo = repo
        self.record = record
    }

    private var publishedParts: (String, String) {
        let published = (record?.publishedAs ?? "").dartTrimmed
        guard let slash = published.firstIndex(of: "/"), slash != published.startIndex else { return ("", "") }
        return (String(published[..<slash]), String(published[published.index(after: slash)...]))
    }

    public var owner: String { repo?.owner ?? publishedParts.0 }
    public var publicOwner: String { repo?.displayOwner ?? publishedParts.0 }
    public var name: String { repo?.name ?? publishedParts.1 }

    public var isOnMesh: Bool { !owner.isEmpty && !name.isEmpty }
    public var isMirrored: Bool { record != nil }

    /// The repository by its public name. `owner` is the node serving it (a
    /// mirror, for the flagship), and that is the relay's business.
    public var label: String {
        isOnMesh ? (repo?.fullName ?? "\(owner)/\(name)") : (record?.name ?? "")
    }

    public var key: String { label.lowercased() }
    public var id: String { key }

    public var detail: String {
        if isMirrored && isOnMesh { return "Mirrored on this device" }
        if isMirrored { return "On this device · not published" }
        let mirrors = repo?.mirrors ?? 0
        if mirrors > 0 { return "On the mesh · \(mirrors) \(mirrors == 1 ? "mirror" : "mirrors") · not on this device yet" }
        return "On the mesh · not on this device yet"
    }

    /// A run on this device needs a working mirror; without one it is cloned
    /// first.
    public var canRunOnDevice: Bool { isMirrored || isOnMesh }
}

/// Everything the form knows about one target after probing the relay.
public struct AgentStartPlan: Equatable, Sendable {
    public let target: AgentStartTarget
    public let options: AgentLaunchOptions?
    public let error: String
    public let loading: Bool

    public init(target: AgentStartTarget, options: AgentLaunchOptions? = nil, error: String = "", loading: Bool = false) {
        self.target = target
        self.options = options
        self.error = error
        self.loading = loading
    }

    public var node: AgentLaunchTarget? { options?.preferredTarget }
    public var canRunOnNode: Bool { (options?.canQueue ?? false) && node != nil }
    public var defaultMode: AgentRunMode { canRunOnNode ? .node : .device }
}

/// What the view does for a run on this device.
public enum AgentDeviceStart: Equatable, Sendable {
    /// Open the repository's agent with the prompt, and start it.
    case open(LocalRepoRecord, prompt: String)
    /// Mirror `owner/name` to this device first, then open it.
    case clone(owner: String, name: String, prompt: String)
}

@MainActor
@Observable
public final class AgentStartModel {
    @ObservationIgnored private let api: AgentsApi?
    @ObservationIgnored private let records: (@MainActor () async -> [LocalRepoRecord])?
    @ObservationIgnored private let isSignedIn: @MainActor () -> Bool
    @ObservationIgnored private let credentialStore: AgentCredentialStore?
    @ObservationIgnored private let localModels: @MainActor () -> LocalModelManager?
    @ObservationIgnored private var probe = 0
    @ObservationIgnored public private(set) var optionsCheckedAt: Date?

    public var prompt: String
    public var attachments: [PromptAttachment] = []
    public var nodeModel: AgentModelChoice = AgentModelCatalog.defaultNodeModel
    public private(set) var targets: [AgentStartTarget]?
    @ObservationIgnored private var meshTargetsMissing = false
    public private(set) var plan: AgentStartPlan?
    /// The mode the person picked, when they did not take the default.
    public var mode: AgentRunMode?
    /// The node the person picked, when they did not take the suggested one.
    public var nodeChoice = ""
    public private(set) var busy = false
    public var error: String?
    public private(set) var notice: String?
    public private(set) var lastQueued: AgentQueueResult?
    public private(set) var branchOptions: [String] = []
    public var baseBranch = ""
    /// What a run on this device would use (nil: not set up yet).
    public private(set) var deviceCredentials: AgentCredentials?

    /// A run on this device needs the on-device repository store to open a
    /// worktree at all, so a build without one can only queue on a node.
    public let deviceRunsAvailable: Bool

    public init(
        api: AgentsApi?,
        records: (@MainActor () async -> [LocalRepoRecord])?,
        signedIn: @escaping @MainActor () -> Bool,
        credentialStore: AgentCredentialStore? = nil,
        localModels: @escaping @MainActor () -> LocalModelManager? = { nil },
        initialPrompt: String = ""
    ) {
        self.api = api
        self.records = records
        self.isSignedIn = signedIn
        self.credentialStore = credentialStore
        self.localModels = localModels
        self.deviceRunsAvailable = records != nil
        self.prompt = initialPrompt
    }

    public var signedIn: Bool { isSignedIn() }

    public var targetsNeedLoading: Bool { targets == nil || meshTargetsMissing }

    public var defaultBranch: String {
        let named = (plan?.target.repo?.defaultBranch ?? "").dartTrimmed
        return named.isEmpty ? "main" : named
    }

    public var baseBranchLabel: String { baseBranch.isEmpty ? defaultBranch : baseBranch }

    public var offersBaseBranch: Bool { effectiveMode == .node && branchOptions.count > 1 }

    public static func orderedBranches(_ names: [String], defaultBranch: String) -> [String] {
        var seen: Set<String> = []
        var out: [String] = []
        for name in [defaultBranch] + names where !name.isEmpty && !seen.contains(name) {
            if name != defaultBranch && !names.contains(name) { continue }
            seen.insert(name)
            out.append(name)
        }
        return out
    }
    public var hasPrompt: Bool { !prompt.dartTrimmed.isEmpty }
    public var hasContent: Bool { hasPrompt || !attachments.isEmpty }

    public var nodeModelChoices: [AgentModelChoice] {
        AgentModelCatalog.choices(offered: plan?.options?.models ?? [:])
    }
    public var effectiveMode: AgentRunMode { mode ?? plan?.defaultMode ?? .device }

    /// The node the prompt goes to.
    public var node: AgentLaunchTarget? {
        plan?.options?.targetNamed(nodeChoice) ?? plan?.node
    }

    public var canRunOnDevice: Bool { deviceRunsAvailable && (plan?.target.canRunOnDevice ?? false) }

    /// Both routes open: the form offers the choice.
    public var offersBothModes: Bool { (plan?.canRunOnNode ?? false) && canRunOnDevice }

    /// More than one node could take it: the node row opens a list.
    public var canPickNode: Bool { (plan?.options?.targets.count ?? 0) > 1 }

    public var canStart: Bool {
        guard let plan, !busy, hasContent, !plan.loading else { return false }
        if !signedIn { return true }
        return effectiveMode == .node ? plan.canRunOnNode : canRunOnDevice
    }

    public var submitLabel: String {
        if busy { return "Sending…" }
        if !signedIn { return "Sign in to start" }
        return effectiveMode == .node ? "Send to mirror" : "Start agent"
    }

    public var targetLabel: String {
        if targets == nil { return "Loading repositories…" }
        return plan?.target.label ?? "No repository available"
    }

    /// The providers the chosen node has a CLI for that it will run a login
    /// for: a node advertises an installed CLI, not a signed-in one.
    public var signInProviders: [String] {
        guard effectiveMode == .node, plan?.canRunOnNode ?? false, let node else { return [] }
        if node.isRunner { return node.needsLogin.filter(isAgentLoginProvider) }
        return node.providers.filter(isAgentLoginProvider)
    }

    public var nodeDetail: String {
        guard let node else { return "" }
        return "\(node.kindLabel) · \(node.online ? "online" : "offline — queue for later")"
    }

    /// Whether a run here uses Gemma 4 on this iPhone.
    public var deviceRunsOnGemma: Bool { deviceCredentials?.kind == .onDevice }

    /// Where the on-device model stands, when a run here would use it.
    public var gemmaReadiness: OnDeviceAgentReadiness? {
        deviceRunsOnGemma ? OnDeviceAgentReadiness.of(localModels()) : nil
    }

    /// One sentence on where the run will happen.
    public var runsOnText: String {
        guard let plan else { return "" }
        if plan.loading { return "Checking where this can run…" }
        if !signedIn {
            return "Sign in and the prompt goes to a mirror of this repository, which runs the agent and opens the pull request."
        }
        if !plan.canRunOnNode && !canRunOnDevice {
            return "No node on this account can run agents for this repository, and this build cannot mirror it to the device. Sign in on a desktop or server node to run agents."
        }
        switch effectiveMode {
        case .node:
            guard let node else { return "No node on this account can run it yet." }
            let kind = node.isRunner ? "runner " : node.isMirror ? "mirror " : "node "
            return node.online
                ? "Sent to \(kind)\(node.node), which is online now."
                : "Queued for \(kind)\(node.node). It starts when that node connects."
        case .device:
            if plan.target.isMirrored { return "Runs here, on the mirror already on this device." }
            if plan.target.isOnMesh {
                let runner = deviceRunsOnGemma ? "with Gemma 4 on this iPhone" : "on your own API key"
                return "Clones \(plan.target.label) from its mirrors to this device first, then runs here \(runner)."
            }
            return "This repository is only on this device, so the run happens here."
        }
    }

    /// The model a run here would use, for the device-mode row.
    public var deviceModelLabel: String {
        guard let credentials = deviceCredentials else { return "Choose a model" }
        if credentials.kind == .onDevice {
            return "\(localModels()?.selectedModel.displayName ?? credentials.modelLabel) · on this iPhone"
        }
        return credentials.modelLabel
    }

    public var offersOnDeviceGemma: Bool {
        deviceRunsAvailable && (localModels()?.engineAvailability.isAvailable ?? false)
    }

    public var onDeviceGemmaReadiness: OnDeviceAgentReadiness { OnDeviceAgentReadiness.of(localModels()) }

    public var onDeviceGemmaCredentials: AgentCredentials {
        .onDevice(model: localModels()?.selectedModelId ?? LocalModelCatalog.defaultModel.id)
    }

    public var onDeviceGemmaLabel: String {
        "\(localModels()?.selectedModel.displayName ?? AgentProviderKind.onDevice.label) · on this iPhone"
    }

    // MARK: - Loading

    /// Mesh repositories and on-device mirrors, folded together so a
    /// repository that is both appears once and carries both facts.
    public func loadTargets() async {
        reloadCredentials()
        var order: [String] = []
        var byKey: [String: AgentStartTarget] = [:]
        var orphans: [AgentStartTarget] = []
        func put(_ key: String, _ target: AgentStartTarget) {
            if byKey[key] == nil { order.append(key) }
            byKey[key] = target
        }

        var repos: [Repository] = []
        var meshRead = api == nil
        if let api {
            // Offline is not fatal: a mirror already here can still be worked on.
            if let read = try? await api.repositories() {
                repos = read
                meshRead = true
            }
        }
        // A mirror on this device may be published under the serving node's
        // name or the public one; both name the same repository.
        var publicKeys: [String: String] = [:]
        for repo in repos {
            let key = repo.fullName.lowercased()
            put(key, AgentStartTarget(repo: repo))
            publicKeys["\(repo.owner)/\(repo.name)".lowercased()] = key
        }
        if let records {
            for record in await records() {
                let route = (record.publishedAs ?? "").dartTrimmed.lowercased()
                let published = publicKeys[route] ?? route
                if !published.isEmpty, let existing = byKey[published] {
                    put(published, AgentStartTarget(repo: existing.repo, record: record))
                } else if !published.isEmpty {
                    put(published, AgentStartTarget(record: record))
                } else {
                    orphans.append(AgentStartTarget(record: record))
                }
            }
        }
        let all = order.compactMap { byKey[$0] }
        // Mirrors first: they are the ones a run can start against immediately.
        let ordered = all.filter(\.isMirrored) + orphans + all.filter { !$0.isMirrored }
        targets = ordered
        meshTargetsMissing = !meshRead || repos.isEmpty
        if let first = ordered.first { await selectTarget(first) }
    }

    /// Re-read the provider a run on this device uses (after the setup sheet).
    public func reloadCredentials() {
        deviceCredentials = credentialStore?.read()
    }

    /// Ask the relay where this repository's agents can run.
    public func selectTarget(_ target: AgentStartTarget) async {
        probe += 1
        let generation = probe
        plan = AgentStartPlan(target: target, loading: target.isOnMesh)
        mode = nil
        nodeChoice = ""
        branchOptions = []
        baseBranch = ""
        error = nil
        notice = nil
        guard target.isOnMesh, let api else { return }
        var options: AgentLaunchOptions?
        var failure = ""
        do {
            options = try await api.agentLaunchOptions(target.owner, target.name)
        } catch let error as AgentLaunchException {
            // Not being allowed to queue is an ordinary answer here, not a
            // fault: it simply means this run happens on the device instead.
            failure = error.status == 403 || error.status == 401 ? "" : error.message
        } catch {
            failure = dartErrorText(error)
        }
        if generation != probe { return }
        plan = AgentStartPlan(target: target, options: options, error: failure)
        optionsCheckedAt = Date()
        if options?.canQueue ?? false, let starter = api as? RunnerAgentStarting {
            let names = await starter.runnerBranchNames(target.owner, target.name)
            if generation != probe { return }
            branchOptions = AgentStartModel.orderedBranches(names, defaultBranch: defaultBranch)
        }
    }

    @discardableResult
    public func refreshOptions() async -> Bool {
        guard let current = plan, !current.loading, current.target.isOnMesh, let api else { return false }
        let generation = probe
        guard let options = try? await api.agentLaunchOptions(current.target.owner, current.target.name) else {
            return false
        }
        if generation != probe || plan?.target != current.target { return false }
        plan = AgentStartPlan(target: current.target, options: options)
        optionsCheckedAt = Date()
        return true
    }

    public func optionsOlder(than seconds: TimeInterval, now: Date = Date()) -> Bool {
        guard let checked = optionsCheckedAt else { return true }
        return now.timeIntervalSince(checked) >= seconds
    }

    // MARK: - Starting

    /// Send the prompt to a mirror through the relay queue: the relay writes
    /// the job, rings the node, and the node cuts a branch, runs the agent and
    /// opens the pull request. True when a run was queued.
    @discardableResult
    public func startOnNode() async -> Bool {
        guard let plan, let api, !busy else { return false }
        busy = true
        error = nil
        notice = nil
        defer { busy = false }
        do {
            let firstLine = (prompt.components(separatedBy: "\n").first ?? "").dartTrimmed
            let title = firstLine.isEmpty && !attachments.isEmpty ? "Photo prompt" : firstLine
            if let starter = api as? RunnerAgentStarting {
                let result = try await starter.launchAgentSession(
                    plan.target.owner, plan.target.name, prompt: prompt, provider: nodeModel.launchProvider,
                    model: nodeModel.alias, targetNode: node?.node ?? "", title: title, createPr: false,
                    issueNumber: 0, orgScope: plan.options?.orgScope ?? false, attachments: attachments,
                    baseBranch: baseBranch == defaultBranch ? "" : baseBranch, branchName: title)
                prompt = ""
                attachments = []
                lastQueued = result
                notice = result.label
                return true
            }
            let result = try await api.launchAgentSession(
                plan.target.owner,
                plan.target.name,
                prompt: prompt,
                provider: nodeModel.launchProvider,
                model: nodeModel.alias,
                targetNode: node?.node ?? "",
                title: firstLine.isEmpty && !attachments.isEmpty ? "Photo prompt" : firstLine,
                createPr: false,
                issueNumber: 0,
                orgScope: plan.options?.orgScope ?? false,
                attachments: attachments
            )
            prompt = ""
            attachments = []
            lastQueued = result
            notice = result.label
            return true
        } catch let failure as AgentLaunchException {
            error = failure.message
        } catch {
            self.error = dartErrorText(error)
        }
        return false
    }

    /// Where a run on this device goes: the mirror already here, or a clone
    /// of the mesh repository first. Nil when there is nothing to run.
    public func deviceStart() -> AgentDeviceStart? {
        var text = prompt.dartTrimmed
        if text.isEmpty && !attachments.isEmpty { text = "See the attached photos." }
        guard !text.isEmpty, let target = plan?.target else { return nil }
        if let record = target.record { return .open(record, prompt: text) }
        if target.isOnMesh { return .clone(owner: target.publicOwner, name: target.name, prompt: text) }
        return nil
    }

    /// The device run was opened: clear the form.
    public func deviceRunStarted() {
        prompt = ""
        attachments = []
    }
}
