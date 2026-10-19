import FMApi
import FMBase
import SwiftUI

struct RunnersScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RunnersModel?
    @State private var accounts: RunnerAccountsModel?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        _model = State(initialValue: api.map { RunnersModel(api: $0) })
        _accounts = State(initialValue: Services.get(RunnerAccountsModel.self) ?? api.map { RunnerAccountsModel(api: $0) })
    }

    var body: some View {
        content
            .navigationTitle("Runners")
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let model = model, let accounts = accounts {
            RunnersContent(model: model, accounts: accounts)
                .task { await model.load() }
        } else {
            FmEmptyState(
                icon: "dns", title: "Runners are not available",
                message: "This phone is not connected to a ForkMesh server yet.")
        }
    }
}

private struct RunnersContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let model: RunnersModel
    let accounts: RunnerAccountsModel
    @State private var attention: RunnerAttentionModel? = Services.get(RunnerAttentionModel.self)
    @State private var signIn: RunnerSignInRequest?
    @State private var accountsFor: RunnerListEntry?
    @State private var revoking: RunnerListEntry?

    var body: some View {
        main
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    FmIconButton(icon: "add", label: "Add runner", onPressed: addRunner)
                        .accessibilityIdentifier("runners-add")
                }
            }
            .sheet(item: $signIn, onDismiss: reload) { (request: RunnerSignInRequest) in
                RunnerSignInSheet(model: accounts.loginModel(for: request))
            }
            .sheet(item: $accountsFor, onDismiss: reload) { (entry: RunnerListEntry) in
                RunnerAccountsSheet(model: accounts, attention: attention, only: entry.name)
            }
            .confirmationDialog(
                revoking.map { RunnerList.revokeTitle($0.name) } ?? "", isPresented: revokeShown,
                titleVisibility: .visible, presenting: revoking
            ) { (entry: RunnerListEntry) in
                Button("Revoke", role: .destructive) { run(.revoke, entry) }
                Button("Cancel", role: .cancel) {}
            } message: { (_: RunnerListEntry) in
                Text(RunnerList.revokeMessage)
            }
    }

    private var addRunner: (() -> Void)? {
        guard let url = model.addRunnerURL else { return nil }
        return { openURL(url) }
    }

    private var revokeShown: Binding<Bool> {
        Binding(get: { revoking != nil }, set: { if !$0 { revoking = nil } })
    }

    @ViewBuilder private var main: some View {
        if model.signedOut {
            message(
                icon: "lock_outline", title: "Sign in to see your runners",
                text: "Runners belong to the ForkMesh account that added them. Sign in to your account on this phone to see and manage yours.")
        } else if model.showsSpinner {
            FmSkeletonList()
        } else if model.showsError {
            message(icon: "error_outline", title: "Could not load runners", text: model.error)
        } else if model.showsEmpty {
            message(
                icon: "dns", title: "No runners yet",
                text: "A runner is a machine that runs agent and Actions jobs for your account. Add one on the website, then it shows here.",
                action: true)
        } else {
            TimelineView(.periodic(from: .now, by: 30)) { (context: TimelineViewDefaultContext) in
                list(nowMs: context.date.epochMillis)
            }
        }
    }

    private func list(nowMs: Int64) -> some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                FmNoticeLine(
                    model.message.isEmpty ? nil : model.message, isError: model.messageIsError,
                    onDismiss: { model.dismissMessage() })
                    .accessibilityIdentifier("runners-message")
                ForEach(model.runners) { (entry: RunnerListEntry) in
                    RunnerRow(
                        entry: entry, nowMs: nowMs, busy: model.isBusy(entry),
                        onSignIn: { (provider: String) in signIn = entry.signInRequest(provider) },
                        onAccounts: { accountsFor = entry },
                        onPause: { run(.pause, entry) },
                        onResume: { run(.resume, entry) },
                        onRevoke: { revoking = entry })
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(FmSpace.x4)
        }
        .fmRefreshable { await refresh() }
    }

    private func message(icon: String, title: String, text: String, action: Bool = false) -> some View {
        ScrollView {
            VStack(spacing: FmSpace.x3) {
                FmEmptyState(icon: icon, title: title, message: text)
                if action, let open = addRunner {
                    FmFilledButton("Add runner", icon: "open_in_new", onPressed: open)
                        .accessibilityIdentifier("runners-empty-add")
                }
            }
            .containerRelativeFrame([.horizontal, .vertical])
        }
        .fmRefreshable { await refresh() }
    }

    private func run(_ action: RunnerAction, _ entry: RunnerListEntry) {
        let current: RunnersModel = model
        Task { await current.perform(action, entry) }
    }

    private func refresh() async {
        await model.load()
        await accounts.load()
    }

    private func reload() {
        let current: RunnersModel = model
        let banner: RunnerAttentionModel? = attention
        Task {
            await banner?.refresh(force: true)
            await current.load()
        }
    }
}

