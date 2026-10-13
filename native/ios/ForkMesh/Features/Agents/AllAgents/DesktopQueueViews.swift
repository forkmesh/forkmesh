import FMBase
import FMChat
import FMControl
import SwiftUI
import UIKit

struct DesktopQueueMenu: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var center: AgentControlCenter
    @State private var showQueue = false
    @State private var error = ""

    static func choices(_ queue: LocalAgentQueue) -> [Int] {
        Array(1...min(queue.limitMax, max(queue.limit + 5, 10)))
    }

    var body: some View {
        if let queue = center.queue {
            Menu {
                Section(queue.summary) {
                    Picker("Run at once", selection: Binding(get: { queue.limit }, set: { next in setLimit(next) })) {
                        ForEach(Self.choices(queue), id: \.self) { count in
                            Text(count == 1 ? "Run 1 agent at once" : "Run \(count) agents at once").tag(count)
                        }
                    }
                }
                Button {
                    showQueue = true
                } label: {
                    Label(queue.waiting == 0 ? "Open queue" : "\(queue.waiting) waiting", systemImage: "list.number")
                }
            } label: {
                HStack(spacing: 2) {
                    Image(systemName: "list.number")
                        .font(.system(size: 13, weight: .semibold))
                    Text("\(queue.running)/\(queue.limit)")
                        .font(.system(size: 11, weight: .semibold).monospacedDigit())
                }
                .foregroundStyle(fm.textSecondary)
                .fixedSize()
                .frame(minWidth: 26, minHeight: 30)
                .contentShape(Rectangle())
            }
            .menuOrder(.fixed)
            .accessibilityLabel(Text("Desktop queue: \(queue.summary)"))
            .accessibilityIdentifier("agents-desktop-queue")
            .sheet(isPresented: $showQueue) {
                DesktopQueueSheet(center: center)
            }
            .alert("Queue not changed", isPresented: Binding(get: { !error.isEmpty }, set: { if !$0 { error = "" } })) {
                Button("Copy") { UIPasteboard.general.string = error }
                Button("Send to agent") { FmMessageHandoff.send(error) }
                Button("OK", role: .cancel) {}
            } message: {
                Text(error)
            }
        }
    }

    private func setLimit(_ limit: Int) {
        guard limit != center.queue?.limit else { return }
        Task {
            do {
                try await center.setQueueLimit(limit)
            } catch {
                self.error = ControlErrorText.describe(error)
            }
        }
    }
}

struct DesktopQueueSheet: View {
    @Environment(\.dismiss) private var dismiss
    @Environment(\.fmPalette) private var fm
    @Bindable var center: AgentControlCenter
    @State private var limit: Int
    @State private var error = ""
    @State private var busy = false
    @State private var editMode: EditMode = .inactive

    init(center: AgentControlCenter) {
        _center = Bindable(center)
        _limit = State(initialValue: center.queue?.limit ?? 1)
    }

