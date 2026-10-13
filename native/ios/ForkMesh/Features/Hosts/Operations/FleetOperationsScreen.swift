import FMApi
import FMControl
import FMDesktopRemote
import FMHosts
import SwiftUI

struct FleetOperationsDestination: View {
    var body: some View {
        if let store = DesktopRemoteFeature.fleet, let hub = DesktopRemoteFeature.hub {
            FleetOperationsScreen(store: store, hub: hub)
        } else {
            DesktopUnavailableView(title: "Fleet operations are not available")
        }
    }
}

struct FleetOperationsScreen: View {
    @Bindable var store: FleetStore
    @Bindable var hub: DesktopControlHub

    @State private var pending: PendingConfirmation?
    @State private var preparing = false
    @State private var error = ""
    @State private var banner = ""
    @State private var desired = 2
    @State private var targetOn = false
    @State private var linkCode = ""
    @State private var showAddHost = false
    @State private var showMirrorPush = false

    var body: some View {
        List {
            Section {
                DesktopReachLine(reach: hub.reach)
                if store.unsupported { DesktopOutdatedRow() }
                if !banner.isEmpty {
                    Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green).font(.footnote)
                        .fmMessageTools(banner)
                }
                if !error.isEmpty {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote)
                        .fmMessageTools(error)
                }
            }
            if let summary = store.summary {
                costsSection(summary.costs)
                if summary.provision.isVisible {
                    Section("Deployment") {
                        NavigationLink(value: Route.provision(id: summary.provision.node)) {
                            VStack(alignment: .leading, spacing: 6) {
                                Text(summary.provision.headline).font(.subheadline.weight(.semibold))
                                ProgressView(value: summary.provision.progress)
                                if !summary.provision.detail.isEmpty {
                                    Text(summary.provision.detail)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(2)
                                }
                            }
                        }
                    }
                }
                targetSection(summary.fleet)
                operationsSection(summary)
                hostsSection
                gatewaysSection(summary)
                Section("Mirror client") {
                    LabeledContent("Status", value: summary.mirrorClientStatus.isEmpty ? "—" : summary.mirrorClientStatus)
                    if !summary.mirrorClientHealth.isEmpty {
                        Text(summary.mirrorClientHealth).font(.caption).foregroundStyle(.secondary)
                    }
                    if summary.pushing > 0 || summary.pushPending > 0 {
                        Text("Pushing \(summary.pushing) repositor\(summary.pushing == 1 ? "y" : "ies") · \(summary.pushPending) waiting")
                            .font(.caption)
                    }
                }
                if !summary.lastResult.isEmpty {
                    Section("Desktop's last result") {
                        Text(summary.lastResult).font(.caption).textSelection(.enabled)
                            .fmMessageTools(summary.lastResult)
                    }
                }
            } else if store.loading {
                Section { ProgressView() }
            }
            if !store.outcomes.isEmpty {
                Section("From this phone") {
                    ForEach(store.outcomes) { RemoteOutcomeRow(outcome: $0) }
                }
            }
            if !store.error.isEmpty && !store.unsupported {
                Section {
                    Label(store.error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote)
                        .fmMessageTools(store.error)
                }
            }
        }
        .navigationTitle("Fleet")
        .fmRefreshable { await store.refresh() }
        .task {
            await store.refresh()
            syncTarget()
        }
        .onChange(of: store.summary?.fleet) { _, _ in syncTarget() }
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
        .sheet(isPresented: $showAddHost) {
            NavigationStack {
                AddSshHostSheet { name, address, user in
                    await prepare(.add(name: name, address: address, user: user))
                }
            }
        }
        .sheet(isPresented: $showMirrorPush) {
            NavigationStack {
                MirrorPushPicker(store: store) { repo in
                    showMirrorPush = false
                    Task { await prepare(.mirrorPush(repo: repo)) }
                }
            }
        }
    }

    private func costsSection(_ costs: FleetCosts) -> some View {
        Section {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(RemoteFormat.money(costs.monthlyCost)).font(.title2.weight(.semibold))
                    Text("per month · \(RemoteFormat.money(costs.yearlyCost)) a year")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 2) {
                    Text("\(costs.vultr)").font(.title2.weight(.semibold))
                    Text("Vultr of \(costs.hosts) hosts").font(.caption).foregroundStyle(.secondary)
                }
            }
            ForEach(costs.byRegion) { row in
                LabeledContent(row.key.uppercased(), value: "\(row.count) · \(RemoteFormat.money(row.monthlyCost))")
                    .font(.footnote)
            }
            ForEach(costs.byPlan) { row in
                LabeledContent(row.key, value: "\(row.count) · \(RemoteFormat.money(row.monthlyCost))")
                    .font(.footnote)
            }
        } header: {
            Text("Costs")
        } footer: {
            Text("From the plan prices the desktop saved when it created each server.")
        }
    }

    private func targetSection(_ fleet: FleetTarget) -> some View {
        Section {
            Text(fleet.line).font(.footnote)
            if fleet.available {
                Toggle("Keep a fleet size", isOn: $targetOn)
                Stepper("Healthy mirrors: \(desired)", value: $desired, in: 1...12)
                    .disabled(!targetOn)
                Button("Apply") {
                    Task { await prepare(.fleetTarget(enabled: targetOn, desired: desired)) }
                }
                .disabled(preparing || (targetOn == fleet.enabled && desired == fleet.desired))
                Button("Check the fleet now") {
                    Task { await prepare(.reconcile) }
                }
                .disabled(preparing || !fleet.enabled || fleet.reconciling)
            }
            if !fleet.status.isEmpty {
                Text(fleet.status).font(.caption).foregroundStyle(.secondary)
                    .fmMessageTools(fleet.status)
            }
        } header: {
            Text("Fleet target")
        }
    }

    private func operationsSection(_ summary: FleetSummary) -> some View {
        Section {
            Menu {
                ForEach(FleetDeployMode.allCases) { mode in
                    Button(mode.title, role: mode == .reinstall ? .destructive : nil) {
                        Task { await prepare(.deployAll(mode)) }
                    }
                }
            } label: {
                Label("Deploy to all hosts", systemImage: "square.and.arrow.down.on.square")
            }
            .disabled(preparing || summary.busy)
            if summary.provision.resumable {
                Button {
                    Task { await prepare(.resume) }
                } label: {
                    Label("Resume the paused deployment", systemImage: "play.circle")
                }
                .disabled(preparing)
            }
            Button {
                showMirrorPush = true
            } label: {
                Label("Mirror push a repository", systemImage: "arrow.up.to.line")
            }
            Button {
                showAddHost = true
            } label: {
                Label("Add an SSH host", systemImage: "plus.circle")
            }
            HStack {
                TextField("Link code", text: $linkCode)
                    .keyboardType(.numberPad)
                    .font(.system(.body, design: .monospaced))
                Button("Link node") {
                    Task { await prepare(.linkCode(linkCode)) }
                }
                .disabled(preparing || !FleetInput.validLinkCode(linkCode))
            }
        } header: {
            Text("Operations")
        } footer: {
            Text("A node being installed shows a six-digit link code; entering it here registers the node under your account.")
        }
    }

    @ViewBuilder
    private var hostsSection: some View {
        if let hosts = HostsFeature.hosts?.snapshot?.sorted, !hosts.isEmpty {
            Section("Per host") {
                ForEach(hosts) { host in
                    NavigationLink {
                        HostOperationsScreen(store: store, host: host)
                            .shellPageClearance()
                    } label: {
                        HStack {
                            HealthDot(health: host.health)
                            Text(host.name)
                            Spacer()
                            Text(host.planLine).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
        }
    }

    @ViewBuilder
    private func gatewaysSection(_ summary: FleetSummary) -> some View {
        if !summary.gateways.isEmpty {
            Section("Unreachable mirror gateways") {
                ForEach(summary.gateways) { gateway in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(gateway.host).font(.subheadline)
                        Text(gateway.line).font(.caption).foregroundStyle(gateway.retired ? Color.secondary : Color.orange)
                            .fmMessageTools(gateway.line)
                    }
                }
            }
        }
    }

    private func syncTarget() {
        guard let fleet = store.summary?.fleet else { return }
        targetOn = fleet.enabled
        desired = max(1, fleet.desired)
    }

    private func prepare(_ action: FleetAction) async {
        preparing = true
        defer { preparing = false }
        do {
            let confirmation = try await store.prepare(action)
            pending = PendingConfirmation(title: action.title, confirmation: confirmation, destructive: action.isDestructive)
            error = ""
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct AddSshHostSheet: View {
    @Environment(\.dismiss) private var dismiss
    let submit: (String, String, String) async -> Void

    @State private var name = ""
    @State private var address = ""
    @State private var user = "root"

    private var valid: Bool {
        FleetInput.validHostName(name) && FleetInput.validAddress(address) && FleetInput.validUser(user)
    }

    var body: some View {
        Form {
            Section {
                TextField("Name", text: $name)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                TextField("IP address or host name", text: $address)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .keyboardType(.URL)
                TextField("SSH user", text: $user)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
            } footer: {
                Text("The desktop reaches the host with the shared ForkMesh SSH key. If it needs a password, the desktop asks for it there; passwords never travel to or from this phone.")
            }
        }
        .navigationTitle("Add host")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button("Next") {
                    let values = (name, address, user)
                    dismiss()
                    Task { await submit(values.0, values.1, values.2) }
                }
                .disabled(!valid)
            }
        }
    }
}

struct MirrorPushPicker: View {
    @Environment(\.dismiss) private var dismiss
    @Bindable var store: FleetStore
    let pick: (String) -> Void
    @State private var filter = ""

    private var repos: [FleetRepo] {
        let needle = filter.trimmingCharacters(in: .whitespaces)
        return needle.isEmpty ? store.repos : store.repos.filter { $0.repo.localizedCaseInsensitiveContains(needle) }
    }

    var body: some View {
        List(repos) { repo in
            Button {
                pick(repo.repo)
            } label: {
                HStack {
                    Image(systemName: repo.isPrivate ? "lock" : "book.closed")
                    Text(repo.repo)
                    Spacer()
                    if repo.pushing { ProgressView() }
                }
            }
            .disabled(repo.pushing)
        }
        .searchable(text: $filter)
        .navigationTitle("Mirror push")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
        }
        .task { await store.loadRepos() }
    }
}
