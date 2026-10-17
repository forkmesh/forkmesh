import SwiftUI
import NexusCore
import NexusKit
import NexusWeb

struct NexusOverviewView: View {
    @ObservedObject var model: NexusDashboardModel

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack(spacing: 16) {
                        Metric(value: "\(model.devices.count)", label: "Devices", symbol: "network")
                        Metric(value: "\(model.scans.first?.counts.open ?? 0)", label: "Open ports", symbol: "lock.open")
                        Metric(value: "\(model.scans.count)", label: "Scans", symbol: "scope")
                    }
                    if model.progress > 0 && model.progress < 1 { ProgressView(model.activity, value: model.progress) } else { Label(model.activity, systemImage: "bolt.horizontal.circle") }
                }
                Section("Notable observations") {
                    let findings = model.scans.prefix(3).flatMap { SecurityInsights.portChanges(current: $0, previous: model.scans.dropFirst().first) }
                    if findings.isEmpty { ContentUnavailableView("No observations yet", systemImage: "checkmark.shield", description: Text("Run a discovery or scan to build a network picture.")) }
                    ForEach(findings) { finding in FindingRow(finding: finding) }
                }
                Section("Recent devices") {
                    ForEach(model.devices.prefix(5)) { device in NavigationLink(value: device) { DeviceRow(device: device) } }
                    if model.devices.isEmpty { Text("No devices discovered yet.").foregroundStyle(.secondary) }
                }
            }
            .navigationTitle("NEXUS")
            .navigationDestination(for: Device.self) { DeviceDetailView(device: $0) }
        }
    }
}

struct NexusDiscoverView: View {
    @ObservedObject var model: NexusDashboardModel
    @State private var query = ""

    var filtered: [Device] { query.isEmpty ? model.devices : model.devices.filter { $0.searchText.localizedCaseInsensitiveContains(query) } }

    var body: some View {
        NavigationStack {
            List(filtered) { device in NavigationLink(value: device) { DeviceRow(device: device) } }
                .overlay { if filtered.isEmpty { ContentUnavailableView("No devices", systemImage: "dot.radiowaves.left.and.right", description: Text("Browse your authorized local network with Bonjour.")) } }
                .navigationTitle("Discover")
                .searchable(text: $query, prompt: "Search devices, services, or addresses")
                .toolbar { ToolbarItem(placement: .primaryAction) { Button(action: model.discover) { Label("Discover", systemImage: "arrow.clockwise") } } }
                .navigationDestination(for: Device.self) { DeviceDetailView(device: $0) }
        }
    }
}

struct NexusScanView: View {
    @ObservedObject var model: NexusDashboardModel
    @State private var target = ""
    @State private var portText = PortPreset.quick.ports.description
    @State private var parseError: String?

    var body: some View {
        NavigationStack {
            Form {
                Section("Target") { TextField("Host, IP, range, or CIDR", text: $target).textInputAutocapitalization(.never).autocorrectionDisabled() }
                Section("Ports") {
                    TextField("Ports", text: $portText).keyboardType(.numbersAndPunctuation)
                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack { ForEach([PortPreset.quick, .common, .web, .remoteAccess, .fileSharing]) { preset in Button(preset.title) { portText = preset.ports.description }.buttonStyle(.bordered) } }
                    }
                }
                Section { Button { begin() } label: { Label("Start TCP connect scan", systemImage: "play.fill").frame(maxWidth: .infinity) }.disabled(target.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty) }
                Section("Recent scans") { ForEach(model.scans) { session in ScanRow(session: session) } }
            }
            .navigationTitle("Scan")
            .alert("Scan setup", isPresented: Binding(get: { parseError != nil }, set: { if !$0 { parseError = nil } })) { Button("OK", role: .cancel) {} } message: { Text(parseError ?? "") }
        }
    }

    private func begin() {
        do { model.scan(target: target, ports: try PortList(parsing: portText)) } catch { parseError = String(describing: error) }
    }
}

struct NexusInspectView: View {
    @ObservedObject var model: NexusDashboardModel
    @State private var dnsName = ""
    @State private var dnsResult: DNSLookup?
    @State private var urlText = "https://"
    @State private var webResult: HTTPInspection?
    @State private var error: String?

    var body: some View {
        NavigationStack {
            List {
                Section("DNS explorer") {
                    HStack { TextField("Hostname", text: $dnsName).textInputAutocapitalization(.never).autocorrectionDisabled(); Button("Lookup") { lookupDNS() }.disabled(dnsName.isEmpty) }
                    if let dnsResult { Text("\(dnsResult.answers.count) answers · \(Int(dnsResult.latencyMilliseconds)) ms via \(dnsResult.resolver)").font(.caption).foregroundStyle(.secondary); ForEach(dnsResult.answers) { record in LabeledContent("\(record.type.title) · \(record.name)", value: record.value) } }
                }
                Section("HTTP and TLS") {
                    HStack { TextField("https://example.com", text: $urlText).textInputAutocapitalization(.never).autocorrectionDisabled(); Button("Inspect") { inspectWeb() } }
                    if let webResult { LabeledContent("Status", value: String(webResult.statusCode)); LabeledContent("Time", value: "\(Int(webResult.elapsedMilliseconds)) ms"); ForEach(webResult.securityObservations) { FindingRow(finding: $0) } }
                }
                Section("iOS capability") { Label("TCP reachability and DNS are native. ICMP and raw-packet traceroute are unavailable to App Store iOS apps.", systemImage: "info.circle").font(.footnote) }
            }
            .navigationTitle("Inspect")
            .alert("Inspection", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) { Button("OK", role: .cancel) {} } message: { Text(error ?? "") }
        }
    }

