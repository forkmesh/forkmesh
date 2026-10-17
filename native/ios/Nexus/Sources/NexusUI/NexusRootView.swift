import SwiftUI
import NexusCore
import NexusKit
import NexusStore

@MainActor
public final class NexusDashboardModel: ObservableObject {
    @Published public var devices: [Device] = []
    @Published public var scans: [ScanSession] = []
    @Published public var discoveries: [DiscoverySession] = []
    @Published public var progress: Double = 0
    @Published public var activity = "Ready"
    @Published public var error: String?
    public let client: NexusClient
    private let store: NexusStore?

    public init(client: NexusClient = NexusClient(), store: NexusStore? = nil) {
        self.client = client
        self.store = store
    }

    public func loadHistory() {
        guard let store else { return }
        do { devices = try store.devices(); scans = try store.scans(); discoveries = try store.discoveries() } catch { self.error = error.localizedDescription }
    }

    public func discover() {
        activity = "Discovering services"
        progress = 0
        Task {
            do {
                let stream = await client.discover()
                var latest: [Device] = []
                for try await update in stream {
                    latest = update.devices
                    devices = latest
                    progress = Double(update.completedServiceTypes) / Double(update.totalServiceTypes)
                    activity = "Discovering \(update.serviceType)"
                }
                try? store?.saveDevices(latest)
                activity = "Discovery complete"
            } catch { self.error = error.localizedDescription; activity = "Discovery stopped" }
        }
    }

    public func scan(target: String, ports: PortList) {
        activity = "Preparing scan"
        progress = 0
        Task {
            do {
                let stream = try await client.scan(target: target, ports: ports)
                var session = ScanSession(target: target, ports: ports)
                for try await update in stream {
                    progress = update.fraction
                    activity = "Scanning \(update.completed) of \(update.total) ports"
                    if let result = update.result { session.results.append(result); session.counts[result.state] += 1 }
                }
                session.status = .completed
                session.finishedAt = Date()
                scans.insert(session, at: 0)
                try? store?.saveScan(session)
                activity = "Scan complete"
            } catch { self.error = error.localizedDescription; activity = "Scan stopped" }
        }
    }
}

public struct NexusRootView: View {
    @StateObject private var model: NexusDashboardModel

    public init(client: NexusClient = NexusClient(), store: NexusStore? = nil) {
        _model = StateObject(wrappedValue: NexusDashboardModel(client: client, store: store))
    }

    public var body: some View {
        TabView {
            NexusOverviewView(model: model).tabItem { Label("Overview", systemImage: "waveform.path.ecg") }
            NexusDiscoverView(model: model).tabItem { Label("Discover", systemImage: "dot.radiowaves.left.and.right") }
            NexusScanView(model: model).tabItem { Label("Scan", systemImage: "scope") }
            NexusInspectView(model: model).tabItem { Label("Inspect", systemImage: "magnifyingglass") }
            NexusHistoryView(model: model).tabItem { Label("History", systemImage: "clock.arrow.circlepath") }
            NexusSettingsView().tabItem { Label("Settings", systemImage: "gearshape") }
        }
        .tint(.cyan)
        .preferredColorScheme(.dark)
        .task { model.loadHistory() }
        .alert("NEXUS", isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })) { Button("OK", role: .cancel) {} } message: { Text(model.error ?? "") }
    }
}
