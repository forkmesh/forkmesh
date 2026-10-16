import FMBase
import FMDesktopRemote
import SwiftUI

struct DesktopLogScreen: View {
    @State var tail: DesktopLogTail
    @State private var queryText = ""

    var body: some View {
        List {
            Section {
                Picker("Show", selection: $tail.severity) {
                    ForEach([LogSeverity.all, .warning, .error]) { level in
                        Text(level.title).tag(level)
                    }
                }
                .pickerStyle(.segmented)
            }
            if tail.more {
                Section {
                    Button {
                        Task { await tail.loadOlder() }
                    } label: {
                        HStack {
                            Text("Load older lines")
                            if tail.loading { ProgressView() }
                        }
                    }
                    .disabled(tail.loading)
                }
            }
            Section {
                ForEach(tail.lines) { line in
                    LogLineRow(line: line)
                }
                if tail.lines.isEmpty && !tail.loading {
                    Text(tail.query.isEmpty ? "The log is empty." : "No line matches.")
                        .foregroundStyle(.secondary)
                }
            }
            if !tail.error.isEmpty {
                Section {
                    Label(tail.error, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                        .font(.footnote)
                        .fmMessageTools(tail.error)
                }
            }
        }
        .listStyle(.plain)
        .searchable(text: $queryText, prompt: "Search the desktop's log")
        .onSubmit(of: .search) {
            tail.query = queryText
            Task { await tail.loadLatest() }
        }
        .onChange(of: queryText) { _, value in
            if value.isEmpty && !tail.query.isEmpty {
                tail.query = ""
                Task { await tail.loadLatest() }
            }
        }
        .onChange(of: tail.severity) { _, _ in
            Task { await tail.loadLatest() }
        }
        .overlay {
            if tail.loading && tail.lines.isEmpty { ProgressView() }
        }
        .navigationTitle("Desktop log")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await tail.loadLatest() }
        .task { await tail.loadLatest() }
    }
}

private struct LogLineRow: View {
    let line: DesktopLogLine

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            if line.timeMs > 0 {
                Text(Date(timeIntervalSince1970: Double(line.timeMs) / 1000), style: .time)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
            Text(line.body)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(color)
                .textSelection(.enabled)
                .lineLimit(12)
        }
        .listRowSeparator(.hidden)
    }

    private var color: Color {
        switch line.severity {
        case .error: return .red
        case .warning: return .orange
        default: return .primary
        }
    }
}

struct StallsScreen: View {
    @Bindable var store: DiagnosticsStore
    @State private var pending: PendingConfirmation?
    @State private var error = ""
    @State private var banner = ""

