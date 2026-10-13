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

    private var recents: [Route] {
        let installed: Set<String> = PackageGateSync.publish()
        return router.recents.filter { PackageGate.shows($0, installed: installed) }
    }

    var body: some View {
        NavigationStack {
            List {
                Section {
                    header
                }
                Section {
                    pocketRow
                    suggestionToggle
                }
                if !recents.isEmpty {
                    Section("Recent") {
                        ForEach(recents, id: \.self) { (route: Route) in
                            row(title: route.title, symbol: RouteCatalog.symbol(route.kind), id: "recent-\(route.kind.rawValue)") {
                                choose(.route(route))
                            }
                        }
                    }
                }
                ForEach(groups) { (group: HubGroup) in
                    Section(group.title) {
                        ForEach(group.items) { (item: HubItem) in
                            row(title: item.title, symbol: item.symbol, id: item.id, destructive: item.action == .signOut) {
                                choose(item.action)
                            }
                        }
                    }
                }
                Section {
                    versionRow
                }
            }
            .listStyle(.insetGrouped)
            .navigationDestination(isPresented: $showWhatsNew) {
                WhatsNewScreen()
            }
            .navigationTitle("You")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    if let settings = settings {
                        Toggle(isOn: Binding<Bool>(
                            get: { settings.showTelemetryStrip },
                            set: { (value: Bool) in settings.setShowTelemetryStrip(value) })
                        ) {
                            Text("Diagnostics")
                                .fmText(.bodySmall, color: fm.textSecondary)
                        }
                        .toggleStyle(.switch)
                        .fixedSize()
                        .accessibilityIdentifier("hub-diagnostics-strip")
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { onClose() }
                        .keyboardShortcut(.cancelAction)
                }
            }
            .confirmationDialog("Sign out of this project?", isPresented: $confirmSignOut, titleVisibility: .visible) {
                Button("Sign out", role: .destructive) {
                    onClose()
                    onAction(.signOut)
                }
                Button("Cancel", role: .cancel) {}
            }
        }
        .sheet(isPresented: $showStatus) {
            UserStatusSheet()
        }
        .sheet(isPresented: $reportingBug) {
            ReportBugSheet(screen: "hub")
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
        .fmFeedback(.destructiveConfirmation, trigger: confirmSignOut)
        .accessibilityIdentifier("you-hub")
    }

    private var header: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            HStack(spacing: FmSpace.x3) {
                AvatarWithDot(state: state.relayState.fmState, label: state.name, avatarPng: state.avatarPng, size: 48)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(state.guest ? "Guest" : state.name)
                        .fmText(.titleMedium)
                        .lineLimit(1)
                    if !state.statusDescription.isEmpty {
                        Text(state.statusDescription)
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .lineLimit(2)
                    }
                }
                Spacer(minLength: 0)
            }
            .accessibilityElement(children: .combine)
            HStack(spacing: FmSpace.x2) {
                ProjectSwitcherButton()
                Spacer(minLength: 0)
                if state.canSetStatus {
                    Button {
                        FmHaptics.selection()
                        showStatus = true
                    } label: {
                        Label("Set a status", systemImage: "face.smiling")
                            .labelStyle(.titleAndIcon)
                    }
                    .buttonStyle(.bordered)
                    .frame(minHeight: 44)
                    .accessibilityIdentifier("hub-set-status")
                }
            }
        }
        .padding(.vertical, FmSpace.x1)
    }

    private var versionRow: some View {
        HStack(spacing: FmSpace.x2) {
            Button {
                FmHaptics.selection()
                showWhatsNew = true
            } label: {
                VStack(alignment: .leading, spacing: 2) {
                    Text(verbatim: AppChangelog.label(infoDictionary: Bundle.main.infoDictionary))
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .monospacedDigit()
                    Text("What's new")
                        .fmText(.bodySmall, color: fm.accent)
                }
                .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
                .contentShape(Rectangle())
            }
            .buttonStyle(.borderless)
            .accessibilityIdentifier("hub-version")
            if onSendDiagnostics != nil {
                DiagnosticsSendButton(alert: diagnostics.alert) {
                    FmHaptics.selection()
                    sendingDiagnostics = true
                }
            }
        }
    }

    private var pocketRow: some View {
        let walking: Bool = walk.moving
        return Button {
            FmHaptics.selection()
            onClose()
            ShellPocket.shared.lock()
        } label: {
            Label {
                VStack(alignment: .leading, spacing: 2) {
                    Text(walking ? "Pocket mode, walking" : "Pocket mode")
                        .foregroundStyle(fm.textPrimary)
                    Text("Locks the screen to Glasses Alert, pings and walking. Slide to unlock.")
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            } icon: {
                Image(systemName: walking ? "figure.walk" : "lock.iphone")
                    .foregroundStyle(fm.accent)
                    .symbolEffect(.pulse, options: .repeating, isActive: walking)
            }
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("hub-pocket-mode")
    }

    private var suggestionToggle: some View {
        Toggle(isOn: Binding<Bool>(get: { suggestions.on }, set: { (on: Bool) in suggestions.setOn(on) })) {
            Label {
                Text("Suggestion mode")
                    .foregroundStyle(fm.textPrimary)
            } icon: {
                Image(systemName: "sparkles")
                    .foregroundStyle(fm.accent)
            }
        }
        .tint(fm.accent)
        .frame(minHeight: 44)
        .accessibilityIdentifier("hub-suggestion-mode")
    }

    private func row(
        title: String, symbol: String, id: String, destructive: Bool = false, action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Label {
                Text(title)
                    .foregroundStyle(destructive ? fm.danger : fm.textPrimary)
            } icon: {
                Image(systemName: symbol)
                    .foregroundStyle(destructive ? fm.danger : fm.accent)
            }
            .frame(maxWidth: .infinity, minHeight: 44, alignment: .leading)
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
