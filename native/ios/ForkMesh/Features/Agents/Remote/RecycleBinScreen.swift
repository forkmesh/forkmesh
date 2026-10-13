import FMAgents
import SwiftUI

struct RecycleBinScreen: View {
    @State private var model: RecycleBinModel

    private static let retentionChoices = [1, 6, 12, 24, 48, 72, 168, 336, 720]

    init(client: AgentOpsClient) {
        _model = State(initialValue: RecycleBinModel(client: client))
    }

    private var confirming: Binding<Bool> {
        Binding(get: { model.confirmation != nil }, set: { if !$0 { model.cancelPurge() } })
    }

    private var targets: [Int] {
        model.selectedIds.isEmpty ? model.bin.ids : model.selectedIds
    }

    var body: some View {
        List {
            if !model.errorText.isEmpty {
                Label(model.errorText, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(model.errorText)
            }
            if !model.unsupported && model.loaded {
                Section {
                    Text(model.bin.hint)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                    retentionMenu
                    if !model.bin.isEmpty { actions }
                    if !model.note.isEmpty {
                        Text(model.note)
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            if !model.bin.isEmpty {
                Section {
                    ForEach(model.bin.agents) { agent in
                        row(agent)
                    }
                } footer: {
                    if model.bin.truncated {
                        Text("Showing the newest \(model.bin.agents.count) of \(model.bin.count). Open the recycle bin on the desktop for the rest.")
                    }
                }
            }
        }
        .navigationTitle("Recycle bin")
        .toolbar {
            if !model.bin.isEmpty {
                ToolbarItem(placement: .topBarTrailing) {
                    Button(model.selectedIds.isEmpty ? "Select all" : "Clear") {
                        if model.selectedIds.isEmpty { model.selectAll() } else { model.clearSelection() }
                    }
                    .accessibilityIdentifier("recycle-bin-select")
                }
            }
        }
        .overlay {
            if model.loading && !model.loaded { ProgressView() }
        }
        .disabled(model.busy)
        .fmRefreshable { await model.refresh() }
        .task { await model.refresh() }
        .confirmationDialog(model.confirmation?.summary ?? "", isPresented: confirming, titleVisibility: .visible) {
            Button("Delete permanently", role: .destructive) {
                Task {
                    if await model.purge() { FmHaptics.commit() }
                }
            }
            Button("Cancel", role: .cancel) { model.cancelPurge() }
        }
    }

    private var retentionMenu: some View {
        Menu {
            ForEach(Self.retentionChoices.filter { $0 >= model.bin.minHours && $0 <= model.bin.maxHours }, id: \.self) { hours in
                Button {
                    Task { await model.setRetention(hours: hours) }
                } label: {
                    if hours == model.bin.retentionHours {
                        Label(Self.retentionLabel(hours), systemImage: "checkmark")
                    } else {
                        Text(Self.retentionLabel(hours))
                    }
                }
            }
        } label: {
            LabeledContent("Keep deleted agents for", value: Self.retentionLabel(model.bin.retentionHours))
        }
        .accessibilityIdentifier("recycle-bin-retention")
    }

    private var actions: some View {
        let selected = model.selectedIds.count
        return AgentChipRow {
            FmToneButton(
                label: selected == 0 ? "Restore all" : "Restore \(selected)", icon: "restore_from_trash_outlined",
                tone: .success
            ) {
                restore(targets)
            }
            .accessibilityIdentifier("recycle-bin-restore")
            FmToneButton(
                label: selected == 0 ? "Empty bin" : "Delete \(selected)", icon: "delete_forever_outlined", tone: .danger
            ) {
                Task { await model.preparePurge(targets) }
            }
            .accessibilityIdentifier("recycle-bin-purge")
        }
        .listRowSeparator(.hidden)
    }

    private func row(_ agent: RecycledAgent) -> some View {
        Button {
            model.toggle(agent.id)
            FmHaptics.selection()
        } label: {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: model.selection.contains(agent.id) ? "checkmark.circle.fill" : "circle")
                    .foregroundStyle(model.selection.contains(agent.id) ? Color.accentColor : .secondary)
                    .frame(width: 22)
                VStack(alignment: .leading, spacing: 3) {
                    Text(agent.headline)
                        .font(.subheadline)
                        .foregroundStyle(.primary)
                        .lineLimit(2)
                    Text(["#\(agent.id)", agent.repo, agent.branch].filter { !$0.isEmpty }.joined(separator: " · "))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                    Text(Self.deletedLine(agent))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                Spacer(minLength: 0)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .swipeActions(edge: .leading) {
            Button {
                restore([agent.id])
            } label: {
                Label("Restore", systemImage: "arrow.uturn.backward")
            }
            .tint(.green)
        }
        .swipeActions(edge: .trailing) {
            Button(role: .destructive) {
                Task { await model.preparePurge([agent.id]) }
            } label: {
                Label("Delete", systemImage: "trash")
            }
        }
        .contextMenu {
            Button {
                restore([agent.id])
            } label: {
                Label("Restore", systemImage: "arrow.uturn.backward")
            }
            Button(role: .destructive) {
                Task { await model.preparePurge([agent.id]) }
            } label: {
                Label("Delete permanently", systemImage: "trash")
            }
        }
        .accessibilityIdentifier("recycle-bin-row-\(agent.id)")
    }

    private func restore(_ ids: [Int]) {
        Task {
            if await model.restore(ids) {
                FmHaptics.commit()
                await HostsFeature.agents?.refresh()
            }
        }
    }

    static func retentionLabel(_ hours: Int) -> String {
        if hours % 24 == 0 && hours >= 48 { return "\(hours / 24) days" }
        return hours == 1 ? "1 hour" : "\(hours) hours"
    }

    static func deletedLine(_ agent: RecycledAgent) -> String {
        guard agent.deletedAtMs > 0 else { return agent.removal }
        let deleted = Date(timeIntervalSince1970: Double(agent.deletedAtMs) / 1000)
        return "Deleted \(deleted.formatted(.relative(presentation: .named))) · \(agent.removal)"
    }
}
