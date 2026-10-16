import FMApi
import FMBase
import FMChat
import FMControl
import FMDesktopRemote
import FMInsights
import FMTeammates
import SwiftUI

struct DesktopsDestination: View {
    var body: some View {
        if let hub = DesktopRemoteFeature.hub, let diagnostics = DesktopRemoteFeature.diagnostics {
            DesktopsScreen(hub: hub, diagnostics: diagnostics)
        } else {
            DesktopUnavailableView()
        }
    }
}

struct DesktopsScreen: View {
    @Bindable var hub: DesktopControlHub
    @Bindable var diagnostics: DiagnosticsStore
    @State private var fleet: FleetLightsModel? = InsightsFeature.api.map { FleetLightsModel(transport: $0) }
    private let buildMac: BuildMacLightModel? = InsightsFeature.buildMacLight
    private let shortcuts: ShortcutsStore? = TeammatesFeature.shortcuts
    @State private var showShortcutOutput = false

    var body: some View {
        List {
            if let desktop = hub.desktop {
                Section {
                    NavigationLink(value: Route.desktop(id: desktop.name.lowercased(), section: nil)) {
                        VStack(alignment: .leading, spacing: 8) {
                            HStack {
                                Image(systemName: "desktopcomputer")
                                Text(desktop.name.isEmpty ? "Desktop" : desktop.name)
                                    .font(.headline)
                                Spacer()
                                if let health = diagnostics.health {
                                    HealthLevelBadge(level: health.level)
                                }
                            }
                            DesktopReachLine(reach: hub.reach)
                            if let health = diagnostics.health {
                                DesktopHealthGauges(health: health, compact: true)
                            }
                        }
                        .padding(.vertical, 4)
                    }
                } footer: {
                    if diagnostics.unsupported {
                        Text("Update ForkMesh on this desktop to see its health here.")
                    } else if !diagnostics.error.isEmpty && hub.reach.isConnected {
                        Text(diagnostics.error)
                            .fmMessageTools(diagnostics.error)
                    }
                }
            } else if let local = hub.local {
                DesktopDiscoverySection(control: local)
            } else {
                Section {
                    Text(
                        "Open ForkMesh on your phone while it is on the same Wi-Fi as your desktop and approve it there. After that, the desktop works from anywhere, encrypted end to end through the relay."
                    )
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                }
            }
            if let light = buildMac?.light {
                Section("Office MacBook") {
                    BuildMacRow(light: light, shortcuts: shortcuts) { showShortcutOutput = true }
                }
            }
            if let fleet = fleet {
                if !fleet.runners.isEmpty {
                    Section("Runners") {
                        ForEach(fleet.runners) { (light: FleetLight) in
                            FleetLightRow(
                                light: light, agents: ShellCounts.shared.runnerAgents[light.name.lowercased()] ?? 0)
                        }
                    }
                }
                if !fleet.cores.isEmpty {
                    Section("Core nodes") {
                        ForEach(fleet.cores) { (light: FleetLight) in
                            FleetLightRow(light: light, agents: 0)
                        }
                        NavigationLink(value: Route.status) {
                            Label("Status page", systemImage: "waveform.path.ecg")
                        }
                    }
                }
            }
        }
        .navigationTitle("Desktops")
        .navigationDestination(isPresented: $showShortcutOutput) {
            if let shortcuts { ShortcutOutputScreen(store: shortcuts).shellPageClearance() }
        }
        .fmRefreshable { await refreshAll() }
        .task { await refreshAll() }
    }

    private func refreshAll() async {
        let lights: Task<Void, Never> = Task {
            if let fleet { await fleet.load() }
        }
        let mac: Task<Void, Never> = Task {
            if let buildMac { await buildMac.load() }
            await shortcuts?.loadRun()
        }
        await diagnostics.refreshHealth()
        await lights.value
        await mac.value
    }
}

private struct FleetStateDot: View {
    let color: Color

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 8, height: 8)
            .accessibilityHidden(true)
    }
}

enum BuildMacShortcut: CaseIterable, Identifiable {
    case iosAdHoc
    case macDesktop

    static let repo = "forkmesh/forkmesh"

    var id: String { file }

    var file: String {
        switch self {
        case .iosAdHoc: return "deploy-native-ios-ad-hoc-mac.sh"
        case .macDesktop: return "build-desktop-macos-mac.sh"
        }
    }

    var name: String {
        switch self {
        case .iosAdHoc: return "Deploy native iOS app (ad hoc, office Mac)"
        case .macDesktop: return "Build macOS desktop app (office Mac)"
        }
    }

    var caption: String {
        switch self {
        case .iosAdHoc: return "iOS"
        case .macDesktop: return "macOS"
        }
    }

