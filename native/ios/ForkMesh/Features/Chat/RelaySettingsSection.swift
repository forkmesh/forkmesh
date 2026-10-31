// The Relay section of Settings (every relay this phone knows, which one is active, add / test / rename / remove / switch) and the Add Relay
// sheet. Logic: FMChat's RelaySettingsModel and AddRelayModel.

import FMAccount
import FMBase
import FMChat
import FMNet
import PhotosUI
import SwiftUI

struct RelaySettingsSection: View {
    @State private var model: RelaySettingsModel? = RelaySettingsSection.makeModel()

    init() {}

    @MainActor
    static func makeModel() -> RelaySettingsModel? {
        guard let settings = Services.get(SettingsService.self), let auth = Services.get(AuthService.self),
            let relay = Services.get(RelayService.self)
        else { return nil }
        let http: HTTPClient = Services.get(HTTPClient.self) ?? URLSessionHTTPClient()
        return RelaySettingsModel(
            settings: settings, auth: auth, relay: relay, switcher: Services.get(RelaySwitchController.self),
            probe: { RelayProbe(http: http) }, http: http)
    }

    var body: some View {
        if let model = model {
            RelaySettingsBody(model: model)
        }
    }
}

private struct RelaySettingsBody: View {
    @Environment(\.fmPalette) private var fm
    let model: RelaySettingsModel
    @State private var adding: Bool = false
    @State private var renaming: RelayProfile?
    @State private var renameText: String = ""
    @State private var removing: RelayProfile?
    @State private var logoTarget: RelayProfile?
    @State private var logoPicking: Bool = false
    @State private var logoItem: PhotosPickerItem?

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(model.rows.enumerated()), id: \.element.id) { (entry: (offset: Int, element: RelayRowInfo)) in
                if entry.offset > 0 {
                    FmRowDivider()
                }
                RelayRow(
                    info: entry.element, connection: model.connection, testing: model.testing == entry.element.id,
                    busy: model.busy,
                    onSwitch: { Task { await model.switchTo(entry.element.relay) } },
                    onTest: { Task { await model.test(entry.element.relay) } },
                    onRename: {
                        renameText = entry.element.relay.label
                        renaming = entry.element.relay
                    },
                    onRemove: { removing = entry.element.relay },
                    onChooseLogo: {
                        logoTarget = entry.element.relay
                        logoPicking = true
                    },
                    onRemoveLogo: { setLogo(entry.element.relay, "") })
            }
            if model.hasMultiple {
                FmRowDivider()
                NavigationLink {
                    RelayNetworksScreen()
                        .shellPageClearance()
                } label: {
                    FmListRow(
                        title: "Browse all networks", subtitle: "Repositories across every relay you are signed into",
                        leading: AnyView(FmIcon("hub_outlined", color: fm.textSecondary)))
                        .allowsHitTesting(false)
                }
                .buttonStyle(.plain)
            }
            if let notice = model.notice {
                RelayNoticeView(notice: notice).padding(.top, FmSpace.x3)
            }
            actions.padding(.top, FmSpace.x3)
            Text(RelaySettingsModel.explanation)
                .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                .padding(.top, FmSpace.x3)
            RelayFeatureSummary(model: model)
            Text(model.buildLine)
                .fmText(.bodySmall, color: fm.textTertiary)
                .padding(.top, FmSpace.x2)
        }
        .sheet(isPresented: $adding) {
            AddRelaySheet(onAdded: { (profile: RelayProfile) in
                adding = false
                model.added(profile)
            })
        }
        .photosPicker(isPresented: $logoPicking, selection: $logoItem, matching: .images)
        .onChange(of: logoItem) { (_: PhotosPickerItem?, item: PhotosPickerItem?) in
            guard let item, let target = logoTarget else { return }
            logoItem = nil
            Task { await saveLogo(item, for: target) }
        }
        .alert("Rename relay", isPresented: renameBinding) {
            TextField(renaming?.id ?? "", text: $renameText)
            Button("Cancel", role: .cancel) { renaming = nil }
            Button("Save") {
                if let target = renaming { model.rename(target, to: renameText) }
                renaming = nil
            }
        }
        .alert(removeTitle, isPresented: removeBinding) {
            Button("Cancel", role: .cancel) { removing = nil }
            Button("Remove", role: .destructive) {
                if let target = removing { Task { await model.remove(target) } }
                removing = nil
            }
        } message: {
            Text(removing.map { model.removeConfirmation($0) } ?? "")
        }
    }

    private func saveLogo(_ item: PhotosPickerItem, for target: RelayProfile) async {
        guard let data = try? await item.loadTransferable(type: Data.self),
            let name = await ProjectLogoStore.save(data, projectKey: target.id)
        else {
            model.say("That image could not be used as a logo.", error: true)
            return
        }
        setLogo(target, name)
        model.say("Logo updated for \(target.label).")
    }

    private func setLogo(_ target: RelayProfile, _ name: String) {
        let previous: String = model.settings.relays.byId(target.id)?.logo ?? target.logo
        model.setLogo(target, logo: name)
        if !previous.isEmpty, previous != name { ProjectLogoStore.delete(previous) }
    }

    private var renameBinding: Binding<Bool> {
        Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })
    }

    private var removeBinding: Binding<Bool> {
        Binding(get: { removing != nil }, set: { if !$0 { removing = nil } })
    }

    private var removeTitle: String { "Remove \(removing?.label ?? "relay")?" }

    private var actions: some View {
        HStack(spacing: FmSpace.x3) {
            FmFilledButton("Add relay", icon: "add", onPressed: model.busy ? nil : { adding = true })
            connectionButton
            FmStatusBadge(label: RelaySettingsModel.stateLabel(model.connection), color: stateColor)
        }
    }

    @ViewBuilder private var connectionButton: some View {
        switch model.connection {
        case .connecting:
            FmOutlinedButton("Connecting…", icon: "sync", onPressed: { model.connectToggle() })
        case .connected:
            FmOutlinedButton("Disconnect", icon: "link_off", onPressed: { model.connectToggle() })
        case .offline:
            FmOutlinedButton("Reconnect", icon: "refresh", onPressed: { model.connectToggle() })
        }
    }

    private var stateColor: Color {
        switch model.connection {
        case .connected: return fm.success
        case .connecting: return fm.warning
        case .offline: return fm.textTertiary
        }
    }
}

