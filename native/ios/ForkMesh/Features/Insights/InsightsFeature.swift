import FMAccount
import FMApi
import FMBase
import FMChat
import FMControl
import FMInsights
import Foundation
import LocalAuthentication
import SwiftUI

@MainActor
enum InsightsFeature {
    static var api: ApiService? { Services.get(ApiService.self) }

    static var isAdmin: Bool { Services.get(AuthService.self)?.session?.isAdmin ?? false }

    static var hostLabel: String {
        guard let settings = Services.get(SettingsService.self) else { return "" }
        return settings.relays.active.host
    }

    static var projectScope: String {
        Services.get(SettingsService.self)?.relays.activeId ?? ""
    }

    static var desktopClient: DesktopStatsClient? {
        guard let hub = Services.get(DesktopControlHub.self) else { return nil }
        return DesktopStatsClient(channel: hub)
    }

    static let desktopLoadLight: DesktopLoadLightModel? = {
        guard let hub = HostsFeature.hub else { return nil }
        let model = DesktopLoadLightModel(channel: hub, linkOpen: { [weak hub] in hub?.currentLink != nil })
        hub.addLinkListener { [weak model] in
            guard let model else { return }
            Task { await model.read() }
        }
        return model
    }()

    static let buildMacLight: BuildMacLightModel? = {
        guard let hub = HostsFeature.hub else { return nil }
        let model = BuildMacLightModel(channel: hub, linkOpen: { [weak hub] in hub?.currentLink != nil })
        hub.addLinkListener { [weak model] in
            guard let model else { return }
            Task { await model.refresh() }
        }
        return model
    }()

    static func livePeers() -> [NodeRow] {
        guard let relay = Services.get(RelayService.self) else { return [] }
        return relay.roster().map { NodeRow(member: $0, diagnostics: NodeDiagnosticFinding.parse($0.diagnostics)) }
    }

    static func relayRows() -> [RelayRow] {
        guard let settings = Services.get(SettingsService.self) else { return [] }
        let directory = settings.relays
        return directory.relays.map { RelayRow(profile: $0, activeId: directory.activeId) }
    }

    static var activeServerUrl: String { Services.get(SettingsService.self)?.relays.activeServerUrl ?? "" }

    static func statsLayoutStore() -> StatsLayoutStore? {
        guard let prefs = Services.get(KeyValueStore.self) else { return nil }
        return StatsLayoutStore(store: prefs, scope: projectScope)
    }

    static func statsHistoryStore() -> StatsHistoryStore? {
        guard let prefs = Services.get(KeyValueStore.self) else { return nil }
        return StatsHistoryStore(store: prefs, scope: projectScope)
    }

    static let crashStore = CrashReportStore.forDevice()

    static func confirmOwner(_ reason: String) async -> Bool {
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else { return false }
        do {
            return try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        } catch {
            return false
        }
    }
}

extension StatsTileStatus {
    var color: Color {
        switch self {
        case .good: return .green
        case .warning: return .orange
        case .critical: return .red
        case .none: return .secondary
        }
    }

    var symbol: String {
        switch self {
        case .good: return "checkmark.circle.fill"
        case .warning: return "exclamationmark.circle.fill"
        case .critical: return "xmark.octagon.fill"
        case .none: return "circle"
        }
    }

    var spoken: String {
        switch self {
        case .good: return "healthy"
        case .warning: return "needs attention"
        case .critical: return "failing"
        case .none: return ""
        }
    }
}

struct InsightsUnavailableView: View {
    let title: String
    let systemImage: String
    let message: String

    var body: some View {
        ContentUnavailableView {
            Label(title, systemImage: systemImage)
        } description: {
            VStack(spacing: 8) {
                Text(message)
                FmMessageTools("\(title). \(message)")
            }
        }
    }
}

struct InsightsErrorView: View {
    let title: String
    let error: InsightsError
    let retry: () async -> Void
    @State private var showDetails = false

    var body: some View {
        ContentUnavailableView {
            Label(title, systemImage: error.needsAdmin ? "lock" : "exclamationmark.triangle")
        } description: {
            VStack(spacing: 8) {
                Text(error.message)
                FmMessageTools(error.code.isEmpty ? error.message : "\(error.message) (\(error.code))")
                if showDetails {
                    Text(error.code).font(.caption.monospaced()).foregroundStyle(.secondary)
                }
            }
        } actions: {
            Button("Retry") { Task { await retry() } }
                .buttonStyle(.borderedProminent)
            Button(showDetails ? "Hide details" : "Details") { showDetails.toggle() }
                .font(.footnote)
        }
    }
}

struct InsightsBanner: View {
    let text: String
    let systemImage: String
    var tint: Color = .orange

    var body: some View {
        Label(text, systemImage: systemImage)
            .font(.footnote)
            .foregroundStyle(tint)
            .accessibilityElement(children: .combine)
            .fmMessageTools(text)
    }
}

struct InsightsLoadingRows: View {
    let rows: Int
    @State private var shown = false

    var body: some View {
        Group {
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text("Loading…").font(.footnote).foregroundStyle(.secondary)
            }
            if shown {
                ForEach(0..<max(rows - 1, 0), id: \.self) { _ in
                    HStack {
                        Text("Loading placeholder row")
                        Spacer()
                        Text("000")
                    }
                    .redacted(reason: .placeholder)
                }
            }
        }
        .task {
            try? await Task.sleep(nanoseconds: 300_000_000)
            shown = true
        }
        .accessibilityLabel("Loading")
    }
}

enum InsightsTime {
    static func relative(_ ms: Int64) -> String {
        guard ms > 0 else { return "never" }
        return Format.relativeTime(ms)
    }

    static func date(_ ms: Int64) -> Date { Date(timeIntervalSince1970: Double(ms) / 1000) }
}
