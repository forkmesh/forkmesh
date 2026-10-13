// The "Start an agent" form. The prompt goes to a mirror through the relay queue; with no node to
// take it, it runs on this phone against a working mirror, on the provider
// this phone is set up with, which may be Gemma 4 on this iPhone. Logic:
// FMAgents' AgentStartModel.
//
// The shell presents `AgentStartCard()` from its prompt bar and offers the
// typed text through `AgentPromptHandoff`.

import FMAccount
import FMAgents
import FMApi
import FMBase
import FMCode
import FMLocalAI
import SwiftUI

/// A repository opened on this device with the agent's prompt.
struct AgentDeviceRun: Identifiable {
    let id: UUID = UUID()
    let record: LocalRepoRecord
    let prompt: String
}

/// The node sign-in a button asked for.
struct AgentSignInRequest: Identifiable {
    let id: UUID = UUID()
    let node: String
    let provider: String
}

struct AgentStartCard: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: AgentStartModel
    @State private var deviceRun: AgentDeviceRun?
    @State private var signIn: AgentSignInRequest?
    @State private var runnerSignIn: RunnerSignInRequest?
    @State private var showTargets: Bool = false
    @State private var showNodes: Bool = false
    @State private var showSetup: Bool = false
    @State private var showLocalAI: Bool = false
    @State private var loaded: Bool = false
    private let onStarted: (() -> Void)?

    init() {
        self.init(onStarted: nil)
    }

    /// `onStarted`: a run was started (the Agents tab reloads its list).
    init(onStarted: (() -> Void)?) {
        self.onStarted = onStarted
        let api: AgentsApi? = Services.get(ApiService.self)
        let auth: AuthService? = Services.get(AuthService.self)
        _model = State(initialValue: AgentStartModel(
            api: api,
            records: AgentsFeature.localRecords(),
            signedIn: { auth == nil || auth?.session != nil },
            credentialStore: AgentsFeature.credentialStore(),
            localModels: { AgentsFeature.localModels() }
        ))
    }

    /// The store's revision, so a repository cloned from this form shows up.
    private var recordsRevision: Int {
        return Services.get(LocalRepoStore.self)?.revision ?? 0
    }

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: 0) {
                header
                promptField.padding(.top, FmSpace.x4)
                targetField.padding(.top, FmSpace.x3)
                AgentRunsOn(
                    model: model,
                    onPickNode: { showNodes = true },
                    onSignIn: { (node: AgentLaunchTarget, provider: String) in
                        if node.isRunner {
                            runnerSignIn = RunnerSignInRequest(runner: node.signInNode.lowercased(), provider: provider)
                        } else {
                            signIn = AgentSignInRequest(node: node.node, provider: provider)
                        }
                    },
                    onPickModel: { showSetup = true },
                    onOpenLocalAI: { showLocalAI = true }
                )
                .padding(.top, FmSpace.x3)
                notes
                submit.padding(.top, FmSpace.x4)
            }
        }
        .task {
            if loaded {
                return
            }
            loaded = true
            let handoff: AgentPromptHandoff? = Services.get(AgentPromptHandoff.self)
            if let handed = handoff?.takePrompt(), model.prompt.isEmpty {
                model.prompt = handed
            }
            // Photos attached in the composer come along with its text.
            if let images = handoff?.takeImages(), !images.isEmpty {
                model.attachments = images
            }
            await model.loadTargets()
        }
        .onChange(of: recordsRevision) { (_: Int, _: Int) in
            Task { await model.loadTargets() }
        }
        .sheet(isPresented: $showTargets) { targetSheet }
        .sheet(isPresented: $showNodes) { nodeSheet }
        .sheet(isPresented: $showSetup, onDismiss: { model.reloadCredentials() }) {
            AgentSetupSheet(initial: model.deviceCredentials) { (_: AgentCredentials?) in
                model.reloadCredentials()
            }
            .fmSheetStyle()
        }
        .sheet(isPresented: $showLocalAI, onDismiss: { model.reloadCredentials() }) {
            NavigationStack {
                LocalAISettingsView()
            }
        }
        .fullScreenCover(item: $signIn) { (request: AgentSignInRequest) in
            NavigationStack {
                AgentLoginTerminalScreen(node: request.node, provider: request.provider)
                    .toolbar { closeButton { signIn = nil } }
            }
        }
        .sheet(item: $runnerSignIn, onDismiss: { Task { await model.loadTargets() } }) { (request: RunnerSignInRequest) in
            if let attention = Services.get(RunnerAttentionModel.self) {
                RunnerSignInSheet(model: attention.loginModel(for: request))
            }
        }
        .fullScreenCover(item: $deviceRun, onDismiss: deviceRunClosed) { (run: AgentDeviceRun) in
            NavigationStack {
                LocalRepoScreen(record: run.record, agentPrompt: run.prompt, agentAutoStart: true)
                    .toolbar { closeButton { deviceRun = nil } }
            }
        }
    }

    private func closeButton(_ action: @escaping () -> Void) -> some ToolbarContent {
        ToolbarItem(placement: .topBarLeading) {
            Button("Done", action: action)
        }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                FmIcon("auto_awesome_outlined", size: 22, color: fm.accent)
                Text("Start an agent").fmText(.titleSmall)
            }
            Text("Describe the change. It is sent to a mirror, which works on its own branch and opens a pull request when it is done.")
                .fmText(.bodySmall, color: fm.textSecondary, height: 1.45)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private var promptBinding: Binding<String> {
        Binding<String>(
            get: { model.prompt },
            set: { (value: String) in
                // The relay's own cap (ApiService.maxAgentPromptChars).
                model.prompt = value.utf16.count > ApiService.maxAgentPromptChars
                    ? String(value.prefix(ApiService.maxAgentPromptChars))
                    : value
            }
        )
    }

    private var promptField: some View {
        FmField(label: "Prompt") {
            FmTextField(
                text: promptBinding,
                hint: "Fix the crash when opening an empty repository…",
                multiline: true
            )
            .frame(minHeight: 72, alignment: .top)
        }
    }

    private var targetField: some View {
        FmField(label: "Repository") {
            AgentTargetButton(
                icon: "folder_outlined",
                label: model.targetLabel,
                detail: model.plan?.target.detail ?? "",
                onTap: (model.targets ?? []).isEmpty ? nil : { showTargets = true }
            )
        }
    }

    @ViewBuilder private var notes: some View {
        if let notice = model.notice {
            AgentNote(text: notice, color: fm.success, icon: "check_circle_outline")
                .padding(.top, FmSpace.x3)
        }
        if let error = model.error {
            AgentNote(text: error, color: fm.danger, icon: "error_outline")
                .padding(.top, FmSpace.x3)
        }
    }

    private var submit: some View {
        HStack {
            Spacer(minLength: 0)
            FmToneButton(
                label: model.submitLabel,
                icon: "play_arrow_rounded",
                tone: .accent,
                filled: true,
                height: 40,
                onPressed: model.canStart ? { Task { await start() } } : nil
            )
        }
    }

    // MARK: Behaviour

    private func start() async {
        guard var plan = model.plan else {
            return
        }
        if !model.signedIn {
            // Where this can run is an answer the relay only gives an
            // account, so sign in first and ask again before deciding.
            if !(await AccountFeature.requireAccount(action: "start an agent")) {
                return
            }
            await model.selectTarget(plan.target)
            guard let refreshed = model.plan else {
                return
            }
            plan = refreshed
            if !plan.canRunOnNode && !model.canRunOnDevice {
                return
            }
        }
        if model.effectiveMode == .node {
            if !(await AccountFeature.requireAccount(action: "start an agent")) {
                return
            }
            if await model.startOnNode() {
                announceStarted()
            }
            return
        }
        if !(await AccountFeature.requireAccount(action: "run a coding agent")) {
            return
        }
        switch model.deviceStart() {
        case .some(.open(let record, let prompt)):
            // The on-device agent picks the photos up when it auto-starts.
            Services.get(AgentPromptHandoff.self)?.offer(images: model.attachments)
            deviceRun = AgentDeviceRun(record: record, prompt: prompt)
            model.deviceRunStarted()
        case .some(.clone):
            // Mirror first: the Clone button in the card does it, and the
            // form picks the new mirror up when the store changes.
            model.error = "Mirror \(plan.target.label) to this device first, then start the agent."
        case .none:
            break
        }
    }

    private func deviceRunClosed() {
        announceStarted()
    }

    private func announceStarted() {
        onStarted?()
        Services.get(AgentPromptHandoff.self)?.started()
    }

    // MARK: Sheets

    private var targetSheet: some View {
        let targets: [AgentStartTarget] = model.targets ?? []
        let mirrored: [AgentStartTarget] = targets.filter { $0.isMirrored }
        let remote: [AgentStartTarget] = targets.filter { !$0.isMirrored }
        return ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                Text("Run the agent on")
                    .fmText(.titleSmall)
                    .padding(FmSpace.x4)
                if !mirrored.isEmpty {
                    FmGroupHeader(label: "Mirrored on this device")
                    ForEach(mirrored) { (target: AgentStartTarget) in
                        targetRow(target)
                    }
                }
                if !remote.isEmpty {
                    FmGroupHeader(label: "On the mesh")
                    ForEach(remote) { (target: AgentStartTarget) in
                        targetRow(target)
                    }
                }
            }
        }
        .fmSheetStyle(fitsContent: false)
    }

    private func targetRow(_ target: AgentStartTarget) -> some View {
        let selected: Bool = target.key == model.plan?.target.key
        return FmListRow(
            title: target.label,
            subtitle: target.detail,
            leading: AnyView(FmIcon(target.isMirrored ? "folder_outlined" : "cloud_outlined", size: 18, color: fm.textTertiary)),
            trailing: selected ? AnyView(FmIcon("check", size: 18, color: fm.accent)) : nil,
            onTap: {
                showTargets = false
                Task { await model.selectTarget(target) }
            },
            showChevron: false
        )
    }

    private var nodeSheet: some View {
        let targets: [AgentLaunchTarget] = model.plan?.options?.targets ?? []
        return ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                Text("Send the prompt to")
                    .fmText(.titleSmall)
                    .padding(FmSpace.x4)
                ForEach(targets, id: \.node) { (target: AgentLaunchTarget) in
                    FmListRow(
                        title: target.node,
                        subtitle: "\(target.kindLabel) · \(target.online ? "online" : "offline — queue for later")",
                        leading: AnyView(FmIcon(target.isMirror ? "dns_outlined" : "computer_outlined", size: 18, color: fm.textTertiary)),
                        trailing: target.node == model.node?.node ? AnyView(FmIcon("check", size: 18, color: fm.accent)) : nil,
                        onTap: {
                            model.nodeChoice = target.node
                            showNodes = false
                        },
                        showChevron: false
                    )
                }
            }
        }
        .fmSheetStyle(fitsContent: false)
    }
}

