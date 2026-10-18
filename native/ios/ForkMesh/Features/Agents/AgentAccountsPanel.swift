import FMAgents
import FMApi
import FMBase
import FMControl
import SwiftUI

enum AgentAccountsRoute: Hashable, Identifiable {
    case terminal(provider: String, account: String, label: String)
    case signIn(mode: String, provider: String, account: String, label: String)

    var id: String {
        switch self {
        case .terminal(let provider, let account, _): return "terminal:\(provider):\(account)"
        case .signIn(let mode, let provider, let account, _): return "\(mode):\(provider):\(account)"
        }
    }
}

struct AgentAccountsPanel: View {
    @Environment(\.fmPalette) private var fm
    let usage: AgentUsageModel
    let runnerAccounts: RunnerAccountsModel?
    @State private var route: AgentAccountsRoute?
    @State private var terminals: Bool = false
    @State private var accountTerminals: Bool = false
    @State private var accountActions: Bool = false
    @State private var notice: FmNotice?
    @State private var noticeProvider: String = ""
    @State private var busy: Bool = false
    @State private var addingProvider: String = ""
    @State private var adding: Bool = false
    @State private var renameTarget: (provider: String, account: UsageAccount)?
    @State private var renaming: Bool = false
    @State private var draft: String = ""
    @State private var deleteTarget: (provider: String, account: UsageAccount)?
    @State private var deleting: Bool = false