private struct RunnerRow: View {
    @Environment(\.fmPalette) private var fm
    let entry: RunnerListEntry
    let nowMs: Int64
    let busy: Bool
    let onSignIn: (String) -> Void
    let onAccounts: () -> Void
    let onPause: () -> Void
    let onResume: () -> Void
    let onRevoke: () -> Void

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                header
                Text(meta)
                    .fmText(.bodySmall, color: fm.textSecondary)
                if !entry.revoked {
                    ForEach(entry.providers.filter { $0.signIn != .unknown }) { (provider: RunnerProviderState) in
                        providerLine(provider)
                    }
                    actions
                        .padding(.top, FmSpace.x1)
                }
            }
        }
        .opacity(entry.revoked ? 0.7 : 1)
        .accessibilityIdentifier("runner-row-\(entry.name)")
    }

    private var online: Bool { !entry.revoked && entry.online(nowMs: nowMs) }

    private var meta: String {
        [entry.scopeLabel, entry.seenText(nowMs: nowMs), entry.jobsText].filter { !$0.isEmpty }
            .joined(separator: RunnerAccounts.separator)
    }

    private var header: some View {
        HStack(spacing: FmSpace.x2) {
            Circle()
                .fill(online ? fm.success : fm.textTertiary)
                .frame(width: 8, height: 8)
                .accessibilityLabel(online ? "Online" : "Offline")
            Text(entry.name)
                .fmText(.titleSmall)
                .lineLimit(1)
                .truncationMode(.middle)
            if let label = entry.status.label {
                Text(label)
                    .fmText(.labelSmall, color: fm.textSecondary)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 2)
                    .background(Capsule().fill(fm.bgOverlay))
                    .accessibilityIdentifier("runner-status-\(entry.name)")
            }
            Spacer(minLength: 0)
            if busy {
                ProgressView().controlSize(.small)
            }
        }
    }

    private func providerLine(_ provider: RunnerProviderState) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
            Text(provider.label)
                .fmText(.labelMedium)
            Text(provider.detail)
                .fmText(.bodySmall, color: tone(provider.signIn))
                .lineLimit(2)
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
    }

    private func tone(_ state: RunnerProviderSignIn) -> Color {
        switch state {
        case .expired: return fm.danger
        case .expiring: return fm.warning
        case .signedIn: return fm.textSecondary
        case .signedOut, .notInstalled, .unknown: return fm.textTertiary
        }
    }

    private var actions: some View {
        ToolsFlowLayout(spacing: FmSpace.x2, lineSpacing: FmSpace.x2) {
            signInButton
            if entry.hasAccounts {
                FmToneButton(label: "Accounts", icon: "manage_accounts", onPressed: busy ? nil : onAccounts)
                    .accessibilityIdentifier("runner-accounts-\(entry.name)")
            }
            if entry.canPause {
                FmToneButton(label: RunnerAction.pause.label, icon: "pause", onPressed: busy ? nil : onPause)
                    .accessibilityIdentifier("runner-pause-\(entry.name)")
            }
            if entry.canResume {
                FmToneButton(label: RunnerAction.resume.label, icon: "play_arrow", onPressed: busy ? nil : onResume)
                    .accessibilityIdentifier("runner-resume-\(entry.name)")
            }
            if entry.canRevoke {
                FmToneButton(label: RunnerAction.revoke.label, icon: "block", tone: .danger, onPressed: busy ? nil : onRevoke)
                    .accessibilityIdentifier("runner-revoke-\(entry.name)")
            }
        }
    }

    @ViewBuilder private var signInButton: some View {
        let providers: [RunnerProviderState] = entry.signInProviders
        if providers.count == 1, let only = providers.first {
            FmToneButton(
                label: "Sign in", icon: "login", tone: .accent, tooltip: "Sign in \(only.label)",
                onPressed: busy ? nil : { onSignIn(only.provider) })
                .accessibilityIdentifier("runner-signin-\(entry.name)")
        } else if providers.count > 1 {
            Menu {
                ForEach(providers) { (provider: RunnerProviderState) in
                    Button("Sign in \(provider.label)") { onSignIn(provider.provider) }
                }
            } label: {
                HStack(spacing: 6) {
                    FmIcon("login", size: 16, color: fm.accent)
                    Text("Sign in")
                        .fmText(.labelMedium, color: fm.accent, height: 1)
                        .lineLimit(1)
                }
                .padding(.horizontal, 10)
                .frame(height: 34)
                .background(fm.accent.opacity(fm.dark ? 0.20 : 0.13))
                .clipShape(FmRoundedShape(radius: FmRadius.md))
            }
            .disabled(busy)
            .accessibilityIdentifier("runner-signin-\(entry.name)")
        }
    }
}