private struct RelayRow: View {
    @Environment(\.fmPalette) private var fm
    let info: RelayRowInfo
    let connection: RelayConnectionState
    let testing: Bool
    let busy: Bool
    let onSwitch: () -> Void
    let onTest: () -> Void
    let onRename: () -> Void
    let onRemove: () -> Void
    let onChooseLogo: () -> Void
    let onRemoveLogo: () -> Void

    var body: some View {
        FmListRow(
            title: info.relay.label,
            subtitleView: AnyView(subtitle),
            leading: AnyView(leading),
            trailing: AnyView(trailing),
            onTap: info.active || busy ? nil : onSwitch,
            showChevron: false)
    }

    @ViewBuilder private var leading: some View {
        if testing {
            FmCircularProgressIndicator(size: 16, strokeWidth: 2).frame(width: 24)
        } else {
            FmIcon(info.active ? "radio_button_checked" : "radio_button_off", size: 20,
                   color: info.active ? fm.accent : fm.textTertiary)
                .frame(width: 24)
        }
    }

    private var subtitle: some View {
        let place: Text = info.place.isEmpty
            ? Text("")
            : Text(info.place).foregroundColor(fm.textSecondary) + Text(" · ").foregroundColor(fm.textTertiary)
        return (place + Text(info.who).foregroundColor(info.signedIn ? fm.success : fm.textTertiary))
            .fmText(.bodySmall)
            .lineLimit(1)
            .truncationMode(.tail)
    }

    private var trailing: some View {
        HStack(spacing: FmSpace.x1) {
            if info.active {
                Circle().fill(pipColor).frame(width: 8, height: 8)
                    .accessibilityLabel(RelaySettingsModel.stateLabel(connection))
            }
            Menu {
                Button("Test connection", action: onTest)
                Button("Rename", action: onRename)
                Button(info.relay.logo.isEmpty ? "Choose logo…" : "Change logo…", action: onChooseLogo)
                if !info.relay.logo.isEmpty {
                    Button("Remove logo", action: onRemoveLogo)
                }
                if info.removable {
                    Button("Remove", role: .destructive, action: onRemove)
                } else {
                    Button("Built-in relay") {}.disabled(true)
                }
            } label: {
                FmIcon("more_horiz", color: fm.textSecondary).frame(width: 44, height: 44)
            }
            .accessibilityLabel("Relay options")
        }
    }

    private var pipColor: Color {
        switch connection {
        case .connected: return fm.success
        case .connecting: return fm.warning
        case .offline: return fm.textTertiary
        }
    }
}

/// What the active relay does not do: the one place the app explains an absence.
private struct RelayFeatureSummary: View {
    @Environment(\.fmPalette) private var fm
    let model: RelaySettingsModel