/// Where the run will happen, and the choice when both are possible.
struct AgentRunsOn: View {
    @Environment(\.fmPalette) private var fm
    let model: AgentStartModel
    let onPickNode: () -> Void
    let onSignIn: (AgentLaunchTarget, String) -> Void
    let onPickModel: () -> Void
    let onOpenLocalAI: () -> Void

    var body: some View {
        if model.plan != nil {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                if model.signedIn && !(model.plan?.loading ?? false) {
                    modeTabs
                    nodeRow
                    baseRow
                    deviceRow
                }
                Text(model.runsOnText)
                    .fmText(.bodySmall, color: fm.textTertiary, height: 1.4)
                    .fixedSize(horizontal: false, vertical: true)
                if model.signedIn {
                    cloneRow
                    signIns
                }
                if let error = model.plan?.error, !error.isEmpty {
                    Text(error).fmText(.bodySmall, color: fm.warning)
                        .fmMessageTools(error)
                }
            }
        }
    }

    @ViewBuilder private var modeTabs: some View {
        if model.offersBothModes {
            FmSegmentTabBar(
                tabs: [FmSegmentTab(label: "On a node"), FmSegmentTab(label: "On this device")],
                index: model.effectiveMode == .node ? 0 : 1
            ) { (index: Int) in
                model.mode = index == 0 ? .node : .device
            }
        }
    }

    @ViewBuilder private var nodeRow: some View {
        if model.effectiveMode == .node, let node = model.node, model.canPickNode {
            AgentTargetButton(icon: "dns_outlined", label: node.node, detail: model.nodeDetail, onTap: onPickNode)
        }
    }

    @ViewBuilder private var baseRow: some View {
        if model.offersBaseBranch {
            Menu {
                ForEach(model.branchOptions, id: \.self) { (name: String) in
                    Button {
                        model.baseBranch = name == model.defaultBranch ? "" : name
                    } label: {
                        if name == model.baseBranchLabel {
                            Label(name, systemImage: "checkmark")
                        } else {
                            Text(name)
                        }
                    }
                }
            } label: {
                HStack(spacing: FmSpace.x2) {
                    FmIcon("call_split", size: 18, color: fm.textTertiary)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(model.baseBranchLabel).fmText(.bodyMedium)
                        Text("Base branch: the agent branches from it, and merges back into it")
                            .fmText(.bodySmall, color: fm.textTertiary)
                    }
                    Spacer(minLength: 0)
                    FmIcon("expand_more", size: 18, color: fm.textTertiary)
                }
                .padding(.vertical, FmSpace.x1)
            }
            .accessibilityIdentifier("agent-start-base-branch")
        }
    }

    /// The model a run on this phone uses: a cloud provider or Gemma 4 here.
    @ViewBuilder private var deviceRow: some View {
        if model.effectiveMode == .device && model.canRunOnDevice {
            AgentTargetButton(
                icon: model.deviceRunsOnGemma ? "phone_iphone" : "auto_awesome_outlined",
                label: model.deviceModelLabel,
                detail: model.deviceCredentials == nil ? "Claude, OpenAI, or Gemma 4 on this iPhone" : "Model for runs on this device",
                onTap: onPickModel
            )
            if let readiness = model.gemmaReadiness, !readiness.isReady {
                GemmaReadinessNote(readiness: readiness, onOpenSettings: onOpenLocalAI)
            }
        }
    }

    @ViewBuilder private var cloneRow: some View {
        if model.effectiveMode == .device, let plan = model.plan, !plan.target.isMirrored, let repo = plan.target.repo, model.deviceRunsAvailable {
            CloneToDeviceButton(repo: repo)
        }
    }

    @ViewBuilder private var signIns: some View {
        if !model.signInProviders.isEmpty, let node = model.node {
            AgentChipRow {
                ForEach(model.signInProviders, id: \.self) { (provider: String) in
                    FmToneButton(label: "Sign in to \(agentProviderLabel(provider)) on \(node.signInNode)", icon: "key_outlined") {
                        onSignIn(node, provider)
                    }
                }
            }
            Text("Needed once per node, when its agent is not signed in yet.")
                .fmText(.bodySmall, color: fm.textTertiary)
        }
    }
}

