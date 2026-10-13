import FMApi
import FMBase
import FMCode
import FMControl
import Observation
import SwiftUI
import UIKit

@MainActor
@Observable
final class MergeQueueFlow {
    var pending: MergeQueueRequest?
    var confirmation: MergeQueueConfirmation?
    var message: String?
    var busy = false
    var lastRepo: MergeQueueRepo?

    func prepare(_ request: MergeQueueRequest) async {
        guard let remote = ReviewEnv.mergeQueue else {
            message = "Pair this phone with your desktop to use its merge queue."
            return
        }
        busy = true
        defer { busy = false }
        do {
            confirmation = try await remote.prepare(request)
            pending = request
        } catch {
            message = MergeQueueRemote.message(for: error)
        }
    }

    func confirm() async {
        guard let remote = ReviewEnv.mergeQueue, let request = pending, let token = confirmation?.token else { return }
        confirmation = nil
        pending = nil
        busy = true
        defer { busy = false }
        do {
            let result = try await remote.run(request, token: token)
            lastRepo = result.repo
            message = result.message
            FmHaptics.commit()
        } catch {
            message = MergeQueueRemote.message(for: error)
            FmHaptics.failure()
        }
    }

    func cancel() {
        confirmation = nil
        pending = nil
    }
}

private struct MergeQueueConfirmationModifier: ViewModifier {
    @Bindable var flow: MergeQueueFlow

    func body(content: Content) -> some View {
        content
            .confirmationDialog(
                flow.pending?.action.title ?? "Merge queue",
                isPresented: Binding(get: { flow.confirmation != nil }, set: { if !$0 { flow.cancel() } }),
                titleVisibility: .visible
            ) {
                Button(flow.pending?.action.title ?? "Confirm", role: flow.pending?.action.isDestructive == true ? .destructive : nil) {
                    Task { await flow.confirm() }
                }
                Button("Cancel", role: .cancel) { flow.cancel() }
            } message: {
                Text(flow.confirmation?.summary ?? "")
            }
            .alert("Merge queue", isPresented: Binding(get: { flow.message != nil }, set: { if !$0 { flow.message = nil } })) {
                Button("Copy") { UIPasteboard.general.string = flow.message ?? "" }
                Button("Send to agent") { FmMessageHandoff.send(flow.message ?? "") }
                Button("OK", role: .cancel) {}
            } message: {
                Text(flow.message ?? "")
            }
    }
}

extension View {
    func mergeQueueConfirmation(_ flow: MergeQueueFlow) -> some View {
        modifier(MergeQueueConfirmationModifier(flow: flow))
    }
}

struct MergeQueueScreen: View {
    let repo: RepoRef?
    @State private var repos: [MergeQueueRepo] = []
    @State private var error: String?
    @State private var loaded = false
    @State private var flow = MergeQueueFlow()
    @State private var tap: UUID?

    var body: some View {
        List {
            if let error {
                Section {
                    Label(error, systemImage: "exclamationmark.triangle").foregroundStyle(.red)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(error)
                }
            }
            if loaded && repos.isEmpty && error == nil {
                Section {
                    Text(repo == nil ? "No repository on the desktop has a merge queue turned on." : "The desktop does not have this repository.")
                        .foregroundStyle(.secondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(repo == nil ? "No repository on the desktop has a merge queue turned on." : "The desktop does not have this repository.")
                }
            }
            ForEach(repos) { (row: MergeQueueRepo) in
                Section {
                    HStack {
                        VStack(alignment: .leading) {
                            Text(row.id).font(.headline)
                            Text(row.summary).font(.footnote).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if !row.writable {
                            Label("Read-only", systemImage: "lock").font(.caption).foregroundStyle(.secondary)
                        } else if row.isOpen {
                            Label("Open on desktop", systemImage: "desktopcomputer").font(.caption).foregroundStyle(.secondary)
                        }
                    }
                    ForEach(row.entries) { (entry: MergeQueueEntry) in
                        entryRow(entry, in: row)
                    }
                    let actions = MergeQueueRules.actions(for: row)
                    if !actions.isEmpty {
                        HStack {
                            ForEach(actions, id: \.self) { (action: MergeQueueAction) in
                                Button(action.title, role: action.isDestructive ? .destructive : nil) {
                                    Task { await flow.prepare(MergeQueueRequest(owner: row.owner, name: row.name, action: action)) }
                                }
                                .buttonStyle(.bordered)
                                .font(.caption)
                            }
                        }
                        .disabled(flow.busy)
                    }
                }
            }
        }
        .navigationTitle("Merge queue")
        .navigationBarTitleDisplayMode(.inline)
        .mergeQueueConfirmation(flow)
        .fmRefreshable { await load() }
        .task { await load() }
        .onChange(of: flow.lastRepo) { _, _ in Task { await load() } }
        .onAppear {
            if tap == nil, let remote = ReviewEnv.mergeQueue {
                tap = remote.onChange { _, _ in Task { await load() } }
            }
        }
        .onDisappear {
            if let tap, let remote = ReviewEnv.mergeQueue { remote.removeTap(tap) }
            tap = nil
        }
    }

    private func entryRow(_ entry: MergeQueueEntry, in row: MergeQueueRepo) -> some View {
        HStack(alignment: .top) {
            Text("\(row.place(of: entry.number) ?? 0).").font(.caption.monospaced()).foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 2) {
                NavigationLink(value: Route.pull(RepoRef(owner: row.owner, name: row.name), number: entry.number, tab: .conversation)) {
                    Text(entry.title.isEmpty ? "Pull request #\(entry.number)" : "#\(entry.number) \(entry.title)")
                        .lineLimit(2)
                }
                HStack(spacing: 4) {
                    Text(entry.state.title)
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(entry.state.needsAttention ? Color.red : entry.state.isRunning ? Color.orange : Color.secondary)
                    if !entry.detail.isEmpty {
                        Text(entry.detail).font(.caption2).foregroundStyle(.secondary).lineLimit(2)
                    }
                }
                .fmMessageTools(entry.state.needsAttention ? "\(row.owner)/\(row.name) #\(entry.number): \(entry.state.title)" + (entry.detail.isEmpty ? "" : " · \(entry.detail)") : nil)
            }
        }
        .swipeActions {
            ForEach(Array(MergeQueueRules.entryActions(entry, in: row).enumerated()), id: \.offset) { pair in
                let action: MergeQueueAction = pair.element.0
                let delta: Int = pair.element.1
                Button(action == .move ? (delta < 0 ? "Up" : "Down") : "Remove", role: action == .remove ? .destructive : nil) {
                    Task {
                        await flow.prepare(MergeQueueRequest(owner: row.owner, name: row.name, action: action, number: entry.number, delta: delta))
                    }
                }
            }
        }
    }

    private func load() async {
        guard let remote = ReviewEnv.mergeQueue else {
            error = "Pair this phone with your desktop to see its merge queue."
            loaded = true
            return
        }
        do {
            repos = try await remote.list(owner: repo?.owner ?? "", name: repo?.name ?? "")
            error = nil
        } catch {
            self.error = MergeQueueRemote.message(for: error)
        }
        loaded = true
    }
}