    var body: some View {
        NavigationStack {
            List {
                if let queue = center.queue {
                    Section {
                        Stepper(value: $limit, in: 1...max(1, queue.limitMax)) {
                            Text(limit == 1 ? "Run 1 agent at once" : "Run \(limit) agents at once")
                        }
                        .accessibilityIdentifier("desktop-queue-limit")
                    } header: {
                        Text(queue.summary)
                    } footer: {
                        Text("Lowering it puts the newest running agents back in the queue.")
                    }
                    Section {
                        let waiting = center.queuedRows
                        if waiting.isEmpty {
                            Text("No agents are waiting.")
                                .foregroundStyle(.secondary)
                        }
                        ForEach(Array(waiting.enumerated()), id: \.element.id) { index, row in
                            DesktopQueuedLine(row: row, position: index + 1)
                                .swipeActions(edge: .leading) {
                                    if index > 0 {
                                        Button {
                                            move(row, to: 0)
                                        } label: {
                                            Label("Move to top", systemImage: "arrow.up.to.line")
                                        }
                                        .tint(fm.accent)
                                    }
                                }
                                .swipeActions(edge: .trailing) {
                                    if row.canStop {
                                        Button(role: .destructive) {
                                            stop(row)
                                        } label: {
                                            Label("Stop", systemImage: "stop.fill")
                                        }
                                    }
                                }
                                .contextMenu {
                                    menu(row, index: index, count: waiting.count)
                                }
                        }
                        .onMove { source, offset in
                            guard let from = source.first, from < waiting.count else { return }
                            move(waiting[from], to: LocalAgentQueue.target(from: from, offset: offset))
                        }
                    } header: {
                        Text("Waiting · the top one starts next")
                    }
                } else {
                    Section {
                        Text("The desktop's queue is not available.")
                            .foregroundStyle(.secondary)
                    }
                }
                if !error.isEmpty {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle")
                            .font(.footnote)
                            .foregroundStyle(.red)
                            .fmMessageTools(error)
                    }
                }
            }
            .environment(\.editMode, $editMode)
            .navigationTitle("Agent queue")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") { dismiss() }
                }
                ToolbarItem(placement: .primaryAction) {
                    if !center.queuedRows.isEmpty {
                        Button(editMode == .active ? "Finish" : "Reorder") {
                            withAnimation { editMode = editMode == .active ? .inactive : .active }
                        }
                    }
                }
            }
            .fmRefreshable { await center.refreshDesktop() }
            .onChange(of: center.queue?.limit) { _, next in
                if let next, !busy { limit = next }
            }
            .task(id: limit) {
                guard let current = center.queue?.limit, limit != current else { return }
                try? await Task.sleep(for: .milliseconds(600))
                if Task.isCancelled { return }
                await perform { try await center.setQueueLimit(limit) }
            }
        }
    }

    @ViewBuilder private func menu(_ row: UnifiedAgentRow, index: Int, count: Int) -> some View {
        if index > 0 {
            Button {
                move(row, to: 0)
            } label: {
                Label("Move to top", systemImage: "arrow.up.to.line")
            }
            Button {
                move(row, to: index - 1)
            } label: {
                Label("Move up", systemImage: "arrow.up")
            }
        }
        if index < count - 1 {
            Button {
                move(row, to: index + 1)
            } label: {
                Label("Move down", systemImage: "arrow.down")
            }
            Button {
                move(row, to: count - 1)
            } label: {
                Label("Move to bottom", systemImage: "arrow.down.to.line")
            }
        }
        if row.canStop {
            Button(role: .destructive) {
                stop(row)
            } label: {
                Label("Stop", systemImage: "stop.fill")
            }
        }
    }

    private func move(_ row: UnifiedAgentRow, to index: Int) {
        Task { await perform { try await center.moveQueued(row, to: index) } }
    }

    private func stop(_ row: UnifiedAgentRow) {
        Task { await perform { try await center.stop(row) } }
    }

    private func perform(_ work: () async throws -> Void) async {
        busy = true
        defer { busy = false }
        do {
            try await work()
            error = ""
        } catch {
            self.error = ControlErrorText.describe(error)
            if let current = center.queue?.limit { limit = current }
        }
    }
}

private struct DesktopQueuedLine: View {
    let row: UnifiedAgentRow
    let position: Int

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text("\(position)")
                .font(.caption.monospacedDigit().weight(.semibold))
                .foregroundStyle(.secondary)
                .frame(minWidth: 18, alignment: .trailing)
            VStack(alignment: .leading, spacing: 2) {
                Text(row.title)
                    .font(.body.weight(.semibold))
                    .lineLimit(2)
                if !row.repoLabel.isEmpty {
                    Text(row.repoLabel)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
        }
        .accessibilityElement(children: .combine)
    }
}

struct DesktopQueuePositionLine: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var center: AgentControlCenter
    let sessionId: Int
    @State private var busy = false
    @State private var error = ""

    var body: some View {
        if sessionId > 0, let position = center.queue?.position(of: sessionId) {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: FmSpace.x2) {
                    Image(systemName: "hourglass")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(fm.textSecondary)
                    Text("Waiting — #\(position + 1) in queue")
                        .fmText(.labelMedium)
                    Spacer(minLength: 0)
                    if position > 0 {
                        Button {
                            Task { await moveToTop() }
                        } label: {
                            Text("Move to top")
                                .fmText(.labelMedium, color: fm.accent)
                        }
                        .buttonStyle(.plain)
                        .disabled(busy)
                        .accessibilityIdentifier("agent-queue-move-top")
                    }
                }
                if !error.isEmpty {
                    Text(error)
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fmMessageTools(error)
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(fm.bgOverlay, in: RoundedRectangle(cornerRadius: FmRadius.md))
            .accessibilityIdentifier("agent-queue-position")
        }
    }

    private func moveToTop() async {
        busy = true
        defer { busy = false }
        do {
            try await center.moveQueued(id: sessionId, to: 0)
            error = ""
        } catch {
            self.error = ControlErrorText.describe(error)
        }
    }
}
