import FMAgents
import FMApi
import FMBase
import SwiftUI

struct RunnerLogoutTarget: Identifiable {
    let runner: String
    let provider: String
    let account: RunnerAccount

    var id: String { "\(runner):\(provider):\(account.id)" }
}

struct RunnerAccountsSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let model: RunnerAccountsModel
    var attention: RunnerAttentionModel?
    var only: String?

    var body: some View {
        NavigationStack {
            ScrollView {
                RunnerAccountsPanel(model: model, attention: attention, only: only)
                    .padding(FmSpace.x4)
            }
            .refreshable { await model.load() }
            .fmPreGlassBackground(fm.bgBase)
            .navigationTitle(only.map { "\($0) accounts" } ?? "Runner accounts")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}

struct RunnerAccountsPanel: View {
    @Environment(\.fmPalette) private var fm
    let model: RunnerAccountsModel
    var attention: RunnerAttentionModel?
    var titled: Bool = false
    var only: String?
    @State private var signIn: RunnerSignInRequest?
    @State private var terminal: RunnerTerminalRequest?
    @State private var logout: RunnerLogoutTarget?

    var body: some View {
        TimelineView(.periodic(from: .now, by: 30)) { (context: TimelineViewDefaultContext) in
            content(nowMs: context.date.epochMillis)
        }
        .accessibilityIdentifier("runner-accounts-panel")
        .task { await model.load() }
        .sheet(item: $signIn, onDismiss: refreshAfterSignIn) { (request: RunnerSignInRequest) in
            RunnerSignInSheet(model: model.loginModel(for: request))
        }
        .fullScreenCover(item: $terminal, onDismiss: refreshAfterTerminal) { (request: RunnerTerminalRequest) in
            RunnerTerminalScreen(request: request)
        }
        .confirmationDialog(
            logoutTitle, isPresented: logoutShown, titleVisibility: .visible, presenting: logout
        ) { (target: RunnerLogoutTarget) in
            Button("Log out", role: .destructive) { logOut(target) }
            Button("Cancel", role: .cancel) {}
        } message: { (target: RunnerLogoutTarget) in
            Text(target.account.active
                ? "The runner deletes this saved login. The next saved account takes over its jobs."
                : "The runner deletes this saved login.")
        }
    }

    private var logoutTitle: String {
        guard let target = logout else { return "" }
        return "Log \(target.account.displayName) out of \(RunnerSignIn.providerLong(target.provider)) on \(target.runner)?"
    }

    private var logoutShown: Binding<Bool> {
        Binding(get: { logout != nil }, set: { if !$0 { logout = nil } })
    }

    private func logOut(_ target: RunnerLogoutTarget) {
        let current: RunnerAccountsModel = model
        Task { await current.remove(runner: target.runner, provider: target.provider, account: target.account) }
    }

    private func refreshAfterTerminal() {
        let current: RunnerAccountsModel = model
        Task { await current.load() }
    }

    private func refreshAfterSignIn() {
        let current: RunnerAccountsModel = model
        let banner: RunnerAttentionModel? = attention
        Task {
            await banner?.refresh(force: true)
            await current.load()
        }
    }

    private func content(nowMs: Int64) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            header
            if shown.isEmpty {
                Text(model.runners.isEmpty || !model.loaded ? model.emptyText : "This runner has no Claude Code or Codex installed.")
                    .fmText(.bodyMedium, color: fm.textSecondary)
            }
            ForEach(shown) { (runner: RunnerAccountsRunner) in
                RunnerAccountsCard(
                    model: model, runner: runner, nowMs: nowMs,
                    onSignIn: { (request: RunnerSignInRequest) in signIn = request },
                    onTerminal: { (request: RunnerTerminalRequest) in terminal = request },
                    onLogout: { (target: RunnerLogoutTarget) in logout = target })
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var shown: [RunnerAccountsRunner] {
        guard let only else { return model.runners }
        return model.runners.filter { $0.name == only }
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if titled {
                Text("Runner accounts")
                    .fmText(.titleSmall)
                    .accessibilityAddTraits(.isHeader)
            }
            HStack(alignment: .top, spacing: FmSpace.x2) {
                Text("Every Claude Code or Codex job on a runner runs as its checked account. Tap another account to switch; the runner applies it on its next check-in. Use ⋯ to log an account in again or out, or open a terminal.")
                    .fmText(.bodySmall, color: fm.textSecondary)
                Spacer(minLength: 0)
                if model.loading {
                    ProgressView().controlSize(.small)
                }
            }
            if !model.message.isEmpty {
                Text(model.message)
                    .fmText(.bodySmall, color: model.messageIsError ? fm.danger : fm.textSecondary)
                    .accessibilityIdentifier("runner-accounts-message")
                    .fmMessageTools(model.message)
            }
        }
    }
}

private struct RunnerAccountsCard: View {
    @Environment(\.fmPalette) private var fm
    let model: RunnerAccountsModel
    let runner: RunnerAccountsRunner
    let nowMs: Int64
    let onSignIn: (RunnerSignInRequest) -> Void
    let onTerminal: (RunnerTerminalRequest) -> Void
    let onLogout: (RunnerLogoutTarget) -> Void

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                HStack(spacing: FmSpace.x2) {
                    Image(systemName: "cpu")
                        .foregroundStyle(fm.accent)
                    Text(runner.name)
                        .fmText(.titleSmall)
                        .lineLimit(1)
                    Spacer(minLength: FmSpace.x2)
                    if !runner.scope.isEmpty {
                        Text(runner.scope)
                            .fmText(.labelSmall, color: fm.textSecondary)
                    }
                }
                ForEach(runner.providers) { (provider: RunnerProviderAccounts) in
                    providerBlock(provider)
                }
                if !runner.terminals && runner.providers.contains(where: \.binaryFound) {
                    Text("Terminals open here once \(runner.name) runs a newer runner build.")
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
            }
        }
        .accessibilityIdentifier("runner-accounts-\(runner.name)")
    }

    private func providerBlock(_ provider: RunnerProviderAccounts) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                Text(provider.label)
                    .fmText(.labelLarge)
                Spacer(minLength: FmSpace.x2)
                if canOpenTerminal(provider) {
                    FmToneButton(label: "Terminal", icon: "terminal_outlined", tone: .accent) {
                        onTerminal(terminalRequest(provider))
                    }
                    .accessibilityLabel(Text("Open a \(provider.label) terminal on \(runner.name)"))
                    .accessibilityIdentifier("runner-terminal-\(runner.name)-\(provider.provider)")
                }
            }
            ForEach(provider.accounts) { (account: RunnerAccount) in
                RunnerAccountRow(
                    account: account,
                    providerLabel: provider.label,
                    pending: model.isPending(runner, provider: provider.provider, account: account.id),
                    pendingText: runner.request(provider: provider.provider, account: account.id)
                        .map { $0.pending && $0.action == RunnerAccounts.removeAction } == true
                        ? "Logging out…" : "Switching…",
                    nowMs: nowMs,
                    canSignIn: provider.canSignIn,
                    onSwitch: { activate(provider.provider, account) },
                    onSignIn: { onSignIn(runner.signInRequest(provider.provider)) },
                    onLogout: { onLogout(RunnerLogoutTarget(runner: runner.name, provider: provider.provider, account: account)) },
                    onTerminal: terminalAction(provider, account))
                    .accessibilityIdentifier("runner-account-\(runner.name)-\(account.id)")
                if account.id != provider.accounts.last?.id {
                    FmHairline()
                }
            }
            if provider.canSignIn {
                signInButton(provider)
            } else {
                Text("\(provider.label) is not installed on this runner.")
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
        }
    }

    private func canOpenTerminal(_ provider: RunnerProviderAccounts) -> Bool {
        runner.terminals && provider.binaryFound
    }

    private func terminalAction(_ provider: RunnerProviderAccounts, _ account: RunnerAccount) -> (() -> Void)? {
        guard account.active, canOpenTerminal(provider) else { return nil }
        let request: RunnerTerminalRequest = terminalRequest(provider)
        let open: (RunnerTerminalRequest) -> Void = onTerminal
        return { open(request) }
    }

    private func terminalRequest(_ provider: RunnerProviderAccounts) -> RunnerTerminalRequest {
        RunnerTerminalRequest(
            runner: runner.name, provider: provider.provider,
            accountLabel: provider.activeAccount?.displayName ?? "",
            openId: runner.openTerminals[provider.provider] ?? "")
    }

    private func signInButton(_ provider: RunnerProviderAccounts) -> some View {
        Button {
            onSignIn(runner.signInRequest(provider.provider))
        } label: {
            HStack(spacing: FmSpace.x2) {
                Image(systemName: "plus.circle")
                    .foregroundStyle(fm.accent)
                Text(provider.signInLabel)
                    .fmText(.labelMedium, color: fm.accent)
                Spacer(minLength: 0)
            }
            .padding(.vertical, 4)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("runner-account-add-\(runner.name)-\(provider.provider)")
    }

    private func activate(_ provider: String, _ account: RunnerAccount) {
        if account.active { return }
        let current: RunnerAccountsModel = model
        let name: String = runner.name
        Task { await current.activate(runner: name, provider: provider, account: account) }
    }
}