    private func lookupDNS() { Task { do { dnsResult = try await model.client.dnsLookup(dnsName, type: .a) } catch { self.error = error.localizedDescription } } }
    private func inspectWeb() { Task { guard let url = URL(string: urlText) else { error = "Enter a valid URL."; return }; do { webResult = try await model.client.inspectWeb(url) } catch { self.error = error.localizedDescription } } }
}

struct NexusHistoryView: View {
    @ObservedObject var model: NexusDashboardModel

    var body: some View {
        NavigationStack {
            List {
                Section("Scans") { ForEach(model.scans) { ScanRow(session: $0) } }
                Section("Discoveries") { ForEach(model.discoveries) { session in LabeledContent(session.status.title, value: "\(session.devices.count) devices") } }
            }
            .navigationTitle("History")
            .overlay { if model.scans.isEmpty && model.discoveries.isEmpty { ContentUnavailableView("No history", systemImage: "clock.arrow.circlepath", description: Text("Completed scans and discoveries appear here.")) } }
        }
    }
}

struct NexusSettingsView: View {
    @AppStorage("nexus.allowPublicTargets") private var allowPublicTargets = false
    @AppStorage("nexus.timeout") private var timeout = 1.5
    @AppStorage("nexus.concurrency") private var concurrency = 48

    var body: some View {
        NavigationStack {
            Form {
                Section("Scan defaults") { Stepper("Timeout \(timeout, specifier: "%.1f") seconds", value: $timeout, in: 0.2...30, step: 0.1); Stepper("\(concurrency) concurrent connections", value: $concurrency, in: 1...128) }
                Section("Authorization") { Toggle("Allow internet targets", isOn: $allowPublicTargets); Text("NEXUS always requires confirmation that you own or are authorized to test an internet target. Internet ranges remain blocked unless explicitly authorized.").font(.footnote).foregroundStyle(.secondary) }
                Section("Privacy and permissions") { Label("Bonjour discovery prompts for Local Network permission when it starts. NEXUS does not read ARP tables, MAC addresses, or device vendors unless a supported source provides them.", systemImage: "hand.raised").font(.footnote) }
            }
            .navigationTitle("Settings")
        }
    }
}

struct Metric: View {
    let value: String; let label: String; let symbol: String
    var body: some View { VStack(alignment: .leading) { Label(label, systemImage: symbol).font(.caption).foregroundStyle(.secondary); Text(value).font(.title2.weight(.semibold)).monospacedDigit() }.frame(maxWidth: .infinity, alignment: .leading) }
}

struct DeviceRow: View {
    let device: Device
    var body: some View { HStack { Image(systemName: device.kind.symbolName).foregroundStyle(.cyan).frame(width: 28); VStack(alignment: .leading) { Text(device.title); Text(device.primaryAddress?.description ?? device.services.first?.type ?? "Bonjour service").font(.caption).foregroundStyle(.secondary) }; Spacer(); if !device.openPorts.isEmpty { Text("\(device.openPorts.count)").font(.caption).foregroundStyle(.secondary) } } }
}

struct DeviceDetailView: View {
    let device: Device
    var body: some View { List { Section("Identity") { LabeledContent("Name", value: device.title); LabeledContent("Type", value: device.kind.title); ForEach(device.addresses, id: \.self) { LabeledContent("Address", value: $0.description) }; ForEach(device.hostnames, id: \.self) { LabeledContent("Hostname", value: $0) } }; Section("Discovered services") { ForEach(device.services) { LabeledContent($0.type, value: $0.name) } }; Section("Open TCP ports") { ForEach(device.openPorts) { LabeledContent("\($0.port) · \($0.serviceName)", value: $0.state.title) } } }.navigationTitle(device.title) }
}

struct ScanRow: View {
    let session: ScanSession
    var body: some View { VStack(alignment: .leading, spacing: 4) { Text(session.target).font(.headline); HStack { Text(session.status.title); Text("\(session.counts.open) open"); Text("\(session.ports.count) ports") }.font(.caption).foregroundStyle(.secondary) } }
}

struct FindingRow: View {
    let finding: SecurityObservation
    var body: some View { VStack(alignment: .leading, spacing: 3) { Label(finding.title, systemImage: finding.severity == .warning ? "exclamationmark.triangle" : "info.circle").foregroundStyle(finding.severity == .warning ? .orange : .primary); Text(finding.detail).font(.caption).foregroundStyle(.secondary) } }
}

#Preview { NexusRootView() }
