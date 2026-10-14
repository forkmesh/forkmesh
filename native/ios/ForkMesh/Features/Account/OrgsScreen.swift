// The organizations the signed-in account belongs to, and creating one.
// Tapping an org opens OrgDetailScreen. Also the pieces OrgDetailScreen shares
// with it (`OrgRoleBadge`, `OrgTextField`). Logic: FMApi's OrgsListModel /
// CreateOrgModel.

import FMAccount
import FMApi
import FMBase
import SwiftUI

@MainActor
struct OrgsScreen: View {
    @Environment(\.fmPalette) var fm
    @State var model: OrgsListModel
    @State var creating: Bool = false
    @State var opened: String?

    init() {
        let api: ApiService? = Services.get(ApiService.self)
        let session: AuthSession? = Services.get(AuthService.self)?.session
        _model = State(initialValue: OrgsListModel(client: api, session: session))
    }

    var body: some View {
        content
            .background(fm.canvas.ignoresSafeArea())
            .navigationTitle("Organizations")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .principal) {
                    VStack(spacing: 2) {
                        Text("Organizations").fmText(.titleMedium).lineLimit(1)
                        Text("Shared /<org>/<repo> namespaces").fmText(.bodySmall).lineLimit(1)
                    }
                }
                if model.signedIn {
                    ToolbarItem(placement: .topBarTrailing) {
                        FmIconButton(icon: "add", label: "New organization", onPressed: { creating = true })
                            .accessibilityIdentifier("org-create-button")
                    }
                }
            }
            .task { await model.reload() }
            .sheet(isPresented: $creating) {
                CreateOrgSheet(onCreated: { (created: OrgSummary) in
                    creating = false
                    Task { await model.reload() }
                    opened = created.name
                })
                .fmSheetStyle(fitsContent: true, background: fm.bgRaised)
            }
            .navigationDestination(item: $opened) { (name: String) in
                OrgDetailScreen(orgName: name)
                    .shellPageClearance()
            }
    }

    @ViewBuilder private var content: some View {
        if !model.signedIn {
            FmEmptyState(icon: "apartment_outlined", title: OrgCopy.signInTitle, message: OrgCopy.signInMessage)
        } else {
            FmSwap(stateKey: model.stateKey) {
                listBody
            }
        }
    }

    @ViewBuilder private var listBody: some View {
        if model.loading {
            FmSkeletonList(rows: 5, label: "Loading organizations")
        } else if let failure = model.failure {
            OrgStateMessage(
                icon: "cloud_off_outlined", title: OrgCopy.loadFailed, message: failure,
                action: FmTextButton("Retry", icon: "refresh", onPressed: { Task { await model.reload() } }).fmAny)
        } else if model.orgs.isEmpty {
            OrgStateMessage(
                icon: "apartment_outlined", title: OrgCopy.emptyTitle, message: OrgCopy.emptyMessage,
                action: FmFilledButton("New organization", icon: "add", onPressed: { creating = true }).fmAny)
        } else {
            ScrollView {
                LazyVStack(spacing: FmSpace.x2) {
                    ForEach(model.orgs, id: \.name) { (org: OrgSummary) in
                        OrgRow(org: org, onTap: { opened = org.name })
                    }
                }
                .padding(FmSpace.x4)
            }
            .fmRefreshable { await model.reload() }
        }
    }
}

/// An empty or failed state with one action below it.
struct OrgStateMessage: View {
    let icon: String
    let title: String
    let message: String
    let action: AnyView?

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                FmEmptyState(icon: icon, title: title, message: message)
                    .fixedSize(horizontal: false, vertical: true)
                if let action = action {
                    action
                }
            }
            .padding(.top, FmSpace.x7)
        }
    }
}

private struct OrgRow: View {
    @Environment(\.fmPalette) var fm
    let org: OrgSummary
    let onTap: () -> Void

