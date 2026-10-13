import FMAccount
import FMApi
import FMBase
import SwiftUI

@MainActor
struct InstalledAppsScreen: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.openURL) var openURL
    @State var model: InstalledAppsModel
    @State var notice: FmNotice?
    @State var revoking: AppInstall?
    private let directoryURL: URL?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        let auth: AuthService? = Services.get(AuthService.self)
        let signedIn: Bool = !(auth?.session?.sessionToken ?? "").isEmpty && !(auth?.isGuest ?? false)
        _model = State(initialValue: InstalledAppsModel(client: signedIn ? api?.appsClient : nil))
        directoryURL = api?.url(InstalledAppsCopy.directoryPath)
    }

    var body: some View {
        content
            .background(fm.bgBase.ignoresSafeArea())
            .navigationTitle(InstalledAppsCopy.title)
            .navigationBarTitleDisplayMode(.inline)
            .task { if !model.loaded { await model.reload() } }
            .onChange(of: model.notice) { _, next in
                if let next = next {
                    notice = FmNotice(next)
                    model.notice = nil
                }
            }
            .fmDialog(isPresented: Binding(get: { revoking != nil }, set: { if !$0 { revoking = nil } })) {
                if let install = revoking {
                    FmConfirmDialog(
                        title: InstalledAppsCopy.revokeTitle(install.app.title), message: InstalledAppsCopy.revokeMessage,
                        confirmLabel: "Revoke", destructive: true, onCancel: { revoking = nil },
                        onConfirm: {
                            revoking = nil
                            Task { await model.revoke(install) }
                        })
                }
            }
    }

    @ViewBuilder private var content: some View {
        if !model.available {
            FmEmptyState(icon: "extension_outlined", title: InstalledAppsCopy.title, message: InstalledAppsCopy.signedOut)
        } else if model.loading && !model.loaded {
            FmSkeletonList(rows: 4, label: "Loading installed apps")
        } else if let failure = model.failure, !model.loaded {
            OrgStateMessage(
                icon: "cloud_off_outlined", title: "Could not load your apps", message: failure,
                action: FmTextButton("Retry", icon: "refresh", onPressed: { Task { await model.reload() } }).fmAny)
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmNoticeLine($notice)
                        .accessibilityIdentifier("installed-apps-notice")
                    if model.apps.isEmpty {
                        FmEmptyState(
                            icon: "extension_outlined", title: InstalledAppsCopy.empty, message: InstalledAppsCopy.emptyDetail)
                            .fixedSize(horizontal: false, vertical: true)
                    } else {
                        sections
                    }
                    if let directoryURL = directoryURL {
                        FmOutlinedButton(InstalledAppsCopy.directory, icon: "open_in_new", fillWidth: true) {
                            openURL(directoryURL)
                        }
                        .padding(.top, FmSpace.x4)
                        .accessibilityIdentifier("installed-apps-directory")
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmRefreshable { await model.reload() }
        }
    }

    @ViewBuilder private var sections: some View {
        if !model.apps.installs.isEmpty {
            FmSectionHeader(title: "Just me")
            ForEach(model.apps.installs) { (install: AppInstall) in
                InstalledAppCard(install: install, busy: model.busy.contains(install.installId), onRevoke: { revoking = install })
            }
        }
        ForEach(model.apps.organizations) { (org: AppOrgInstalls) in
            if !org.installs.isEmpty || !org.requests.isEmpty {
                FmSectionHeader(title: org.name)
                    .padding(.top, FmSpace.x3)
                ForEach(org.installs) { (install: AppInstall) in
                    InstalledAppCard(
                        install: install, busy: model.busy.contains(install.installId), onRevoke: { revoking = install })
                }
                if org.canManage && !org.requests.isEmpty {
                    Text("Requests")
                        .fmText(.labelLarge, color: fm.textSecondary)
                        .padding(.top, FmSpace.x2)
                    ForEach(org.requests) { (request: AppInstallRequest) in
                        AppInstallRequestCard(
                            request: request, busy: model.busy.contains(request.id),
                            onDecide: { (approve: Bool) in Task { await model.decide(request, org: org.name, approve: approve) } })
                    }
                }
            }
        }
    }
}

private struct AppScopeList: View {
    @Environment(\.fmPalette) var fm
    let scopes: [AppScope]

    var body: some View {
        if !scopes.isEmpty {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(scopes) { (scope: AppScope) in
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
                        FmIcon("check", size: 14, color: fm.textTertiary)
                        Text(scope.title).fmText(.bodySmall, color: fm.textSecondary)
                    }
                    .accessibilityElement(children: .combine)
                    .accessibilityHint(scope.hint)
                }
            }
        }
    }
}

private struct AppTitleLine: View {
    @Environment(\.fmPalette) var fm
    let app: AppSummary
    let detail: String

    var body: some View {
        HStack(alignment: .top, spacing: FmSpace.x3) {
            FmIcon("extension_outlined", size: 22, color: fm.accent)
                .frame(width: 40, height: 40)
                .background(fm.accentSubtle)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
            VStack(alignment: .leading, spacing: 2) {
                Text(app.title).fmText(.titleSmall).lineLimit(1)
                if !detail.isEmpty {
                    Text(detail).fmText(.bodySmall, color: fm.textTertiary).lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}

private struct InstalledAppCard: View {
    @Environment(\.fmPalette) var fm
    let install: AppInstall
    let busy: Bool
    let onRevoke: () -> Void

    private var detail: String {
        var parts: [String] = [install.lastUsedText()]
        if !install.installedBy.isEmpty { parts.append("installed by \(install.installedBy)") }
        return parts.joined(separator: " · ")
    }

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                AppTitleLine(app: install.app, detail: detail)
                if !install.app.description.isEmpty {
                    Text(install.app.description).fmText(.bodyMedium, color: fm.textSecondary)
                }
                AppScopeList(scopes: install.scopes)
                if install.canRevoke {
                    HStack {
                        Spacer(minLength: 0)
                        FmToneButton(label: "Revoke", tone: .danger, onPressed: busy ? nil : onRevoke)
                            .accessibilityIdentifier("installed-app-revoke-\(install.installId)")
                    }
                }
            }
        }
        .accessibilityIdentifier("installed-app-\(install.installId)")
    }
}

private struct AppInstallRequestCard: View {
    @Environment(\.fmPalette) var fm
    let request: AppInstallRequest
    let busy: Bool
    let onDecide: (Bool) -> Void

    var body: some View {
        FmCard(radius: FmRadius.lg) {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                AppTitleLine(
                    app: request.app,
                    detail: request.requester.isEmpty ? "Install request" : "Requested by \(request.requester)")
                AppScopeList(scopes: request.scopes)
                HStack(spacing: FmSpace.x2) {
                    Spacer(minLength: 0)
                    FmToneButton(label: "Deny", tone: .neutral, onPressed: busy ? nil : { onDecide(false) })
                    FmToneButton(label: "Approve", tone: .accent, filled: true, onPressed: busy ? nil : { onDecide(true) })
                }
            }
        }
        .accessibilityIdentifier("app-install-request-\(request.id)")
    }
}