    var body: some View {
        List {
            if let stalls = store.stalls {
                if stalls.stalls.isEmpty {
                    Section {
                        Text("No UI stalls recorded since the desktop started.")
                            .foregroundStyle(.secondary)
                    }
                } else {
                    Section {
                        Button {
                            Task { await prepare(.sendStalls, title: "Send all to an agent") }
                        } label: {
                            Label("Send all to an agent", systemImage: "wand.and.stars")
                        }
                        .disabled(!stalls.canSend)
                    } footer: {
                        if !stalls.canSend {
                            Text("The desktop needs a local ForkMesh checkout to run a fixing agent in.")
                        }
                    }
                    Section("Recorded") {
                        ForEach(stalls.stalls) { stall in
                            NavigationLink {
                                StallDetailScreen(store: store, stall: stall, canSend: stalls.canSend)
                                    .shellPageClearance()
                            } label: {
                                VStack(alignment: .leading, spacing: 3) {
                                    Text(stall.summary).font(.subheadline).lineLimit(3)
                                    Text(stall.when).font(.caption).foregroundStyle(.secondary)
                                }
                            }
                        }
                    }
                }
            }
            if !banner.isEmpty {
                Section {
                    Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green)
                        .fmMessageTools(banner)
                }
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
        .overlay {
            if store.stalls == nil && store.error.isEmpty { ProgressView() }
        }
        .navigationTitle("UI stalls")
        .fmRefreshable { await store.refreshStalls() }
        .task { await store.refreshStalls() }
        .sheet(item: $pending) { item in
            NavigationStack {
                RemoteConfirmSheet(title: item.title, confirmation: item.confirmation) { confirmation, _ in
                    try await store.run(confirmation)
                } onDone: { message in
                    banner = message
                }
            }
        }
    }

    private func prepare(_ action: DiagnosticsAction, title: String) async {
        do {
            pending = PendingConfirmation(
                title: title, confirmation: try await store.prepare(action), destructive: false)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct StallDetailScreen: View {
    let store: DiagnosticsStore
    let stall: StallRecord
    let canSend: Bool

    @State private var detail: StallRecord?
    @State private var error = ""
    @State private var pending: PendingConfirmation?
    @State private var banner = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(stall.summary).font(.headline)
                Text(stall.when).font(.caption).foregroundStyle(.secondary)
                if !banner.isEmpty {
                    Label(banner, systemImage: "checkmark.circle").foregroundStyle(.green)
                        .fmMessageTools(banner)
                }
                if !error.isEmpty {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red).font(.footnote)
                        .fmMessageTools(error)
                }
                if let detail {
                    Text(detail.text)
                        .font(.system(.caption2, design: .monospaced))
                        .textSelection(.enabled)
                    if detail.truncated {
                        Text("The rest of this record stays on the desktop.")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                } else if error.isEmpty {
                    ProgressView()
                }
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle("Stall")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Button {
                    Task { await prepare() }
                } label: {
                    Label("Send to an agent", systemImage: "wand.and.stars")
                }
                .disabled(!canSend)
            }
        }
        .task { await load() }
        .sheet(item: $pending) { item in
            NavigationStack {
                RemoteConfirmSheet(title: item.title, confirmation: item.confirmation) { confirmation, _ in
                    try await store.run(confirmation)
                } onDone: { message in
                    banner = message
                }
            }
        }
    }

    private func load() async {
        do {
            detail = try await store.stall(stall.index)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }

    private func prepare() async {
        do {
            pending = PendingConfirmation(
                title: "Send to an agent", confirmation: try await store.prepare(.sendStall(index: stall.index)),
                destructive: false)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct ProcessesScreen: View {
    @Bindable var store: DiagnosticsStore
    @State private var pending: PendingConfirmation?
    @State private var selected: DesktopProcess?
    @State private var error = ""

    var body: some View {
        List {
            if let list = store.processes {
                let agents = list.agentProcesses
                if !agents.isEmpty {
                    Section("Agent runs") {
                        ForEach(agents) { process in
                            processRow(process, list: list)
                        }
                    }
                }
                Section("Largest memory users") {
                    ForEach(list.processes) { process in
                        processRow(process, list: list)
                    }
                }
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
        .overlay {
            if store.processes == nil && store.error.isEmpty { ProgressView() }
        }
        .navigationTitle("Memory")
        .fmRefreshable { await store.refreshProcesses() }
        .task { await store.refreshProcesses() }
        .sheet(item: $selected) { process in
            NavigationStack { ProcessDetailScreen(store: store, process: process) }
        }
        .sheet(item: $pending) { item in
            NavigationStack {
                RemoteConfirmSheet(
                    title: item.title, confirmation: item.confirmation, destructive: true
                ) { confirmation, _ in
                    try await store.run(confirmation)
                }
            }
        }
    }

    private func processRow(_ process: DesktopProcess, list: ProcessList) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(process.name).font(.subheadline.weight(.semibold))
                Spacer()
                Text(RemoteFormat.bytes(process.rssBytes)).font(.subheadline.monospacedDigit())
            }
            ShareBar(share: list.share(of: process), tint: list.share(of: process) > 0.2 ? .orange : .accentColor)
            if !process.description.isEmpty {
                Text(process.description)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if let agent = process.agent {
                HStack {
                    Label(agent.label, systemImage: "cpu").font(.caption).lineLimit(1)
                    Spacer()
                    if agent.stoppable {
                        Button("Stop", role: .destructive) {
                            Task { await prepareStop(agent.session) }
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                    }
                }
            }
            Text(process.command)
                .font(.caption2.monospaced())
                .foregroundStyle(.secondary)
                .lineLimit(2)
        }
        .padding(.vertical, 2)
        .contentShape(Rectangle())
        .onTapGesture { selected = process }
    }

    private func prepareStop(_ session: Int) async {
        do {
            pending = PendingConfirmation(
                title: "Stop agent", confirmation: try await store.prepare(.stopAgent(session: session)),
                destructive: true)
        } catch {
            self.error = RemoteErrorText.describe(error)
        }
    }
}

struct ProcessDetailScreen: View {
    @Bindable var store: DiagnosticsStore
    let process: DesktopProcess

    @State private var details = ""
    @State private var identity = ""
    @State private var token = ""
    @State private var stoppable = false
    @State private var password = ""
    @State private var passwordRequired = false
    @State private var message = ""
    @State private var error = ""

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text(process.description).font(.subheadline).foregroundStyle(.secondary)
                if !message.isEmpty { Label(message, systemImage: "checkmark.circle").foregroundStyle(.green) }
                if !error.isEmpty { Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red) }
                if details.isEmpty && error.isEmpty { ProgressView() }
                Text(details).font(.system(.caption2, design: .monospaced)).textSelection(.enabled)
            }
            .padding()
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle(process.name)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            if stoppable {
                ToolbarItem(placement: .primaryAction) {
                    Button("Stop", role: .destructive) { Task { await stop() } }
                }
            }
        }
        .task { await load() }
        .alert("Administrator password", isPresented: $passwordRequired) {
            SecureField("Password", text: $password)
            Button("Cancel", role: .cancel) { password = "" }
            Button("Stop", role: .destructive) { Task { await stop() } }
        } message: {
            Text("Enter this computer's password to stop \(process.name).")
        }
    }

    private func load() async {
        do {
            let reply = try await store.family.call("process", ["pid": .int(Int64(process.pid))], timeout: 20)
            details = reply.str("details")
            identity = reply.str("identity")
            token = reply.str("token")
            stoppable = reply.bool("stoppable")
        } catch { self.error = RemoteErrorText.describe(error) }
    }

    private func stop() async {
        do {
            var params: JSONObject = ["pid": .int(Int64(process.pid)), "identity": .string(identity), "token": .string(token)]
            if !password.isEmpty { params["password"] = .string(password) }
            password = ""
            let reply = try await store.family.call("processStop", params, timeout: 20)
            token = reply.str("token")
            if reply.bool("passwordRequired") { passwordRequired = true; return }
            message = reply.str("message", "Termination requested.")
            stoppable = false
            await store.refreshProcesses()
        } catch { self.error = RemoteErrorText.describe(error) }
    }
}
