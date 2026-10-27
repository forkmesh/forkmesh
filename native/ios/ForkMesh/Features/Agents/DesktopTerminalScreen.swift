import FMAgents
import FMBase
import FMChat
import FMControl
import SwiftUI
import UIKit

struct DesktopTerminalScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let center: AgentControlCenter
    let attach: String
    let session: Int
    let mode: String
    let account: String
    let accountLabel: String
    let fixedProvider: Bool
    @State private var terminal: DesktopAgentTerminal?
    @State private var signInLink: String = ""
    @State private var repo: String = ""
    @State private var provider: String = "claude-code"
    @State private var input: String = ""
    @State private var ctrl: Bool = false
    @State private var notice: FmNotice?
    @State private var confirmEnd: Bool = false
    @State private var supported: Bool?
    @State private var fullAccess: Bool = false
    @State private var autoStarted: Bool = false

    init(
        center: AgentControlCenter, terminal attach: String = "", desktopKey: String = "", provider: String = "",
        account: String = "", accountLabel: String = ""
    ) {
        self.center = center
        self.attach = attach
        self.session = 0
        self.mode = ""
        self.account = account
        self.accountLabel = accountLabel
        self.fixedProvider = !provider.isEmpty
        _provider = State(initialValue: provider.isEmpty ? "claude-code" : provider)
        _terminal = State(initialValue: attach.isEmpty ? nil : DesktopAgentTerminal(
            channel: center.hub.terminalChannel(for: desktopKey), terminal: attach))
    }

    init(center: AgentControlCenter, signIn mode: String, provider: String, account: String, accountLabel: String) {
        self.center = center
        self.attach = ""
        self.session = 0
        self.mode = mode
        self.account = account
        self.accountLabel = accountLabel
        self.fixedProvider = true
        _provider = State(initialValue: provider)
        _terminal = State(initialValue: DesktopAgentTerminal(
            channel: center.hub, owner: "", name: "", provider: provider, mode: mode, account: account))
    }

    init(center: AgentControlCenter, row: UnifiedAgentRow) {
        self.center = center
        self.attach = ""
        self.mode = ""
        self.account = ""
        self.accountLabel = ""
        self.fixedProvider = true
        self.session = row.localId
        _terminal = State(initialValue: DesktopAgentTerminal(
            channel: center.hub.terminalChannel(for: row.desktopKey), session: row.localId, owner: row.repoOwner,
            name: row.repoName,
            provider: row.provider))
    }

    private var title: String {
        if let terminal = terminal {
            return terminal.taskLabel
        }
        return "Agent in a terminal"
    }

    var body: some View {
        content
            .background(fm.canvas)
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { toolbarItems }
            .confirmationDialog("End \(terminal?.taskLabel ?? "the agent")?", isPresented: $confirmEnd, titleVisibility: .visible) {
                Button("End", role: .destructive) {
                    end()
                }
            } message: {
                Text("The desktop closes its terminal, which stops the agent. Leaving this screen instead keeps it running.")
            }
            .onAppear {
                terminal?.reattach()
            }
            .onDisappear {
                terminal?.detach()
            }
            .onChange(of: terminal?.terminalId ?? "") { (_: String, id: String) in
                if !id.isEmpty && attach.isEmpty && session == 0 && mode.isEmpty {
                    Task { await center.refreshDesktop() }
                }
            }
            .onChange(of: terminal?.revision ?? 0) { (_: Int, _: Int) in
                if let terminal = terminal, terminal.isSignIn, let link = terminal.screen.links().last {
                    signInLink = link
                }
            }
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItem(placement: .principal) {
            VStack(spacing: 0) {
                Text(title).fmText(.titleSmall).lineLimit(1)
                if let terminal = terminal, !terminal.repoLabel.isEmpty {
                    Text(terminal.repoLabel).fmText(.bodySmall, color: fm.textTertiary).lineLimit(1)
                }
            }
        }
        ToolbarItemGroup(placement: .topBarTrailing) {
            if let terminal = terminal {
                Button {
                    copy(terminal.screen.allRows.map { $0.text }.joined(separator: "\n").dartTrimmed)
                } label: {
                    FmIcon("copy_all_outlined", size: 22, color: fm.textSecondary)
                }
                .accessibilityLabel(Text("Copy terminal text"))
                if !terminal.closed && !terminal.terminalId.isEmpty {
                    Button(role: .destructive) {
                        confirmEnd = true
                    } label: {
                        if terminal.ending {
                            ProgressView()
                        } else {
                            Text("End")
                        }
                    }
                    .disabled(terminal.ending)
                    .accessibilityIdentifier("desktop-terminal-end")
                }
            }
        }
    }

    @ViewBuilder private var content: some View {
        if let terminal = terminal {
            VStack(spacing: 0) {
                TerminalStatusLine(terminal: terminal, onRestart: canStartAgain(terminal) ? { startAgain() } : nil, notice: $notice)
                if !signInLink.isEmpty {
                    TerminalLinkBar(
                        url: signInLink,
                        onCopy: { copy(signInLink, what: "Link") },
                        onOpen: {
                            if let url = URL(string: signInLink) {
                                openURL(url)
                            }
                        }
                    )
                }
                TerminalGridView(terminal: terminal)
                    .id(ObjectIdentifier(terminal))
                TerminalKeyBar(terminal: terminal, ctrl: $ctrl, onPaste: {
                    if let text = UIPasteboard.general.string, !text.isEmpty {
                        terminal.paste(text)
                    }
                })
                inputLine
            }
        } else {
            startForm
        }
    }

    private func canStartAgain(_ terminal: DesktopAgentTerminal) -> Bool {
        if !attach.isEmpty || session > 0 {
            return false
        }
        return terminal.closed || (terminal.terminalId.isEmpty && terminal.statusLevel == .error)
    }

    private func startAgain() {
        terminal?.detach()
        signInLink = ""
        if mode.isEmpty {
            terminal = nil
        } else {
            terminal = DesktopAgentTerminal(
                channel: center.hub, owner: "", name: "", provider: provider, mode: mode, account: account)
        }
    }

    private var repositories: [LocalAgentRepository] {
        center.desktopSnapshot?.repositories ?? []
    }

    private var selectedRepo: String {
        if repositories.contains(where: { $0.label == repo }) {
            return repo
        }
        return repositories.first?.label ?? ""
    }

    private var repoBinding: Binding<String> {
        Binding(get: { selectedRepo }, set: { repo = $0 })
    }

    private var startForm: some View {
        ScrollView {
            FmCard(radius: FmRadius.lg) {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    Text("AGENT IN A DESKTOP TERMINAL").fmText(.labelSmall, color: fm.textTertiary)
                    Text("The desktop opens \(fixedProvider ? agentProviderLabel(provider) : "Claude Code or Codex") in a terminal on one of its repositories. You watch and type into it here, and it is listed with your agents. Leaving this screen keeps it running.")
                        .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                        .fixedSize(horizontal: false, vertical: true)
                    if fullAccess {
                        Text("It runs with full permissions: Claude Code skips every permission prompt, and Codex runs without approvals or a sandbox.")
                            .fmText(.bodySmall, color: fm.warning, height: 1.35)
                            .fixedSize(horizontal: false, vertical: true)
                            .accessibilityIdentifier("desktop-terminal-full-permissions")
                    }
                    if !accountLabel.isEmpty {
                        Text("Signed in as the \(agentProviderLabel(provider)) account \(accountLabel).")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    if supported == false {
                        Text(ControlErrorText.describe(code: "unsupported"))
                            .fmText(.bodySmall, color: fm.warning)
                            .fmMessageTools(ControlErrorText.describe(code: "unsupported"))
                    }
                    if repositories.isEmpty {
                        if center.loading {
                            FmLinearProgressIndicator()
                        } else {
                            Text(center.desktopNote.isEmpty
                                 ? "The desktop has no repository open, so it has nowhere to start an agent."
                                 : center.desktopNote)
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .fmMessageTools(center.desktopNote.isEmpty
                                    ? "The desktop has no repository open, so it has nowhere to start an agent."
                                    : center.desktopNote)
                        }
                    } else {
                        Picker("Repository", selection: repoBinding) {
                            ForEach(repositories) { (item: LocalAgentRepository) in
                                Text(item.label).tag(item.label)
                            }
                        }
                        .accessibilityIdentifier("desktop-terminal-repository")
                        if !fixedProvider {
                            agentPicker
                        }
                        HStack {
                            Spacer()
                            FmFilledButton("Start", icon: "play_arrow", onPressed: supported == false ? nil : {
                                start()
                            })
                            .accessibilityIdentifier("desktop-terminal-start")
                        }
                    }
                }
            }
            .padding(FmSpace.x3)
        }
        .fmRefreshable {
            await center.refreshDesktop()
        }
        .task {
            if center.desktopSnapshot == nil {
                await center.refreshDesktop()
            }
            supported = await DesktopTerminalSupport.check(center.hub)
            fullAccess = await DesktopTerminalSupport.check(center.hub, DesktopTerminalSupport.accountsCapability) ?? false
            if fixedProvider && !autoStarted && supported != false && repositories.count == 1 {
                autoStarted = true
                start()
            }
        }
    }

    private var agentPicker: some View {
        Picker("Agent", selection: $provider) {
            ForEach(agentLoginProviders.map { $0.id }, id: \.self) { (id: String) in
                Text(agentProviderLabel(id)).tag(id)
            }
        }
        .pickerStyle(.segmented)
        .accessibilityIdentifier("desktop-terminal-provider")
    }

    private func start() {
        guard let picked = repositories.first(where: { $0.label == selectedRepo }) else {
            return
        }
        FmHaptics.tap()
        terminal = DesktopAgentTerminal(
            channel: center.hub, owner: picked.owner, name: picked.name, provider: provider, account: account)
    }

    private func end() {
        guard let terminal = terminal else {
            return
        }
        Task {
            await terminal.end()
            if terminal.closed {
                notice = FmNotice("\(terminal.taskLabel) ended")
                await center.refreshDesktop()
            }
        }
    }

    private func copy(_ text: String, what: String = "Terminal text") {
        UIPasteboard.general.string = text
        notice = FmNotice("\(what) copied")
    }

    private var inputLine: some View {
        HStack(spacing: FmSpace.x2) {
            TextField("Type or paste, then send (sends Enter)", text: $input)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .fmMono(size: FmType.small, color: fm.textPrimary)
                .submitLabel(.send)
                .onSubmit { submitLine() }
                .terminalChords(text: $input, ctrl: $ctrl, terminal: terminal)
                .accessibilityIdentifier("desktop-terminal-input")
            Button {
                submitLine()
            } label: {
                FmIcon("keyboard_return", size: 22, color: fm.accent)
                    .frame(width: 44, height: 44)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Send with Enter"))
        }
        .padding(EdgeInsets.fmLTRB(FmSpace.x3, 0, FmSpace.x3, FmSpace.x3))
    }

    private func submitLine() {
        guard let terminal = terminal else {
            return
        }
        let text: String = input
        input = ""
        terminal.paste(text)
        if text.isEmpty {
            terminal.key(.enter)
        } else {
            Task {
                try? await Task.sleep(nanoseconds: 120_000_000)
                if !terminal.closed {
                    terminal.key(.enter)
                }
            }
        }
    }
}