    private var center: AgentControlCenter? { HostsFeature.agents }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                desktopLine
                content
                if let runnerAccounts = runnerAccounts, runnerAccounts.visible {
                    RunnerAccountsPanel(
                        model: runnerAccounts, attention: Services.get(RunnerAttentionModel.self), titled: true)
                        .padding(.top, FmSpace.x2)
                }
            }
            .padding(FmSpace.x4)
        }
        .fmPreGlassBackground(fm.bgBase)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                if usage.loading || busy {
                    ProgressView().controlSize(.small)
                } else {
                    FmIconButton(icon: "refresh", label: "Refresh", onPressed: {
                        Task { await refresh() }
                    })
                    .accessibilityIdentifier("usage-refresh")
                }
            }
        }
        .navigationDestination(item: $route) { (route: AgentAccountsRoute) in
            destination(route)
                .shellPageClearance()
        }
        .fmRefreshable { await refresh() }
        .alert("Add a \(agentProviderLabel(addingProvider)) account", isPresented: $adding) {
            TextField("Account name", text: $draft)
                .textInputAutocapitalization(.words)
            Button("Add and sign in") { add() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The desktop keeps a separate sign-in for it, then opens that sign-in here.")
        }
        .alert("Rename account", isPresented: $renaming) {
            TextField("Account name", text: $draft)
                .textInputAutocapitalization(.words)
            Button("Rename") { rename() }
            Button("Cancel", role: .cancel) {}
        }
        .confirmationDialog(
            "Delete the \(agentProviderLabel(deleteTarget?.provider ?? "")) account \"\(deleteTarget?.account.label ?? "")\"?",
            isPresented: $deleting, titleVisibility: .visible
        ) {
            Button("Delete account", role: .destructive) { delete() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The desktop forgets the account and signs it out. Agents running as it stop being able to sign in. Its agent history stays on the desktop.")
        }
        .onChange(of: route) { (previous: AgentAccountsRoute?, next: AgentAccountsRoute?) in
            if previous != nil && next == nil {
                Task { await usage.refresh() }
            }
        }
        .task {
            await refresh()
            await readCapabilities()
            await runnerAccounts?.refresh()
        }
    }

    @ViewBuilder private var desktopLine: some View {
        if let hub = HostsFeature.hub, hub.desktop != nil {
            Label(hub.reach.label, systemImage: "desktopcomputer")
                .fmText(.bodySmall, color: hub.reach.isConnected ? fm.textSecondary : fm.warning)
                .accessibilityIdentifier("usage-desktop")
        }
    }

    @ViewBuilder private var content: some View {
        if let summary = usage.summary {
            if summary.providers.isEmpty {
                Text("The desktop reports no Claude Code or Codex accounts yet.")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            ForEach(summary.providers) { (provider: UsageProviderMeter) in
                providerCard(provider)
            }
            if let total = summary.totalSpendUsd {
                Text("API spend this month: \(UsageFormat.usd(total))")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            if !accountActions && summary.providers.contains(where: { !$0.accounts.isEmpty }) {
                Text("Update ForkMesh on the desktop to switch, add, rename, sign in to or sign out of accounts from here.")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } else if usage.unsupported {
            Text("Update ForkMesh on the desktop to see its usage here.")
                .fmText(.bodySmall, color: fm.textSecondary)
        } else if !usage.errorText.isEmpty {
            Text(usage.errorText)
                .fmText(.bodySmall, color: fm.danger)
                .fmMessageTools(usage.errorText)
        } else {
            FmLinearProgressIndicator()
        }
    }

    private func providerCard(_ provider: UsageProviderMeter) -> some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                HStack(spacing: FmSpace.x2) {
                    Text(provider.label)
                        .fmText(.titleSmall)
                    if let summary = usage.summary, summary.providers.count > 1, summary.activeProvider == provider.id {
                        Text("Used for new agents")
                            .fmText(.labelSmall, color: fm.textSecondary)
                    }
                    Spacer(minLength: FmSpace.x2)
                    if terminals && center != nil {
                        FmToneButton(label: "New terminal", icon: "add", tone: .accent, filled: true) {
                            route = .terminal(
                                provider: provider.id, account: "", label: provider.activeAccount?.label ?? "")
                        }
                        .accessibilityIdentifier("accounts-new-terminal-\(provider.id)")
                    }
                }
                if provider.accounts.isEmpty {
                    ForEach(provider.windows) { (window: UsageWindowMeter) in
                        UsageMeterRow(window: window, nowMs: usage.nowMs)
                    }
                } else {
                    ForEach(provider.accounts) { (account: UsageAccount) in
                        accountRow(provider, account)
                        if account.id != provider.accounts.last?.id {
                            FmHairline()
                        }
                    }
                }
                if provider.credits != nil || provider.id == UsageCreditsRow.provider {
                    UsageCreditsRow(credits: provider.credits)
                }
                if accountActions {
                    FmTextButton("Add account", icon: "add") {
                        addingProvider = provider.id
                        draft = ""
                        adding = true
                    }
                    .accessibilityIdentifier("accounts-add-\(provider.id)")
                }
                if noticeProvider == provider.id, notice != nil {
                    FmNoticeLine($notice)
                }
            }
        }
    }

    private func accountRow(_ provider: UsageProviderMeter, _ account: UsageAccount) -> some View {
        let windows: [UsageWindowMeter] = account.active ? provider.windows : account.windows
        return VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                Button {
                    select(provider, account)
                } label: {
                    HStack(spacing: FmSpace.x2) {
                        Image(systemName: account.active ? "largecircle.fill.circle" : "circle")
                            .font(.system(size: 18))
                            .foregroundStyle(account.active ? fm.accent : fm.textTertiary)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(account.label)
                                .fmText(.labelLarge)
                                .lineLimit(1)
                            Text(subtitle(account))
                                .fmText(.bodySmall, color: account.signedIn == false ? fm.warning : fm.textSecondary)
                                .lineLimit(1)
                        }
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .disabled(account.active || !accountActions || busy)
                .accessibilityLabel(Text("\(account.label), \(subtitle(account))"))
                .accessibilityHint(Text(account.active ? "" : "Use this account for new agents"))
                Spacer(minLength: FmSpace.x2)
                if accountTerminals && center != nil {
                    Button {
                        route = .terminal(provider: provider.id, account: account.id, label: account.label)
                    } label: {
                        Image(systemName: "apple.terminal")
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(fm.accent)
                            .frame(width: 36, height: 36)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(Text("New \(provider.label) terminal as \(account.label)"))
                }
                accountMenu(provider, account)
            }
            ForEach(windows) { (window: UsageWindowMeter) in
                UsageMeterRow(window: window, nowMs: usage.nowMs)
            }
        }
    }

    @ViewBuilder private func accountMenu(_ provider: UsageProviderMeter, _ account: UsageAccount) -> some View {
        if accountActions || accountTerminals {
            Menu {
                if accountActions && !account.active {
                    Button {
                        select(provider, account)
                    } label: {
                        Label("Use for new agents", systemImage: "checkmark.circle")
                    }
                }
                if accountTerminals && center != nil {
                    Button {
                        route = .terminal(provider: provider.id, account: account.id, label: account.label)
                    } label: {
                        Label("New terminal as this account", systemImage: "apple.terminal")
                    }
                    Button {
                        route = .signIn(mode: "login", provider: provider.id, account: account.id, label: account.label)
                    } label: {
                        Label("Log in…", systemImage: "person.badge.key")
                    }
                    Button(role: .destructive) {
                        route = .signIn(mode: "logout", provider: provider.id, account: account.id, label: account.label)
                    } label: {
                        Label("Log out…", systemImage: "rectangle.portrait.and.arrow.right")
                    }
                }
                if accountActions {
                    Button {
                        renameTarget = (provider.id, account)
                        draft = account.label
                        renaming = true
                    } label: {
                        Label("Rename…", systemImage: "pencil")
                    }
                }
                if accountActions && account.id != "default" {
                    Button(role: .destructive) {
                        deleteTarget = (provider.id, account)
                        deleting = true
                    } label: {
                        Label("Delete…", systemImage: "trash")
                    }
                }
            } label: {
                Image(systemName: "ellipsis.circle")
                    .font(.system(size: 17))
                    .foregroundStyle(fm.textSecondary)
                    .frame(width: 36, height: 36)
            }
            .accessibilityLabel(Text("\(account.label) actions"))
        }
    }

    private func subtitle(_ account: UsageAccount) -> String {
        var parts: [String] = []
        if !account.identity.isEmpty {
            parts.append(account.identity)
        }
        if account.signedIn == false {
            parts.append("Not signed in")
        }
        if account.active {
            parts.append("Used for new agents")
        }
        return parts.isEmpty ? "Signed in" : parts.joined(separator: " · ")
    }

    @ViewBuilder private func destination(_ route: AgentAccountsRoute) -> some View {
        if let center = center {
            switch route {
            case .terminal(let provider, let account, let label):
                DesktopTerminalScreen(center: center, provider: provider, account: account, accountLabel: label)
            case .signIn(let mode, let provider, let account, let label):
                DesktopTerminalScreen(
                    center: center, signIn: mode, provider: provider, account: account, accountLabel: label)
            }
        } else {
            FmEmptyState(
                icon: "computer",
                title: "No desktop connected",
                message: "Pair this phone with the desktop to open terminals on it."
            )
        }
    }

    private func refresh() async {
        await usage.refresh()
        await AgentOpsFeature.scheduleUsageReminders(usage.reminders, nowMs: usage.nowMs)
    }

    private func readCapabilities() async {
        guard let center = center else {
            return
        }
        terminals = await DesktopTerminalSupport.check(center.hub) ?? false
        accountTerminals = await DesktopTerminalSupport.check(center.hub, DesktopTerminalSupport.accountsCapability) ?? false
        accountActions = await DesktopTerminalSupport.check(center.hub, "agentops.accounts") ?? false
    }

    private func select(_ provider: UsageProviderMeter, _ account: UsageAccount) {
        if account.active || busy {
            return
        }
        busy = true
        noticeProvider = provider.id
        notice = nil
        Task {
            let error: String = await usage.selectAccount(provider: provider.id, account: account.id)
            busy = false
            if error.isEmpty {
                FmHaptics.tap()
                notice = FmNotice("New \(provider.label) agents use \(account.label)")
            } else {
                notice = FmNotice.error(error)
            }
        }
    }

    private func add() {
        let label: String = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        let provider: String = addingProvider
        if label.isEmpty || busy {
            return
        }
        busy = true
        noticeProvider = provider
        notice = nil
        Task {
            let result = await usage.addAccount(provider: provider, label: label)
            busy = false
            if !result.error.isEmpty {
                notice = FmNotice.error(result.error)
                return
            }
            if accountTerminals && center != nil && !result.account.isEmpty {
                route = .signIn(mode: "login", provider: provider, account: result.account, label: label)
            } else {
                notice = FmNotice("Added \(label). Sign it in on the desktop.")
            }
        }
    }

    private func delete() {
        guard let target = deleteTarget, !busy else {
            return
        }
        busy = true
        noticeProvider = target.provider
        notice = nil
        Task {
            let error: String = await usage.deleteAccount(provider: target.provider, account: target.account.id)
            busy = false
            deleteTarget = nil
            notice = error.isEmpty ? FmNotice("Deleted \(target.account.label)") : FmNotice.error(error)
        }
    }

    private func rename() {
        let label: String = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let target = renameTarget, !label.isEmpty, label != target.account.label, !busy else {
            return
        }
        busy = true
        noticeProvider = target.provider
        notice = nil
        Task {
            let error: String = await usage.renameAccount(provider: target.provider, account: target.account.id, label: label)
            busy = false
            notice = error.isEmpty ? FmNotice("Renamed to \(label)") : FmNotice.error(error)
        }
    }
}