/// Why Gemma 4 cannot run yet, with the way to fix it.
struct GemmaReadinessNote: View {
    @Environment(\.fmPalette) private var fm
    let readiness: OnDeviceAgentReadiness
    let onOpenSettings: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            AgentNote(text: readiness.message, color: fm.warning, icon: "memory_outlined")
            if readiness.canBeFixedInSettings {
                FmToneButton(label: "Open On-device AI", icon: "download_outlined", tone: .accent, onPressed: onOpenSettings)
            }
        }
    }
}

struct AgentNote: View {
    let text: String
    let color: Color
    let icon: String

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon(icon, size: 16, color: color)
            Text(text)
                .fmText(.bodySmall, color: color, height: 1.4)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmMessageTools(text)
        }
    }
}

/// The repository (or node) row, styled like a field control.
struct AgentTargetButton: View {
    @Environment(\.fmPalette) private var fm
    let icon: String
    let label: String
    let detail: String
    let onTap: (() -> Void)?

    private var face: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon(icon, size: 18, color: fm.textTertiary)
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                Text(label).fmText(.bodyMedium)
                if !detail.isEmpty {
                    Text(detail).fmText(.bodySmall, color: fm.textTertiary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            if onTap != nil {
                FmIcon("expand_more", size: 18, color: fm.textTertiary)
            }
        }
        .padding(FmSpace.x3)
        .background(fm.bgOverlay)
        .clipShape(FmRoundedShape(radius: FmRadius.md))
        .contentShape(Rectangle())
    }

    var body: some View {
        if let onTap = onTap {
            Button(action: onTap) { face }
                .buttonStyle(.plain)
        } else {
            face
        }
    }
}