    var body: some View {
        FmCard(onTap: onTap, radius: FmRadius.lg) {
            HStack(spacing: FmSpace.x3) {
                FmIcon("apartment_outlined", size: 24, color: fm.accent)
                    .frame(width: 42, height: 42)
                    .background(fm.accentSubtle)
                    .clipShape(FmRoundedShape(radius: FmRadius.md))
                Text(org.name)
                    .fmText(.bodyMedium, weight: FmType.emphasis)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                OrgRoleBadge(role: org.role)
                FmIcon("chevron_right", size: 24, color: fm.textDisabled)
            }
        }
    }
}

/// A pill for an org role, coloured by privilege.
struct OrgRoleBadge: View {
    @Environment(\.fmPalette) var fm
    let role: String

    private var color: Color {
        switch orgRoleTone(role) {
        case .accent: return fm.accent
        case .warning: return fm.warning
        case .tertiary: return fm.textTertiary
        }
    }

    var body: some View {
        FmStatusBadge(label: orgRoleLabel(role), color: color)
    }
}

/// The compact filled text field of the org forms.
struct OrgTextField: View {
    @Environment(\.fmPalette) var fm
    let label: String
    @Binding var text: String
    let autofocus: Bool

    @FocusState var focused: Bool

    init(label: String, text: Binding<String>, autofocus: Bool = false) {
        self.label = label
        self._text = text
        self.autofocus = autofocus
    }

    var body: some View {
        FmField(label: label) {
            TextField("", text: $text)
                .fmText(.bodyLarge)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .focused($focused)
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
                .background(fm.bgBase)
                .clipShape(FmRoundedShape(radius: FmRadius.md))
                .overlay {
                    FmRoundedShape(radius: FmRadius.md)
                        .strokeBorder(focused ? fm.accent : Color.clear, lineWidth: 1.2)
                }
        }
        .padding(.bottom, FmSpace.x3)
        .onAppear {
            if autofocus {
                focused = true
            }
        }
    }
}

@MainActor
private struct CreateOrgSheet: View {
    @Environment(\.fmPalette) var fm
    @State var model: CreateOrgModel?
    let onCreated: (OrgSummary) -> Void

    init(onCreated: @escaping (OrgSummary) -> Void) {
        self.onCreated = onCreated
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: CreateOrgModel(client: api))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        if let model = model {
            CreateOrgForm(model: model, onCreated: onCreated)
        }
    }
}

private struct CreateOrgForm: View {
    @Environment(\.fmPalette) var fm
    @Bindable var model: CreateOrgModel
    let onCreated: (OrgSummary) -> Void

    private func submit() {
        Task { @MainActor in
            if let created = await model.submit() {
                onCreated(created)
            }
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("New organization")
                .fmText(.titleLarge)
            Text(OrgCopy.createHelper)
                .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                .padding(.top, FmSpace.x2)
            VStack(spacing: 0) {
                OrgTextField(label: "Name (lowercase, dashes)", text: $model.name, autofocus: true)
                    .accessibilityIdentifier("org-name-field")
                OrgTextField(label: "Display name (optional)", text: $model.displayName)
                OrgTextField(label: "Description (optional)", text: $model.description)
            }
            .padding(.top, FmSpace.x4)
            if !model.error.isEmpty {
                Text(model.error)
                    .fmText(.bodyMedium, color: fm.danger)
                    .fmMessageTools(model.error)
                    .padding(.top, FmSpace.x1)
            }
            FmFilledButton(
                "Create organization",
                fillWidth: true,
                onPressed: model.busy ? nil : { submit() })
                .overlay(alignment: .trailing) {
                    if model.busy {
                        FmCircularProgressIndicator(size: 18, strokeWidth: 2)
                            .padding(.trailing, FmSpace.x4)
                    }
                }
                .padding(.top, FmSpace.x3)
                .accessibilityIdentifier("org-create-submit")
        }
        .padding(FmSpace.x4)
    }
}