    var symbol: String {
        switch self {
        case .iosAdHoc: return "iphone"
        case .macDesktop: return "macwindow"
        }
    }

    var hint: String {
        switch self {
        case .iosAdHoc: return "Build the iOS app ad hoc on the Office MacBook and email the install link"
        case .macDesktop: return "Build the macOS desktop app on the Office MacBook"
        }
    }

    var entry: ShortcutEntry {
        ShortcutEntry(file: file, scope: "shortcut", name: name, kind: "script")
    }
}

private struct BuildMacRow: View {
    let light: BuildMacLight
    let shortcuts: ShortcutsStore?
    let onStarted: () -> Void

    @State private var pending: PendingTeamConfirmation?
    @State private var started = false
    @State private var error = ""

    private var color: Color {
        switch light.state {
        case .idle: return .green
        case .building: return .accentColor
        case .offline: return .red
        }
    }

    private var detail: String {
        let summary: String = light.summary.prefix(1).uppercased() + light.summary.dropFirst()
        guard light.state == .building, light.sinceMs > 0 else { return summary }
        return "\(summary), started \(InsightsTime.relative(light.sinceMs))"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 12) {
                HStack(spacing: 12) {
                    Image(systemName: "laptopcomputer")
                        .frame(width: 24)
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Office MacBook")
                        Text(detail).font(.caption).foregroundStyle(.secondary)
                    }
                }
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(light.label))
                Spacer()
                if shortcuts != nil {
                    ForEach(BuildMacShortcut.allCases) { shortcut in
                        button(shortcut)
                    }
                }
                FleetStateDot(color: color)
            }
            if !error.isEmpty {
                Label(error, systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundStyle(.red)
                    .fmMessageTools(error)
            }
        }
        .sheet(
            item: $pending,
            onDismiss: {
                if started {
                    started = false
                    onStarted()
                }
            }
        ) { item in
            NavigationStack {
                TeamConfirmSheet(pending: item) { confirmation in
                    guard let shortcuts else { throw LocalControlException("unsupported") }
                    return try await shortcuts.run(confirmation)
                } onDone: { _ in
                    started = true
                }
            }
        }
    }

    private func running(_ shortcut: BuildMacShortcut) -> Bool {
        shortcuts?.isRunning(repo: BuildMacShortcut.repo, file: shortcut.file) == true
    }

    private func button(_ shortcut: BuildMacShortcut) -> some View {
        Button {
            Task { await prepare(shortcut) }
        } label: {
            VStack(spacing: 2) {
                if running(shortcut) {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: shortcut.symbol)
                }
                Text(shortcut.caption).font(.caption2)
            }
            .frame(minWidth: 40, minHeight: 36)
        }
        .buttonStyle(.bordered)
        .accessibilityLabel(Text(shortcut.hint))
    }

    private func prepare(_ shortcut: BuildMacShortcut) async {
        guard let shortcuts else { return }
        do {
            pending = PendingTeamConfirmation(
                title: "Run \(shortcut.name)",
                confirmation: try await shortcuts.prepare(repo: BuildMacShortcut.repo, entry: shortcut.entry))
            error = ""
        } catch {
            self.error = TeamErrorText.describe(error)
        }
    }
}

private struct FleetLightRow: View {
    let light: FleetLight
    let agents: Int

    private var state: String {
        switch light.kind {
        case .core: return light.healthy ? "Operational" : "Has an issue"
        case .runner: return light.healthy ? "Signed in" : "Not signed in or not responding"
        }
    }

    private var detail: String {
        light.healthy || light.reason.isEmpty ? state : "\(state): \(light.reason)"
    }

    private var agentsLabel: String {
        agents == 1 ? "1 agent" : "\(agents) agents"
    }

    var body: some View {
        HStack(spacing: 12) {
            Image(systemName: light.kind == .core ? "server.rack" : "cpu")
                .foregroundStyle(light.healthy ? Color.green : Color.red)
                .frame(width: 24)
            VStack(alignment: .leading, spacing: 2) {
                Text(light.name)
                Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(3)
            }
            Spacer()
            if agents > 0 {
                Text(agentsLabel)
                    .font(.caption.weight(.semibold))
                    .monospacedDigit()
                    .foregroundStyle(Color.accentColor)
            }
            FleetStateDot(color: light.healthy ? .green : .red)
        }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(agents > 0 ? "\(light.label), running \(agentsLabel)" : light.label))
    }
}

private struct DesktopDiscoverySection: View {
    let control: LocalControlService

