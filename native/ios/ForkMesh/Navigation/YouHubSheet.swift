import FMAccount
import FMApi
import FMBase
import FMTools
import SwiftUI

struct YouHubSheet: View {
    @Environment(\.fmPalette) private var fm
    @State private var confirmSignOut: Bool = false
    @State private var showStatus: Bool = false
    @State private var reportingBug: Bool = false
    @State private var showWhatsNew: Bool = false
    @State private var sendingDiagnostics: Bool = false
    @State private var diagnostics: PerformanceDiagnostics = PerformanceDiagnostics.shared
    @State private var settings: SettingsService? = Services.get(SettingsService.self)
    @State private var suggestions: ShellSuggestions = ShellSuggestions.shared
    private var walk: WalkActivityController { WalkActivityController.shared }

    let router: AppRouter
    let state: ShellTopBarState
    var linksInBar: Bool = false
    let onOpen: (Route) -> Void
    let onAction: (HubItem.Action) -> Void
    let onClose: () -> Void
    var onSendDiagnostics: ((String) async -> String?)? = nil

    private var groups: [HubGroup] {
        let all: [HubGroup] = HubGroups.current(guest: state.guest)
        guard linksInBar else { return all }
        let account: [HubItem] = all.flatMap(\.items).filter { (item: HubItem) -> Bool in
            item.route?.kind == .settings || (!HomeShell.movesToBar(item.action) && item.action != .reportBug)
        }
        return account.isEmpty ? [] : [HubGroup(id: "account", title: "Account", items: account)]
    }

