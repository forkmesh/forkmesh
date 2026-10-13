import FMAgents
import SwiftUI

struct CapabilityGrantsScreen: View {
    @State private var model: CapabilityGrantsModel
    @State private var pending: (grant: CapabilityGrant, confirmation: AgentOpsConfirmation)?
    @State private var confirming = false

    init(client: AgentOpsClient) {
        _model = State(initialValue: CapabilityGrantsModel(client: client))
    }

    var body: some View {
        List {
            if !model.errorText.isEmpty {
                Label(model.errorText, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(model.errorText)
            }
            if model.grants.isEmpty && !model.loading {
                Text("No saved permissions. \"Always allow\" answers show up here.")
                    .foregroundStyle(.secondary)
            }
            ForEach(model.grants) { grant in
                HStack(alignment: .top, spacing: 12) {
                    Image(systemName: grant.kind.systemImage)
                        .foregroundStyle(grant.allows ? .green : .red)
                        .frame(width: 22)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(grant.resource.isEmpty ? grant.capability : grant.resource)
                            .font(.system(.subheadline, design: .monospaced))
                            .lineLimit(2)
                        Text([grant.allows ? "Allowed" : "Denied", grant.capability, grant.worktree]
                            .filter { !$0.isEmpty }.joined(separator: " · "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Spacer(minLength: 0)
                    if model.revoking.contains(grant.id) { ProgressView() }
                }
                .swipeActions(edge: .trailing) {
                    Button(role: .destructive) {
                        Task { await begin(grant) }
                    } label: {
                        Label("Revoke", systemImage: "xmark.shield")
                    }
                }
            }
        }
        .navigationTitle("Saved permissions")
        .fmRefreshable { await model.refresh() }
        .task { await model.refresh() }
        .confirmationDialog(pending?.confirmation.summary ?? "", isPresented: $confirming, titleVisibility: .visible) {
            Button("Revoke", role: .destructive) {
                guard let pending else { return }
                Task { await model.revoke(pending.grant, confirmation: pending.confirmation) }
            }
            Button("Cancel", role: .cancel) { pending = nil }
        }
    }

    private func begin(_ grant: CapabilityGrant) async {
        guard let confirmation = await model.prepareRevoke(grant) else { return }
        pending = (grant, confirmation)
        confirming = true
    }
}