    var body: some View {
        let card = LocalDesktopText.card(control)
        let hosts = control.hosts
        let blocked = Services.get(LocalMeshService.self)?.localNetworkBlocked ?? false
        Section {
            if control.status == .idle {
                Text(hosts.isEmpty ? LocalDesktopText.hostsEmpty(blocked: blocked) : card.detail)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            } else {
                VStack(alignment: .leading, spacing: 4) {
                    Text(card.title).font(.headline)
                    if !card.detail.isEmpty {
                        Text(card.detail).font(.footnote).foregroundStyle(.secondary)
                            .fmMessageTools(card.detail)
                    }
                    if control.status == .awaitingApproval, let code = LocalDesktopText.spacedCode(control.pairingCode) {
                        Text(code).font(.system(size: 34, weight: .semibold, design: .monospaced))
                    }
                }
                .padding(.vertical, 4)
            }
            ForEach(hosts) { host in
                HStack {
                    Image(systemName: "desktopcomputer")
                    VStack(alignment: .leading, spacing: 2) {
                        Text(host.label)
                        Text(LocalDesktopText.hostSubtitle(host)).font(.caption).foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button(control.isPaired(host.nodeId) ? "Connect" : "Pair") {
                        Task { await control.connect(host) }
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(control.status == .connecting || control.status == .awaitingApproval)
                }
            }
        } header: {
            Text("On this network")
        } footer: {
            Text(
                "A desktop signed in to your account connects by itself. Once connected, it works from anywhere, encrypted end to end through the relay."
            )
        }
    }
}

struct DesktopDestination: View {
    let id: String
    let section: String?

    var body: some View {
        if let hub = DesktopRemoteFeature.hub, let diagnostics = DesktopRemoteFeature.diagnostics,
            let files = DesktopRemoteFeature.files, let fleet = DesktopRemoteFeature.fleet,
            let update = DesktopRemoteFeature.update
        {
            switch section {
            case "files":
                FileRootsScreen(store: files)
            case "log":
                DesktopLogScreen(tail: DesktopLogTail(store: diagnostics))
            case "stalls":
                StallsScreen(store: diagnostics)
            case "processes", "memory":
                ProcessesScreen(store: diagnostics)
            case "fleet":
                FleetOperationsScreen(store: fleet, hub: hub)
            default:
                DesktopScreen(hub: hub, diagnostics: diagnostics, files: files, fleet: fleet, update: update)
            }
        } else {
            DesktopUnavailableView()
        }
    }
}

struct DesktopScreen: View {
    @Bindable var hub: DesktopControlHub
    @Bindable var diagnostics: DiagnosticsStore
    let files: FilesBrowserStore
    let fleet: FleetStore
    let update: DesktopUpdateStore
    @State private var pendingUpdate: PendingConfirmation?
    @State private var pendingComputerRestart: PendingConfirmation?

    var body: some View {
        List {
            Section {
                DesktopReachLine(reach: hub.reach)
                if let transport = hub.transport {
                    TransportBadge(transport: transport)
                }
                if diagnostics.unsupported {
                    DesktopOutdatedRow()
                }
            }
            if let health = diagnostics.health {
                Section {
                    HStack {
                        HealthLevelBadge(level: health.level)
                        Spacer()
                        Text(health.agentsLine)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                    DesktopHealthGauges(health: health, compact: false)
                    if !health.pressureLine.isEmpty {
                        Text(health.pressureLine)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    Text("Health")
                }
            } else if diagnostics.loading {
                Section { ProgressView() }
            }
            Section("On the desktop") {
                NavigationLink {
                    FileRootsScreen(store: files)
                        .shellPageClearance()
                } label: {
                    Label("Files", systemImage: "folder")
                }
                NavigationLink {
                    DesktopLogScreen(tail: DesktopLogTail(store: diagnostics))
                        .shellPageClearance()
                } label: {
                    Label("Log", systemImage: "text.alignleft")
                }
                NavigationLink {
                    StallsScreen(store: diagnostics)
                        .shellPageClearance()
                } label: {
                    HStack {
                        Label("UI stalls", systemImage: "hourglass")
                        Spacer()
                        if let stalls = diagnostics.health?.stalls, stalls > 0 {
                            Text("\(stalls)").foregroundStyle(.secondary)
                        }
                    }
                }
                NavigationLink {
                    ProcessesScreen(store: diagnostics)
                        .shellPageClearance()
                } label: {
                    Label("Memory and processes", systemImage: "memorychip")
                }
            }
            DesktopUpdateSection(
                store: update, pending: $pendingUpdate,
                pendingComputerRestart: $pendingComputerRestart)
            if let backups = diagnostics.backups {
                DesktopBackupsSection(backups: backups)
            }
            Section("Fleet") {
                NavigationLink(value: Route.hosts) {
                    Label("Hosts", systemImage: "server.rack")
                }
                NavigationLink {
                    FleetOperationsScreen(store: fleet, hub: hub)
                        .shellPageClearance()
                } label: {
                    Label("Fleet operations", systemImage: "square.stack.3d.up")
                }
            }
            if !diagnostics.error.isEmpty && !diagnostics.unsupported {
                Section {
                    Label(diagnostics.error, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fmMessageTools(diagnostics.error)
                }
            }
        }
        .navigationTitle(hub.desktop?.name ?? "Desktop")
        .fmRefreshable {
            await diagnostics.refreshHealth()
            await diagnostics.refreshBackups()
            await update.refresh()
        }
        .task {
            await update.refresh()
            await diagnostics.refreshHealth()
            await diagnostics.refreshBackups()
        }
        .onDisappear {
            Task { await update.unwatch() }
        }
        .sheet(item: $pendingUpdate) { item in
            NavigationStack {
                RemoteConfirmSheet(title: item.title, confirmation: item.confirmation, destructive: item.destructive) { confirmation, _ in
                    try await update.run(confirmation)
                }
            }
        }
        .sheet(item: $pendingComputerRestart) { item in
            NavigationStack {
                RemoteConfirmSheet(title: item.title, confirmation: item.confirmation, destructive: item.destructive) { confirmation, _ in
                    try await update.restartComputer(confirmation)
                }
            }
        }
    }
}

struct DesktopBackupsSection: View {
    let backups: DesktopBackupStatus

    var body: some View {
        Section {
            LabeledContent("Status", value: backups.running ? DesktopBackupResult.running.label : backups.result.label)
            if backups.hasBackup {
                LabeledContent("Last backup") {
                    Text(date(backups.lastBackupAtMs), style: .relative)
                }
                LabeledContent("Size", value: backups.sizeLine)
            }
            if backups.enabled && backups.nextDueAtMs > 0 {
                LabeledContent("Next") {
                    Text(date(backups.nextDueAtMs), style: .relative)
                }
            }
            if backups.keep > 0 {
                LabeledContent("Keeps", value: backups.keep == 1 ? "1 copy" : "\(backups.keep) copies")
            }
            if !backups.location.isEmpty {
                LabeledContent("Location", value: backups.location)
            }
            if !backups.errorText.isEmpty {
                Label(backups.errorText, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(backups.errorText)
            }
        } header: {
            Text("Backups")
        }
    }

    private func date(_ ms: Int64) -> Date {
        Date(timeIntervalSince1970: Double(ms) / 1000)
    }
}

struct HealthLevelBadge: View {
    let level: DesktopHealthLevel

    var body: some View {
        Text(level.label)
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .foregroundStyle(tint)
            .background(tint.opacity(0.14), in: Capsule())
    }

    private var tint: Color {
        switch level {
        case .ok: return .green
        case .busy: return .orange
        case .strained: return .red
        case .unknown: return .gray
        }
    }
}

struct DesktopHealthGauges: View {
    let health: FMDesktopRemote.DesktopHealth
    let compact: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: compact ? 6 : 10) {
            gauge("CPU", share: max(0, health.cpuPercent) / 100, detail: RemoteFormat.percent(health.cpuPercent))
            gauge("Memory", share: health.memoryUsedShare, detail: health.memoryLine)
            gauge("Disk", share: health.diskUsedShare, detail: health.diskLine)
            if health.onBattery {
                gauge("Battery", share: health.batteryShare, detail: health.batteryLine,
                      tint: health.batteryShare <= 0.2 ? .red : health.batteryShare <= 0.4 ? .orange : .accentColor)
            }
            if !compact {
                LabeledContent("Load", value: health.loadLine)
                LabeledContent("ForkMesh memory", value: RemoteFormat.bytes(health.appResident))
                if health.pressureValid {
                    LabeledContent(
                        "Memory pressure",
                        value: String(format: "%.1f%% some · %.1f%% full", health.memorySome, health.memoryFull))
                }
                if health.openFiles >= 0 {
                    LabeledContent(
                        "Open files",
                        value: health.openFileLimit > 0
                            ? "\(health.openFiles) of \(health.openFileLimit)" : "\(health.openFiles)")
                }
            }
        }
        .font(compact ? .caption : .subheadline)
    }

    private func gauge(_ title: String, share: Double, detail: String, tint: Color? = nil) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(title)
                Spacer()
                Text(detail).foregroundStyle(.secondary)
            }
            ShareBar(share: share, tint: tint ?? (share > 0.9 ? .red : share > 0.75 ? .orange : .accentColor))
        }
        .accessibilityElement(children: .combine)
    }
}
