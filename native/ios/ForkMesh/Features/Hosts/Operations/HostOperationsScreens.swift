import FMApi
import FMDesktopRemote
import FMHosts
import SwiftUI

struct ProvisionDestination: View {
    let id: String

    var body: some View {
        if let store = DesktopRemoteFeature.fleet {
            ProvisionScreen(store: store, node: id)
        } else {
            DesktopUnavailableView(title: "Deployments are not available")
        }
    }
}

struct ProvisionScreen: View {
    @Bindable var store: FleetStore
    let node: String

    @State private var status: ProvisionStatus?
    @State private var error = ""
    @State private var pending: PendingConfirmation?

    var body: some View {
        List {
            if let status {
                Section {
                    Text(status.headline).font(.headline)
                    ProgressView(value: status.progress)
                    if !status.detail.isEmpty { Text(status.detail).font(.footnote) }
                    if !status.message.isEmpty {
                        Text(status.message).font(.footnote).foregroundStyle(.secondary)
                            .fmMessageTools(status.message)
                    }
                    if !status.gap.isEmpty {
                        Label(status.gap, systemImage: "hourglass").font(.caption)
                    }
                    if status.attempts > 0 {
                        LabeledContent("Install attempts", value: "\(status.attempts)")
                    }
                }
                Section("Stages") {
                    ForEach(status.stages) { stage in
                        HStack(alignment: .top, spacing: 10) {
                            stageIcon(stage.state)
                                .frame(width: 22)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("\(stage.number). \(stage.name)")
                                    .font(.subheadline.weight(stage.state == .active ? .semibold : .regular))
                                if !stage.detail.isEmpty {
                                    Text(stage.detail).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
                if status.resumable {
                    Section {
                        Button {
                            Task { await resume() }
                        } label: {
                            Label("Resume deployment", systemImage: "play.circle")
                        }
                    } footer: {
                        Text("The deployment continues where it stopped; no second server is created.")
                    }
                }
                if !status.log.isEmpty {
                    Section("Deployment log") {
                        ForEach(Array(status.log.enumerated()), id: \.offset) { _, line in
                            Text(line)
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                    }
                }
                if !status.isVisible && status.node.isEmpty {
                    Section {
                        Text("No deployment is running.").foregroundStyle(.secondary)
                    }
                }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote)
                        .fmMessageTools(error)
                }
            }
        }
        .overlay {
            if status == nil && error.isEmpty { ProgressView() }
        }
        .navigationTitle(node.isEmpty ? "Deployment" : node)
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await load() }
        .task { await load() }
        .onChange(of: store.summary?.provision) { _, _ in Task { await load() } }
        .sheet(item: $pending) { item in
            NavigationStack {
                RemoteConfirmSheet(title: item.title, confirmation: item.confirmation) { confirmation, typed in
                    try await store.run(confirmation, typed: typed)
                }
            }
        }
    }

    @ViewBuilder
    private func stageIcon(_ state: ProvisionStageState) -> some View {
        switch state {
        case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .active: ProgressView()
        case .failed: Image(systemName: "xmark.octagon.fill").foregroundStyle(.red)
        case .pending: Image(systemName: "circle").foregroundStyle(.secondary)
        }
    }

    private func load() async {
        do {
            status = try await store.provision()
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    private func resume() async {
        do {
            pending = PendingConfirmation(
                title: FleetAction.resume.title, confirmation: try await store.prepare(.resume), destructive: false)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct HostOperationsScreen: View {
    @Bindable var store: FleetStore
    let host: HostRow

    @State private var pending: PendingConfirmation?
    @State private var error = ""
    @State private var banner = ""
    @State private var preparing = false

    var body: some View {
        List {
            Section {
                HStack(spacing: 10) {
                    HealthDot(health: host.health)
                    Text(host.healthLine).font(.headline)
                }
                if !host.address.isEmpty { LabeledContent("Address", value: host.address) }
                if !host.planLine.isEmpty { LabeledContent("Plan", value: host.planLine) }
            }
            Section {
                NavigationLink {
                    DiskUsageScreen(store: store, host: host.name, path: "/")
                        .shellPageClearance()
                } label: {
                    Label("Disk usage", systemImage: "internaldrive")
                }
                actionButton(.agentClis(host: host.name), icon: "terminal")
                actionButton(.actions(host: host.name, enabled: true), icon: "play.rectangle")
                actionButton(.actions(host: host.name, enabled: false), icon: "pause.rectangle")
            } header: {
                Text("Operations")
            } footer: {
                Text("Actions variables and provider sign-ins stay on the desktop; set them there.")
            }
            Section {
                actionButton(.uninstall(host: host.name), icon: "trash.slash")
                actionButton(.forget(host: host.name), icon: "minus.circle")
            } header: {
                Text("Remove")
            } footer: {
                Text("To destroy the server itself, use Destroy on the host's page.")
            }
            let outcomes = store.outcomes.filter { $0.subject.hasPrefix(host.id) }
            if !outcomes.isEmpty {
                Section("From this phone") {
                    ForEach(outcomes) { RemoteOutcomeRow(outcome: $0) }
                }
            }
            if !banner.isEmpty {
                Section { Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green).fmMessageTools(banner) }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote)
                        .fmMessageTools(error)
                }
            }
        }
        .navigationTitle(host.name)
        .navigationBarTitleDisplayMode(.inline)
        .sheet(item: $pending) { item in
            NavigationStack {
                RemoteConfirmSheet(
                    title: item.title, confirmation: item.confirmation, destructive: item.destructive
                ) { confirmation, typed in
                    try await store.run(confirmation, typed: typed)
                } onDone: { message in
                    banner = message
                }
            }
        }
    }

    private func actionButton(_ action: FleetAction, icon: String) -> some View {
        Button(role: action.isDestructive ? .destructive : nil) {
            Task { await prepare(action) }
        } label: {
            HStack {
                Label(action.title, systemImage: icon)
                Spacer()
                if store.isRunning(action: action.wireName, subject: host.id) { ProgressView() }
            }
        }
        .disabled(preparing || store.isRunning(action: action.wireName, subject: host.id))
    }

    private func prepare(_ action: FleetAction) async {
        preparing = true
        defer { preparing = false }
        do {
            pending = PendingConfirmation(
                title: action.title, confirmation: try await store.prepare(action), destructive: action.isDestructive)
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct DiskUsageScreen: View {
    let store: FleetStore
    let host: String
    let path: String

    @State private var usage: DiskUsage?
    @State private var error = ""

    var body: some View {
        List {
            if let usage {
                Section {
                    LabeledContent("Total", value: RemoteFormat.bytes(usage.total))
                }
                Section {
                    ForEach(usage.children) { entry in
                        NavigationLink {
                            DiskUsageScreen(store: store, host: host, path: entry.path)
                                .shellPageClearance()
                        } label: {
                            VStack(alignment: .leading, spacing: 4) {
                                HStack {
                                    Text(entry.name).lineLimit(1)
                                    Spacer()
                                    Text(RemoteFormat.bytes(entry.bytes))
                                        .font(.subheadline.monospacedDigit())
                                        .foregroundStyle(.secondary)
                                }
                                ShareBar(share: usage.share(of: entry))
                            }
                        }
                    }
                    if usage.children.isEmpty {
                        Text("Nothing below this folder, or it could not be read.").foregroundStyle(.secondary)
                    }
                } footer: {
                    Text("Read-only: one `du` level over the desktop's SSH connection to the host.")
                }
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote)
                        .fmMessageTools(error)
                }
            }
        }
        .overlay {
            if usage == nil && error.isEmpty {
                VStack(spacing: 8) {
                    ProgressView()
                    Text("Measuring \(path)…").font(.footnote).foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle(path)
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await load() }
        .task { await load() }
    }

    private func load() async {
        do {
            usage = try await store.diskUsage(host: host, path: path)
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct NodeDestination: View {
    let name: String

    var body: some View {
        if let store = DesktopRemoteFeature.fleet {
            NodeOperationsScreen(store: store, name: name)
        } else {
            DesktopUnavailableView(title: "Node operations are not available")
        }
    }
}

struct NodeOperationsScreen: View {
    @Bindable var store: FleetStore
    let name: String

    @State private var pending: PendingConfirmation?
    @State private var error = ""
    @State private var banner = ""

    private var host: HostRow? { HostsFeature.hosts?.host(named: name) }

    var body: some View {
        List {
            Section {
                LabeledContent("Node", value: name)
                if let host {
                    NavigationLink(value: Route.host(name: host.name, section: nil)) {
                        Label("Saved host on the desktop", systemImage: "server.rack")
                    }
                }
            }
            if let summary = store.summary {
                if summary.nodeDeleteInProgress {
                    Section {
                        Label("Deleting \(summary.nodeDeleteTarget)…", systemImage: "hourglass")
                    }
                }
                if summary.admin {
                    Section {
                        Button(role: .destructive) {
                            Task { await prepare() }
                        } label: {
                            Label("Delete node completely", systemImage: "trash")
                        }
                        .disabled(summary.nodeDeleteInProgress)
                    } footer: {
                        Text("Destroys its Vultr server, removes its DNS record, erases it from the relay and forgets the saved host. Face ID and the typed phrase confirm it.")
                    }
                } else {
                    Section {
                        Text("Only a platform admin can delete a node.").foregroundStyle(.secondary)
                    }
                }
            } else if store.loading {
                Section { ProgressView() }
            }
            if !banner.isEmpty {
                Section { Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green).fmMessageTools(banner) }
            }
            if !error.isEmpty || !store.error.isEmpty {
                Section {
                    Label(error.isEmpty ? store.error : error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .fmMessageTools(error.isEmpty ? store.error : error)
                }
            }
        }
        .navigationTitle(name)
        .navigationBarTitleDisplayMode(.inline)
        .task { await store.refresh() }
        .sheet(item: $pending) { item in
            NavigationStack {
                RemoteConfirmSheet(
                    title: item.title, confirmation: item.confirmation, destructive: true
                ) { confirmation, typed in
                    try await store.run(confirmation, typed: typed)
                } onDone: { message in
                    banner = message
                }
            }
        }
    }

    private func prepare() async {
        do {
            let action = FleetAction.deleteNode(name)
            pending = PendingConfirmation(
                title: action.title, confirmation: try await store.prepare(action), destructive: true)
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}