    var body: some View {
        let missing: [RelayFeature] = model.missingFeatures
        if !missing.isEmpty {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    FmIcon("info_outline", size: 18, color: fm.warning)
                    Text("Not available on \(model.activeLabel)")
                        .fmText(.bodySmall, color: fm.warning, weight: FmType.emphasis)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools("Not available on \(model.activeLabel): \(missing.map(\.label).joined(separator: ", ")). \(model.missingExplanation)")
                }
                Text(missing.map(\.label).joined(separator: " · "))
                    .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                Text(model.missingExplanation)
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            .padding(FmSpace.x3)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(fm.warning.opacity(fm.dark ? 0.14 : 0.10))
            .clipShape(FmRoundedShape(radius: FmRadius.md))
            .padding(.top, FmSpace.x3)
        }
    }
}

/// An inline result line under the relay list, kept on screen rather than flashed.
struct RelayNoticeView: View {
    @Environment(\.fmPalette) private var fm
    let notice: RelayNotice

    var body: some View {
        let color: Color = notice.error ? fm.danger : fm.success
        HStack(alignment: .top, spacing: FmSpace.x2) {
            FmIcon(notice.error ? "error_outline" : "check_circle_outline", size: 18, color: color)
            Text(notice.message)
                .fmText(.bodySmall, color: color, height: 1.35)
                .frame(maxWidth: .infinity, alignment: .leading)
                .fmMessageTools(notice.message)
        }
        .padding(FmSpace.x3)
        .background(color.opacity(fm.dark ? 0.16 : 0.10))
        .clipShape(FmRoundedShape(radius: FmRadius.md))
    }
}

/// Ask for a relay address, verify it, and save it when it answers like a ForkMesh relay.
struct AddRelaySheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var model: AddRelayModel?
    private let onAdded: (RelayProfile) -> Void

    init(onAdded: @escaping (RelayProfile) -> Void) {
        self.onAdded = onAdded
        let settings: SettingsService? = Services.get(SettingsService.self)
        let http: HTTPClient = Services.get(HTTPClient.self) ?? URLSessionHTTPClient()
        _model = State(initialValue: settings.map { AddRelayModel(relays: $0.relays, probe: RelayProbe(http: http)) })
    }

    var body: some View {
        if let model = model {
            AddRelayForm(model: model, onAdded: onAdded)
                .fmSheetStyle()
        }
    }
}

private struct AddRelayForm: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: AddRelayModel
    let onAdded: (RelayProfile) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            FmPanelHeader(title: "Add a relay")
            Text(AddRelayModel.intro)
                .fmText(.bodySmall, color: fm.textSecondary, height: 1.35)
                .padding(.top, FmSpace.x3)
            FmField(label: "Relay address") {
                TextField("relay.example.com", text: $model.host)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled(true)
                    .keyboardType(.URL)
                    .onSubmit { Task { await model.check() } }
            }
            .padding(.top, FmSpace.x4)
            FmField(label: "Name (optional)") {
                TextField("Staging", text: $model.label)
                    .onSubmit { Task { await model.check() } }
            }
            .padding(.top, FmSpace.x3)
            verdict.padding(.top, FmSpace.x4)
            HStack(spacing: FmSpace.x3) {
                FmOutlinedButton(model.checkLabel, fillWidth: true, onPressed: model.checking ? nil : {
                    Task { await model.check() }
                })
                FmFilledButton(model.addLabel, fillWidth: true, busy: false, onPressed: model.canAdd ? { add() } : nil)
            }
            .padding(.top, FmSpace.x4)
        }
        .padding(FmSpace.x4)
        .animation(.easeOut(duration: FmMotion.normal), value: model.checking)
    }

    @ViewBuilder private var verdict: some View {
        if model.checking {
            HStack(spacing: FmSpace.x3) {
                FmCircularProgressIndicator(size: 16, strokeWidth: 2)
                Text("Checking that this is a ForkMesh relay…").fmText(.bodySmall, color: fm.textSecondary)
            }
        } else if let result = model.result {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                RelayNoticeView(notice: RelayNotice(message: result.message, error: !result.ok))
                if let missing = model.missingLine {
                    Text(missing).fmText(.bodySmall, color: fm.warning, height: 1.35)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(missing)
                }
            }
        }
    }

    private func add() {
        let logoUrl: String = model.result?.logoUrl ?? ""
        let branded: Bool = model.result?.branded ?? false
        if let saved = model.add() {
            FmHaptics.commit()
            Task { await ProjectImages.shared.adoptInstanceLogo(saved.id, logoUrl: logoUrl, branded: branded) }
            onAdded(saved)
        }
    }
}
