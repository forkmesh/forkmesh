import FMBase
import FMControl
import FMMiniApps
import SwiftUI

struct MiniAppsDestination: View {
    var body: some View {
        if let client = MiniAppsFeature.client {
            MiniAppsScreen(client: client)
        } else {
            DesktopUnavailableView()
        }
    }
}

struct MiniAppsScreen: View {
    let client: MiniAppClient
    @State private var apps: [MiniAppDescriptor] = []
    @State private var problem: String?
    @State private var loading = false

    var body: some View {
        List {
            Section {
                MiniAppReachRow(reach: client.reach, pending: client.outbox.pending.count)
            }
            if let problem {
                Section { Text(problem).font(.footnote).foregroundStyle(.secondary) }
            }
            Section("Services on the desktop") {
                if apps.isEmpty && !loading {
                    Text("No background services share anything with this phone yet.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
                ForEach(apps) { app in
                    NavigationLink {
                        MiniAppScreen(client: client, app: app)
                            .shellPageClearance()
                    } label: {
                        VStack(alignment: .leading, spacing: 2) {
                            HStack {
                                Text(app.name)
                                if app.running {
                                    Image(systemName: "circle.fill")
                                        .font(.system(size: 6))
                                        .foregroundStyle(.green)
                                        .accessibilityLabel("Running")
                                }
                            }
                            if !app.summary.isEmpty {
                                Text(app.summary).font(.footnote).foregroundStyle(.secondary).lineLimit(2)
                            }
                        }
                    }
                    .disabled(!app.hasCard)
                }
            }
        }
        .navigationTitle("Background services")
        .overlay { if loading && apps.isEmpty { ProgressView() } }
        .task { await load() }
        .refreshable { await load() }
    }

    private func load() async {
        loading = true
        defer { loading = false }
        do {
            apps = try await client.catalog()
            client.cachedApps = apps
            problem = nil
        } catch {
            apps = client.cachedApps
            problem = apps.isEmpty
                ? ControlErrorText.describe(error)
                : "Showing the last list; the desktop is not reachable right now."
        }
    }
}

struct MiniAppReachRow: View {
    let reach: MiniAppReach
    let pending: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            switch reach {
            case .live(let transport):
                Label(transport == "relay" ? "Connected through the relay" : "Connected on this network",
                      systemImage: "checkmark.circle")
            case .desktopAway:
                Label("The desktop is away; actions wait until it is back", systemImage: "moon.zzz")
            case .unpaired:
                Label("Pair this phone with your desktop to use its services", systemImage: "link")
            }
            if pending > 0 {
                Text("\(pending) waiting to send").font(.caption).foregroundStyle(.secondary)
            }
        }
        .font(.footnote)
    }
}

struct MiniAppScreen: View {
    let client: MiniAppClient
    let app: MiniAppDescriptor
    @State private var card: MiniAppCard?
    @State private var fetchedMs: Int64 = 0
    @State private var status: String?
    @State private var confirming: MiniAppCard.Action?
    @State private var observer: UUID?

    var body: some View {
        List {
            if let card {
                Section {
                    if !card.subtitle.isEmpty {
                        Text(card.subtitle).font(.footnote).foregroundStyle(.secondary)
                    }
                    ForEach(card.rows) { row in
                        HStack(alignment: .firstTextBaseline) {
                            Text(row.label)
                            Spacer()
                            Text(row.value)
                                .foregroundStyle(color(row.tone))
                                .multilineTextAlignment(.trailing)
                        }
                    }
                }
                if !card.actions.isEmpty {
                    Section {
                        ForEach(card.actions) { action in
                            Button(role: action.destructive ? .destructive : nil) {
                                if action.confirm.isEmpty && !action.destructive {
                                    Task { await run(action) }
                                } else {
                                    confirming = action
                                }
                            } label: {
                                Text(action.title)
                            }
                        }
                    }
                }
            } else {
                Section { ProgressView() }
            }
            if let status {
                Section { Text(status).font(.footnote).foregroundStyle(.secondary) }
            }
        }
        .navigationTitle(card?.title ?? app.name)
        .confirmationDialog(confirming?.confirm.isEmpty == false ? confirming!.confirm : "Run \(confirming?.title ?? "")?",
                            isPresented: Binding(get: { confirming != nil }, set: { if !$0 { confirming = nil } }),
                            titleVisibility: .visible) {
            if let action = confirming {
                Button(action.title, role: action.destructive ? .destructive : nil) {
                    Task { await run(action) }
                }
            }
        }
        .task { await load() }
        .refreshable { await load() }
        .onAppear {
            observer = client.observe(app) { _, _ in Task { await load() } }
        }
        .onDisappear {
            if let observer { client.stopObserving(observer) }
            observer = nil
        }
    }

    private func color(_ tone: MiniAppCard.Tone) -> Color {
        switch tone {
        case .neutral: return .secondary
        case .good: return .green
        case .attention: return .orange
        case .warning: return .red
        }
    }

    private func load() async {
        if card == nil, let cached = client.cachedCard(for: app) {
            card = cached.card
            fetchedMs = cached.fetchedMs
        }
        do {
            card = try await client.card(for: app)
            fetchedMs = Int64(Date().timeIntervalSince1970 * 1000)
            status = nil
        } catch {
            if card != nil {
                let when = Date(timeIntervalSince1970: TimeInterval(fetchedMs) / 1000)
                status = "Last updated \(when.formatted(.relative(presentation: .named))); the desktop is not reachable."
            } else {
                status = ControlErrorText.describe(error)
            }
        }
    }

    private func run(_ action: MiniAppCard.Action) async {
        do {
            switch try await client.perform(action, on: app) {
            case .done:
                status = nil
                await load()
            case .queued:
                status = "\(action.title) will run when the desktop is reachable."
                MiniAppsFeature.scheduleBackgroundRefresh()
            }
        } catch {
            status = ControlErrorText.describe(error)
        }
    }
}
