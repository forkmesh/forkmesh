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
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            header
            VStack(spacing: 0) {
                pocketRow
                rowDivider
                suggestionToggle
                if settings != nil {
                    rowDivider
                    diagnosticsToggle
                }
            }
            .padding(.horizontal, FmSpace.x3)
            .background(
                RoundedRectangle(cornerRadius: 14, style: .continuous).fill(fm.textPrimary.opacity(0.05)))
            items
            footerDivider
            versionRow
        }
        .padding(FmSpace.x3)
    }

    private var rowDivider: some View {
        Rectangle()
            .fill(fm.border.opacity(0.7))
            .frame(height: 0.5)
            .padding(.leading, 40)
    }

    private var footerDivider: some View {
        Rectangle()
            .fill(fm.border.opacity(0.7))
            .frame(height: 0.5)
    }

    private func tile(_ symbol: String, tint: Color? = nil) -> some View {
        Image(systemName: symbol)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(tint ?? fm.accent)
            .frame(width: 28, height: 28)
            .background(
                RoundedRectangle(cornerRadius: 8, style: .continuous).fill((tint ?? fm.accent).opacity(0.12)))
    }

    private var subtitle: String {
        state.guest ? "Exploring as a guest" : state.statusDescription
    }

    private var header: some View {
        HStack(spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                AvatarWithDot(state: state.relayState.fmState, label: state.name, avatarPng: state.avatarPng, size: 44)
                    .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(state.guest ? "Guest" : state.name)
                        .fmText(.titleSmall)
                        .lineLimit(1)
                    if !subtitle.isEmpty {
                        Text(subtitle)
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
        let tint: Color = item.action == .signOut ? fm.danger : fm.textPrimary
        return Button {
            choose(item.action)
        } label: {
            Label(item.title, systemImage: item.symbol)
                .labelStyle(.titleAndIcon)
                .fmText(.labelLarge, color: tint)
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, FmSpace.x3)
                .frame(maxWidth: .infinity, minHeight: 42)
                .background(
                    RoundedRectangle(cornerRadius: 12, style: .continuous).fill(tint.opacity(0.07)))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityIdentifier("hub-item-\(item.id)")
    }

    private var versionRow: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: FmSpace.x2) {
                versionButton
                diagnosticsButton
            }
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                versionButton
                diagnosticsButton
            }
        }
        .padding(.horizontal, FmSpace.x1)
    }

    private var versionButton: some View {
        Button {
            FmHaptics.selection()
            showWhatsNew = true
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: AppChangelog.label(infoDictionary: Bundle.main.infoDictionary))
                    .fmText(.labelSmall, color: fm.textSecondary)
                    .monospacedDigit()
                    .fixedSize()
                Text("What's new")
                    .fmText(.labelMedium, color: fm.accent)
                    .fixedSize()
            }
            .frame(maxWidth: .infinity, minHeight: 36, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .accessibilityIdentifier("hub-version")
    }

    @ViewBuilder private var diagnosticsButton: some View {
        if onSendDiagnostics != nil {
            DiagnosticsSendButton(alert: diagnostics.alert, compact: true) {
                FmHaptics.selection()
                sendingDiagnostics = true
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
            HStack(spacing: FmSpace.x3) {
                tile(walking ? "figure.walk" : "lock.iphone")
                    .symbolEffect(.pulse, options: .repeating, isActive: walking)
                Text(walking ? "Pocket mode, walking" : "Pocket mode")
                    .fmText(.bodyMedium)
                Spacer(minLength: 0)
                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
            }
            .frame(maxWidth: .infinity, minHeight: 46, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityHint(Text("Locks the screen to Glasses Alert, pings and walking. Slide to unlock."))
        .accessibilityIdentifier("hub-pocket-mode")
    }

    private var suggestionToggle: some View {
        Toggle(isOn: Binding<Bool>(get: { suggestions.on }, set: { (on: Bool) in suggestions.setOn(on) })) {
            HStack(spacing: FmSpace.x3) {
                tile("sparkles")
                Text("Suggestion mode")
                    .fmText(.bodyMedium)
            }
        }
        .tint(fm.accent)
        .controlSize(.small)
        .frame(minHeight: 46)
        .accessibilityIdentifier("hub-suggestion-mode")
    }

    @ViewBuilder private var diagnosticsToggle: some View {
        if let settings = settings {
            Toggle(isOn: Binding<Bool>(
                get: { settings.showTelemetryStrip },
                set: { (value: Bool) in settings.setShowTelemetryStrip(value) })
            ) {
                HStack(spacing: FmSpace.x3) {
                    tile("waveform.path.ecg")
                    Text("Diagnostics")
                        .fmText(.bodyMedium)
                }
            }
            .tint(fm.accent)
            .controlSize(.small)
            .frame(minHeight: 46)
            .accessibilityIdentifier("hub-diagnostics-strip")
        }
    }

    private func row(
        title: String, symbol: String, id: String, destructive: Bool = false, action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            HStack(spacing: FmSpace.x3) {
                tile(symbol, tint: destructive ? fm.danger : nil)
                Text(title)
                    .fmText(.bodyMedium, color: destructive ? fm.danger : fm.textPrimary)
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
