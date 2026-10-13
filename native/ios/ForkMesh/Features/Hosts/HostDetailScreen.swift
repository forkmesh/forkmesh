// WS7: one host: its facts, its actions (each confirmed afresh on the desktop), the newest log
// lines, and the way into its live log.

import FMAgents
import FMBase
import FMControl
import FMDesktopRemote
import FMHosts
import SwiftUI

struct HostDetailScreen: View {
    @Bindable var store: HostsStore
    @Bindable var hub: DesktopControlHub
    let hostName: String

    @State private var detail: HostRow?
    @State private var error = ""
    @State private var confirming: HostConfirmation?
    @State private var preparing: HostAction?
    @State private var sessions: [DesktopTerminalInfo] = []
    @State private var openTerminal: HostTerminalTarget?
    @State private var pendingTerminal: HostTerminalTarget?

    private var host: HostRow? { detail ?? store.host(named: hostName) }

    var body: some View {
        List {
            if let host {
                Section {
                    HStack(spacing: 10) {
                        HealthDot(health: host.health)
                        Text(host.healthLine).font(.headline)
                    }
                    facts(host)
                }
                if host.actions.contains(.terminal) || !sessions.isEmpty {
                    terminalSection(host)
                }
                Section("Actions") {
                    ForEach(host.actions.filter { !$0.opensTerminal }) { action in
                        Button(role: action.isDestructive ? .destructive : nil) {
                            Task { await prepare(action) }
                        } label: {
                            HStack {
                                Label(action.title, systemImage: icon(action))
                                Spacer()
                                if preparing == action || store.isRunning(action, host: host.name) {
                                    ProgressView()
                                }
                            }
                        }
                        .disabled(preparing != nil || store.isRunning(action, host: host.name))
                    }
                }
                if let fleet = DesktopRemoteFeature.fleet {
                    Section {
                        NavigationLink {
                            HostOperationsScreen(store: fleet, host: host)
                                .shellPageClearance()
                        } label: {
                            Label("Disk usage, agent CLIs, Actions, remove", systemImage: "wrench.and.screwdriver")
                        }
                    }
                }
                if host.actions.contains(.restart) {
                    Section {
                        ForEach(agentLoginProviders.map { $0.id }, id: \.self) { (provider: String) in
                            NavigationLink {
                                AgentLoginTerminalScreen(node: host.name, provider: provider)
                                    .shellPageClearance()
                            } label: {
                                Label("Sign in to \(agentProviderLabel(provider))", systemImage: "person.badge.key")
                            }
                        }
                    } header: {
                        Text("Agent sign-in")
                    } footer: {
                        Text("Runs the provider's own sign-in on the node itself, through the relay.")
                    }
                }
                Section {
                    NavigationLink {
                        HostLogViewerScreen(stream: HostLogStream(channel: hub, host: host.name))
                            .shellPageClearance()
                    } label: {
                        Label("Live log", systemImage: "text.alignleft")
                    }
                    if !host.recentLog.isEmpty {
                        ForEach(Array(host.recentLog.suffix(12).enumerated()), id: \.offset) { _, line in
                            Text(line)
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .lineLimit(3)
                        }
                    }
                } header: {
                    Text("Log")
                }
                if !host.lastResult.isEmpty {
                    Section("Desktop's last result") {
                        Text(host.lastResult)
                            .font(.caption)
                            .textSelection(.enabled)
                            .fmMessageTools(host.lastResult)
                    }
                }
                let outcomes = store.outcomes.filter { $0.host.lowercased() == host.id }
                if !outcomes.isEmpty {
                    Section("From this phone") {
                        ForEach(outcomes) { OutcomeRow(outcome: $0) }
                    }
                }
            } else if error.isEmpty {
                ProgressView()
            }
            if !error.isEmpty {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .fmMessageTools(error)
                }
            }
        }
        .navigationTitle(hostName)
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await load() }
        .task { await load() }
        .onAppear { Task { await loadSessions() } }
        .onChange(of: store.snapshot) { _, _ in
            Task { await load() }
        }
        .sheet(item: $confirming, onDismiss: {
            if let target = pendingTerminal {
                pendingTerminal = nil
                openTerminal = target
            }
        }) { confirmation in
            NavigationStack {
                HostActionSheet(store: store, confirmation: confirmation, onTerminal: { terminal in
                    pendingTerminal = HostTerminalTarget(id: terminal)
                })
            }
            .presentationDetents([.medium, .large])
        }
        .navigationDestination(item: $openTerminal) { target in
            HostTerminalScreen(hub: hub, host: hostName, terminal: target.id)
                .shellPageClearance()
        }
    }

    private func terminalSection(_ host: HostRow) -> some View {
        Section {
            if host.actions.contains(.terminal) {
                Button {
                    Task { await prepare(.terminal) }
                } label: {
                    HStack {
                        Label("Open SSH terminal", systemImage: "apple.terminal")
                        Spacer()
                        if preparing == .terminal { ProgressView() }
                    }
                }
                .disabled(preparing != nil)
            }
            ForEach(sessions) { session in
                NavigationLink {
                    HostTerminalScreen(hub: hub, host: host.name, terminal: session.terminal)
                        .shellPageClearance()
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(session.running ? "Open session" : "Ended session")
                        Text(sessionLine(session))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
        } header: {
            Text("Terminal")
        } footer: {
            Text(host.user.isEmpty
                ? "A shell on the host, opened by the desktop with its saved key. It stays open when you leave it; close it from its screen."
                : "A shell on the host as \(host.user), opened by the desktop with its saved key. It stays open when you leave it; close it from its screen.")
        }
    }

    private func sessionLine(_ session: DesktopTerminalInfo) -> String {
        var parts: [String] = []
        if session.startedAtMs > 0 {
            let started = Date(timeIntervalSince1970: Double(session.startedAtMs) / 1000)
            parts.append("since \(started.formatted(date: .omitted, time: .shortened))")
        }
        if !session.device.isEmpty { parts.append("from \(session.device)") }
        return parts.joined(separator: " ")
    }

    private func loadSessions() async {
        sessions = (try? await store.openTerminals(host: hostName)) ?? []
    }

    @ViewBuilder
    private func facts(_ host: HostRow) -> some View {
        if !host.address.isEmpty { LabeledContent("Address", value: host.address) }
        if !host.dnsHostname.isEmpty { LabeledContent("Hostname", value: host.dnsHostname) }
        if !host.user.isEmpty { LabeledContent("SSH user", value: host.user) }
        if !host.provider.isEmpty { LabeledContent("Provider", value: host.provider) }
        if !host.planLine.isEmpty { LabeledContent("Plan", value: host.planLine) }
        if !host.version.isEmpty { LabeledContent("ForkMesh", value: host.version) }
        if !host.status.isEmpty { LabeledContent("Install", value: host.status) }
    }

    private func icon(_ action: HostAction) -> String {
        switch action {
        case .update: return "arrow.down.circle"
        case .installBinary: return "square.and.arrow.up"
        case .terminal: return "apple.terminal"
        case .restart: return "arrow.clockwise"
        case .stop: return "stop.circle"
        case .start: return "play.circle"
        case .install: return "plus.circle"
        case .destroy: return "trash"
        }
    }

    private func load() async {
        do {
            detail = try await store.detail(hostName)
            error = ""
            await loadSessions()
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }

    private func prepare(_ action: HostAction) async {
        preparing = action
        defer { preparing = nil }
        do {
            confirming = try await store.prepare(action, host: hostName)
            error = ""
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }
}