    var body: some View {
        ViewThatFits(in: .vertical) {
            content
            ScrollView {
                content
            }
            .scrollIndicators(.hidden)
        }
        .background(fm.sheetSurface)
        .confirmationDialog("Sign out of this project?", isPresented: $confirmSignOut, titleVisibility: .visible) {
            Button("Sign out", role: .destructive) {
                onClose()
                onAction(.signOut)
            }
            Button("Cancel", role: .cancel) {}
        }
        .sheet(isPresented: $showStatus) {
            UserStatusSheet()
        }
        .sheet(isPresented: $reportingBug) {
            ReportBugSheet(screen: "hub")
        }
        .sheet(isPresented: $showWhatsNew) {
            NavigationStack {
                WhatsNewScreen()
                    .toolbar {
                        ToolbarItem(placement: .confirmationAction) {
                            Button("Done") { showWhatsNew = false }
                        }
                    }
            }
        }
        .sheet(isPresented: $sendingDiagnostics) {
            if let send = onSendDiagnostics {
                DiagnosticsSendSheet(router: router, send: send, onSent: { onClose() })
            }
        }
        .task { await HubPackageState.shared.refresh() }
        .task {
            while !Task.isCancelled {
                await diagnostics.refresh()
                try? await Task.sleep(for: .seconds(PerformanceDiagnostics.refreshSeconds))
            }
        }
        .fmLoading(HubPackageState.shared.refreshing)
        .fmFeedback(.destructiveConfirmation, trigger: confirmSignOut)
        .accessibilityIdentifier("you-hub")
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            header
            Divider()
            VStack(spacing: 0) {
                pocketRow
                suggestionToggle
                diagnosticsToggle
            }
            Divider()
            items
            versionRow
        }
        .padding(FmSpace.x3)
    }

    private var header: some View {
        HStack(spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                AvatarWithDot(state: state.relayState.fmState, label: state.name, avatarPng: state.avatarPng, size: 40)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(state.guest ? "Guest" : state.name)
                        .fmText(.titleSmall)
                        .lineLimit(1)
                    if !state.statusDescription.isEmpty {
                        Text(state.statusDescription)
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .lineLimit(1)
                    }
                }
            }
            .accessibilityElement(children: .combine)
            Spacer(minLength: 0)
            if state.canSetStatus {
                Button {
                    FmHaptics.selection()
                    showStatus = true
                } label: {
                    Label("Set a status", systemImage: "face.smiling")
                        .labelStyle(.titleAndIcon)
                        .fmText(.labelMedium)
                        .lineLimit(1)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .accessibilityIdentifier("hub-set-status")
            }
        }
    }

    @ViewBuilder private var items: some View {
        if linksInBar {
            ViewThatFits(in: .horizontal) {
                HStack(spacing: FmSpace.x2) {
                    ForEach(groups.flatMap(\.items)) { (item: HubItem) in
                        chip(item)
                    }
                    Spacer(minLength: 0)
                }
                VStack(spacing: 0) {
                    ForEach(groups.flatMap(\.items)) { (item: HubItem) in
                        row(title: item.title, symbol: item.symbol, id: item.id, destructive: item.action == .signOut) {
                            choose(item.action)
                        }
                    }
                }
            }
        } else {
            ForEach(groups) { (group: HubGroup) in
                VStack(alignment: .leading, spacing: 0) {
                    Text(group.title)
                        .fmText(.labelSmall, color: fm.textSecondary)
                        .padding(.top, FmSpace.x1)
                    ForEach(group.items) { (item: HubItem) in
                        row(title: item.title, symbol: item.symbol, id: item.id, destructive: item.action == .signOut) {
                            choose(item.action)
                        }
                    }
                }
            }
        }
    }

    private func chip(_ item: HubItem) -> some View {
        let destructive: Bool = item.action == .signOut
        return Button {
            choose(item.action)
        } label: {
            Label(item.title, systemImage: item.symbol)
                .labelStyle(.titleAndIcon)
                .fmText(.labelMedium, color: destructive ? fm.danger : fm.textPrimary)
                .lineLimit(1)
        }
        .buttonStyle(.bordered)
        .controlSize(.small)
        .tint(destructive ? fm.danger : fm.textPrimary)
        .accessibilityIdentifier("hub-item-\(item.id)")
    }

    private var versionRow: some View {
        HStack(spacing: FmSpace.x2) {
            Button {
                FmHaptics.selection()
                showWhatsNew = true
            } label: {
                HStack(spacing: FmSpace.x2) {
                    Text(verbatim: AppChangelog.label(infoDictionary: Bundle.main.infoDictionary))
                        .fmText(.labelSmall, color: fm.textSecondary)
                        .monospacedDigit()
                        .lineLimit(1)
                    Text("What's new")
                        .fmText(.labelSmall, color: fm.accent)
                        .lineLimit(1)
                }
                .frame(maxWidth: .infinity, minHeight: 32, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.borderless)
            .accessibilityIdentifier("hub-version")
            if onSendDiagnostics != nil {
                DiagnosticsSendButton(alert: diagnostics.alert, compact: true) {
                    FmHaptics.selection()
                    sendingDiagnostics = true
                }
            }
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, FmSpace.x1)
        .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(fm.bgElevated))
    }

    private var pocketRow: some View {
        let walking: Bool = walk.moving
        return Button {
            FmHaptics.selection()
            onClose()
            ShellPocket.shared.lock()
        } label: {
            Label {
                Text(walking ? "Pocket mode, walking" : "Pocket mode")
                    .fmText(.bodyMedium)
            } icon: {
                Image(systemName: walking ? "figure.walk" : "lock.iphone")
                    .foregroundStyle(fm.accent)
                    .symbolEffect(.pulse, options: .repeating, isActive: walking)
            }
            .frame(maxWidth: .infinity, minHeight: 36, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityHint(Text("Locks the screen to Glasses Alert, pings and walking. Slide to unlock."))
        .accessibilityIdentifier("hub-pocket-mode")
    }

    private var suggestionToggle: some View {
        Toggle(isOn: Binding<Bool>(get: { suggestions.on }, set: { (on: Bool) in suggestions.setOn(on) })) {
            Label {
                Text("Suggestion mode")
                    .fmText(.bodyMedium)
            } icon: {
                Image(systemName: "sparkles")
                    .foregroundStyle(fm.accent)
            }
        }
        .tint(fm.accent)
        .controlSize(.small)
        .frame(minHeight: 36)
        .accessibilityIdentifier("hub-suggestion-mode")
    }

    @ViewBuilder private var diagnosticsToggle: some View {
        if let settings = settings {
            Toggle(isOn: Binding<Bool>(
                get: { settings.showTelemetryStrip },
                set: { (value: Bool) in settings.setShowTelemetryStrip(value) })
            ) {
                Label {
                    Text("Diagnostics")
                        .fmText(.bodyMedium)
                } icon: {
                    Image(systemName: "waveform.path.ecg")
                        .foregroundStyle(fm.accent)
                }
            }
            .tint(fm.accent)
            .controlSize(.small)
            .frame(minHeight: 36)
            .accessibilityIdentifier("hub-diagnostics-strip")
        }
    }

    private func row(
        title: String, symbol: String, id: String, destructive: Bool = false, action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Label {
                Text(title)
                    .fmText(.bodyMedium, color: destructive ? fm.danger : fm.textPrimary)
            } icon: {
                Image(systemName: symbol)
                    .foregroundStyle(destructive ? fm.danger : fm.accent)
            }
            .frame(maxWidth: .infinity, minHeight: 36, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("hub-item-\(id)")
    }

    private func choose(_ action: HubItem.Action) {
        FmHaptics.selection()
        switch action {
        case .signOut:
            confirmSignOut = true
        case .setStatus:
            showStatus = true
        case .reportBug:
            reportingBug = true
        case let .route(route):
            onClose()
            onOpen(route)
        default:
            onClose()
            onAction(action)
        }
    }
}

struct ShellHubUnroll: ViewModifier {
    let progress: CGFloat

    func body(content: Content) -> some View {
        content
            .mask(alignment: .top) {
                Rectangle()
                    .padding(-40)
                    .scaleEffect(x: 1, y: max(progress, 0.001), anchor: .top)
            }
            .scaleEffect(0.9 + 0.1 * progress, anchor: .topTrailing)
            .offset(y: -16 * (1 - progress))
            .opacity(Double(min(1, progress * 1.6)))
    }
}