private struct RunnerAccountRow: View {
    @Environment(\.fmPalette) private var fm
    let account: RunnerAccount
    let providerLabel: String
    let pending: Bool
    let pendingText: String
    let nowMs: Int64
    let canSignIn: Bool
    let onSwitch: () -> Void
    let onSignIn: () -> Void
    let onLogout: () -> Void
    let onTerminal: (() -> Void)?

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                Button(action: onSwitch) {
                    HStack(alignment: .top, spacing: FmSpace.x2) {
                        Image(systemName: account.active ? "checkmark.circle.fill" : "circle")
                            .foregroundStyle(account.active ? fm.success : fm.textTertiary)
                        details
                        Spacer(minLength: 0)
                    }
                    .padding(.vertical, 4)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(pending)
                .accessibilityElement(children: .combine)
                .accessibilityHint(Text(hint))
                .accessibilityAddTraits(account.active ? .isSelected : [])
                if pending {
                    ProgressView().controlSize(.small)
                        .padding(.top, 6)
                }
                menu
            }
            ForEach(account.usage) { (window: RunnerUsageWindow) in
                UsageMeterRow(window: UsageWindowMeter(runner: window), nowMs: nowMs)
            }
            Text(RunnerAccounts.usageNote(account, nowMs: nowMs))
                .fmText(.bodySmall, color: account.usageError.isEmpty ? fm.textTertiary : fm.warning)
                .fmMessageTools(account.usageError.isEmpty ? nil : RunnerAccounts.usageNote(account, nowMs: nowMs))
        }
    }

    private var hint: String {
        if pending { return "A change to this account is waiting for the runner" }
        return account.active ? "Jobs on this runner use this account" : "Switches this runner to this account"
    }

    private var menu: some View {
        Menu {
            if !account.active {
                Button(action: onSwitch) {
                    Label("Use this account", systemImage: "checkmark.circle")
                }
            }
            if let onTerminal = onTerminal {
                Button(action: onTerminal) {
                    Label("Open \(providerLabel) terminal", systemImage: "apple.terminal")
                }
            }
            if canSignIn {
                Button(action: onSignIn) {
                    Label("Log in again…", systemImage: "person.badge.key")
                }
            }
            Button(role: .destructive, action: onLogout) {
                Label("Log out…", systemImage: "rectangle.portrait.and.arrow.right")
            }
        } label: {
            Image(systemName: "ellipsis.circle")
                .font(.system(size: 17))
                .foregroundStyle(fm.textSecondary)
                .frame(width: 36, height: 36)
        }
        .disabled(pending)
        .accessibilityLabel(Text("\(account.displayName) actions"))
    }

    private var details: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text(account.displayName)
                    .fmText(.bodyMedium)
                    .lineLimit(1)
                if !account.plan.isEmpty {
                    Text(account.plan)
                        .fmText(.labelSmall, color: fm.textSecondary)
                }
            }
            if pending {
                Text(pendingText)
                    .fmText(.labelSmall, color: fm.warning)
                    .fmMessageTools(pendingText)
            }
        }
    }
}
