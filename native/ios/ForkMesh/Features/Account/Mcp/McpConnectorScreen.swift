import FMAccount
import FMApi
import FMBase
import SwiftUI
import UIKit

@MainActor
struct McpConnectorScreen: View {
    @Environment(\.fmPalette) var fm
    @State var orgs: [OrgSummary] = []
    @State var loading: Bool = true
    @State var failure: String = ""
    @State var opened: String?

    private let api: ApiService?

    init() {
        api = Services.get(ApiService.self)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                SettingsSection(title: "Remote MCP server") {
                    SettingsCardBox {
                        VStack(alignment: .leading, spacing: FmSpace.x2) {
                            CopyableValueRow(label: "Server URL", value: McpConnector.serverURL(origin: api?.serverOrigin ?? ""))
                            SettingsNote(text: "Claude Code, Codex and other MCP clients reach your organization's tasks here with a revocable token. Each token can read and update tasks, nothing else.")
                        }
                    }
                }
                SettingsSection(title: "Organizations", last: true) {
                    SettingsCardBox(padded: false) { orgList }
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationTitle("MCP connector")
        .navigationBarTitleDisplayMode(.inline)
        .task { await load() }
        .fmRefreshable { await load() }
        .navigationDestination(item: $opened) { (org: String) in
            if let api = api {
                McpTokensScreen(model: McpConnectorModel(org: org, client: api))
                    .shellPageClearance()
            }
        }
    }

    @ViewBuilder private var orgList: some View {
        if loading && orgs.isEmpty {
            FmSkeletonList(rows: 2, label: "Loading organizations")
        } else if orgs.isEmpty {
            SettingsEmptyNote(
                title: failure.isEmpty ? "No organizations" : "Organizations unavailable",
                text: failure.isEmpty ? "MCP tokens belong to an organization. Create one in Settings > Organizations." : failure)
                .padding(FmSpace.x4)
        } else {
            VStack(spacing: 0) {
                ForEach(Array(orgs.enumerated()), id: \.element.name) { (index: Int, org: OrgSummary) in
                    if index > 0 { FmHairline() }
                    FmListRow(
                        title: org.name,
                        subtitle: org.role.isEmpty ? "member" : org.role,
                        leading: FmIcon("apartment_outlined", size: 22, color: fm.textSecondary).fmAny,
                        onTap: { opened = org.name })
                }
            }
        }
    }

    private func load() async {
        guard let api = api else {
            loading = false
            failure = "Sign in to manage MCP tokens."
            return
        }
        loading = true
        defer { loading = false }
        do {
            orgs = try await api.myOrgs()
            failure = ""
        } catch {
            failure = "Could not load your organizations."
        }
    }
}

@MainActor
struct McpTokensScreen: View {
    @Environment(\.fmPalette) var fm
    @State var model: McpConnectorModel
    @State var confirmRevoke: McpTokenRow?
    @State var tokensNotice: FmNotice?
    @State var mintedNotice: FmNotice?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if let minted = model.minted {
                    SettingsSection(title: "New token (shown once)") {
                        SettingsCardBox {
                            VStack(alignment: .leading, spacing: FmSpace.x3) {
                                MintedTokenCard(minted: minted, serverURL: model.serverURL, onDone: {
                                    mintedNotice = nil
                                    model.dismissMinted()
                                }, onCopied: copied)
                                FmNoticeLine($mintedNotice)
                            }
                        }
                    }
                }
                SettingsSection(title: "Tokens") {
                    SettingsCardBox(padded: false) {
                        VStack(alignment: .leading, spacing: 0) {
                            tokenList
                            if tokensNotice != nil {
                                FmHairline()
                                FmNoticeLine($tokensNotice)
                                    .padding(.horizontal, FmSpace.x4)
                                    .padding(.vertical, FmSpace.x3)
                            }
                        }
                    }
                }
                SettingsSection(title: "Create a token", last: true) {
                    SettingsCardBox { mintForm }
                }
            }
            .padding(FmSpace.x4)
        }
        .background(fm.bgBase.ignoresSafeArea())
        .navigationTitle(model.org)
        .navigationBarTitleDisplayMode(.inline)
        .task { await model.load() }
        .fmRefreshable { await model.load() }
        .fmDialog(isPresented: revokeShown) {
            if let row = confirmRevoke {
                FmConfirmDialog(
                    title: "Revoke \(row.title)?",
                    message: "Any agent using this token loses access at once.",
                    confirmLabel: "Revoke",
                    destructive: true,
                    onCancel: { confirmRevoke = nil },
                    onConfirm: {
                        confirmRevoke = nil
                        Task { @MainActor in
                            if await model.revoke(row.id) { tokensNotice = FmNotice("Token revoked") }
                        }
                    })
            }
        }
    }

    private var revokeShown: Binding<Bool> {
        Binding<Bool>(get: { confirmRevoke != nil }, set: { (shown: Bool) in if !shown { confirmRevoke = nil } })
    }

    private func copied(_ what: String) {
        mintedNotice = FmNotice("\(what) copied")
    }

    @ViewBuilder private var tokenList: some View {
        let now: Int64 = SystemClock().nowMillis
        if model.loading && model.tokens.isEmpty {
            FmSkeletonList(rows: 2, label: "Loading tokens")
        } else if model.tokens.isEmpty {
            SettingsEmptyNote(
                title: model.error.isEmpty ? "No tokens" : "Tokens unavailable",
                text: model.error.isEmpty ? "Create one below and paste it into your MCP client." : model.error)
                .fmMessageTools(model.error, alignment: .top)
                .padding(FmSpace.x4)
        } else {
            VStack(spacing: 0) {
                ForEach(Array(model.tokens.enumerated()), id: \.element.id) { (index: Int, row: McpTokenRow) in
                    if index > 0 { FmHairline() }
                    tokenRow(row, now: now)
                }
            }
        }
    }

    private func tokenRow(_ row: McpTokenRow, now: Int64) -> some View {
        let active: Bool = row.isActive(nowMillis: now)
        return HStack(spacing: FmSpace.x3) {
            FmIcon("hub_outlined", size: 22, color: active ? fm.textSecondary : fm.textDisabled)
            VStack(alignment: .leading, spacing: 2) {
                Text(row.title).fmText(.titleSmall).lineLimit(1)
                Text("\(McpConnector.providerLabel(row.provider)) · \(row.status(nowMillis: now)) · used \(AccountTime.label(row.lastUsedAt, nowMillis: now))")
                    .fmText(.bodySmall, color: fm.textTertiary)
                    .lineLimit(2)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            if active {
                Menu {
                    Button("Rotate") {
                        Task { @MainActor in
                            if await model.rotate(row) != nil { tokensNotice = FmNotice("New token made; the old one is revoked") }
                        }
                    }
                    Button("Revoke", role: .destructive) { confirmRevoke = row }
                } label: {
                    FmIcon("more_vert", size: 20, color: fm.textTertiary)
                        .frame(width: 44, height: 44)
                }
                .accessibilityLabel(Text("Token actions"))
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .frame(minHeight: 60)
    }

    private var mintForm: some View {
        VStack(alignment: .leading, spacing: 0) {
            Picker("Client", selection: $model.provider) {
                ForEach(McpConnector.providers, id: \.self) { (provider: String) in
                    Text(McpConnector.providerLabel(provider)).tag(provider)
                }
            }
            .pickerStyle(.segmented)
            .padding(.bottom, FmSpace.x3)
            SettingsTextField(label: "Label (optional)", text: $model.label)
            Stepper(value: $model.days, in: 1...McpConnector.maxDays, step: 30) {
                Text("Expires after \(model.days) day\(model.days == 1 ? "" : "s")").fmText(.bodyMedium)
            }
            .padding(.bottom, FmSpace.x3)
            if !model.error.isEmpty && !model.tokens.isEmpty {
                SettingsNote(text: model.error, color: fm.danger)
                    .fmMessageTools(model.error)
                    .padding(.bottom, FmSpace.x3)
            }
            FmFilledButton(model.busy ? "Creating…" : "Create token", icon: "add", onPressed: model.busy ? nil : {
                Task { @MainActor in await model.mint() }
            })
        }
    }
}

private struct MintedTokenCard: View {
    @Environment(\.fmPalette) var fm
    let minted: MintedMcpToken
    let serverURL: String
    let onDone: () -> Void
    let onCopied: (String) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            SettingsNote(text: "Copy it now. ForkMesh keeps only a hash, so it cannot show this token again.", color: fm.warning)
            Text(McpConnector.mask(minted.token))
                .fmText(.bodyMedium)
                .monospaced()
                .textSelection(.disabled)
            HStack(spacing: FmSpace.x2) {
                FmOutlinedButton("Copy token", icon: "content_copy", onPressed: {
                    UIPasteboard.general.setItems([[UIPasteboard.typeAutomatic: minted.token]], options: [.localOnly: true, .expirationDate: Date().addingTimeInterval(120)])
                    onCopied("Token")
                })
                FmOutlinedButton("Copy command", icon: "content_copy", onPressed: {
                    UIPasteboard.general.setItems(
                        [[UIPasteboard.typeAutomatic: McpConnector.claudeCommand(serverURL: serverURL, token: minted.token)]],
                        options: [.localOnly: true, .expirationDate: Date().addingTimeInterval(120)])
                    onCopied("Command")
                })
            }
            FmTextButton("Done", onPressed: onDone)
        }
    }
}

struct CopyableValueRow: View {
    @Environment(\.fmPalette) var fm
    let label: String
    let value: String

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            VStack(alignment: .leading, spacing: 2) {
                Text(label).fmText(.labelMedium, color: fm.textTertiary)
                Text(value.isEmpty ? "—" : value)
                    .fmText(.bodyMedium)
                    .monospaced()
                    .lineLimit(2)
                    .truncationMode(.middle)
                    .textSelection(.enabled)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            if !value.isEmpty {
                FmIconButton(icon: "content_copy", label: "Copy \(label)", size: 20, color: fm.textTertiary, onPressed: {
                    UIPasteboard.general.string = value
                })
            }
        }
    }
}
