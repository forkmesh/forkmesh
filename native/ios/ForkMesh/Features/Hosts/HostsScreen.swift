// WS7: the owner's hosts (Vultr mirror nodes, SSH-installed nodes), managed through the desktop
// that holds their keys: on this network, or through the relay from anywhere.
//
// `HostsDestination` is the entry point; the shell (WS3) places it (a Tools menu row, a tab, or a
// NavigationLink) without knowing anything else about this area.

import FMBase
import FMChat
import FMControl
import FMHosts
import SwiftUI

/// The Hosts area as one navigation destination.
struct HostsDestination: View {
    var body: some View {
        if let store = HostsFeature.hosts, let hub = HostsFeature.hub {
            HostsScreen(store: store, hub: hub)
        } else {
            ContentUnavailableView(
                "Hosts are not available", systemImage: "server.rack",
                description: Text("Sign in, then pair your desktop on the same network."))
        }
    }
}

struct HostDestination: View {
    let name: String

    var body: some View {
        if let store = HostsFeature.hosts, let hub = HostsFeature.hub {
            HostDetailScreen(store: store, hub: hub, hostName: name)
        } else {
            ContentUnavailableView(
                "Hosts are not available", systemImage: "server.rack",
                description: Text("Sign in, then pair your desktop on the same network."))
        }
    }
}

struct HostsScreen: View {
    @Bindable var store: HostsStore
    @Bindable var hub: DesktopControlHub
    @State private var showAdd = false
    @State private var banner: HostActionOutcome?

    var body: some View {
        List {
            Section {
                DesktopReachLine(reach: hub.reach)
                if let transport = hub.transport {
                    TransportBadge(transport: transport)
                }
                if let snapshot = store.snapshot {
                    Text(snapshot.summary).font(.subheadline.weight(.semibold))
                    if !snapshot.provisioning.isEmpty {
                        Label("Creating \(snapshot.provisioning)…", systemImage: "hourglass")
                            .font(.footnote)
                    }
                }
                if !store.error.isEmpty {
                    Label(store.error, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fmMessageTools(store.error)
                }
            }
            if let outcome = store.outcomes.first {
                Section("Latest") {
                    OutcomeRow(outcome: outcome)
                }
            }
            if let snapshot = store.snapshot {
                Section("Hosts") {
                    if snapshot.hosts.isEmpty {
                        Text("The desktop has no saved hosts yet.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(snapshot.sorted) { host in
                        NavigationLink {
                            HostDetailScreen(store: store, hub: hub, hostName: host.name)
                                .shellPageClearance()
                        } label: {
                            HostRowView(host: host, running: store.running.contains { $0.hasSuffix(":\(host.id)") })
                        }
                    }
                }
            } else if store.loading {
                Section {
                    HStack {
                        ProgressView()
                        Text("Asking the desktop…").foregroundStyle(.secondary)
                    }
                }
                Section {
                    NavigationLink {
                        FleetOperationsDestination()
                            .shellPageClearance()
                    } label: {
                        Label("Fleet: deploy to all, mirror push, add an SSH host, costs", systemImage: "square.stack.3d.up")
                    }
                }
            } else if case .notPaired = hub.reach {
                Section {
                    Text(
                        "Open ForkMesh on your phone while it is on the same Wi-Fi as your desktop and approve it there. After that, hosts and agents work from anywhere, encrypted end to end through the relay."
                    )
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("Hosts")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    showAdd = true
                } label: {
                    Label("Add host", systemImage: "plus")
                }
                .disabled(!(store.snapshot?.canCreate ?? false) || (store.snapshot?.busy ?? false))
            }
        }
        .fmRefreshable { await store.refresh() }
        .task { await store.refresh() }
        .sheet(isPresented: $showAdd) {
            NavigationStack {
                AddHostSheet(store: store)
            }
        }
    }
}

struct HostRowView: View {
    let host: HostRow
    var running = false

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            HealthDot(health: host.health)
                .padding(.top, 5)
            VStack(alignment: .leading, spacing: 3) {
                HStack {
                    Text(host.name).font(.body.weight(.semibold))
                    if running {
                        ProgressView().controlSize(.mini)
                    }
                }
                Text(host.healthLine)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
                let details = [host.address, host.planLine, host.version.isEmpty ? "" : "v\(host.version)"]
                    .filter { !$0.isEmpty }
                    .joined(separator: " · ")
                if !details.isEmpty {
                    Text(details)
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .lineLimit(1)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }
}

struct HealthDot: View {
    let health: HostHealth

    var body: some View {
        Circle()
            .fill(color)
            .frame(width: 10, height: 10)
            .accessibilityLabel(health.label)
    }

    private var color: Color {
        switch health {
        case .online: return .green
        case .attention: return .orange
        case .offline: return .red
        case .provisioning: return .blue
        case .unknown: return .gray
        }
    }
}

struct OutcomeRow: View {
    let outcome: HostActionOutcome

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Label(outcome.headline, systemImage: outcome.ok ? "checkmark.circle" : "xmark.octagon")
                .foregroundStyle(outcome.ok ? Color.green : Color.red)
                .font(.subheadline.weight(.semibold))
            if !outcome.message.isEmpty {
                Text(outcome.message)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(6)
                    .textSelection(.enabled)
                    .fmMessageTools(outcome.message)
            }
        }
    }
}
