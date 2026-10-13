import FMAgents
import SwiftUI

struct ApprovalDestination: View {
    let approvalId: String

    var body: some View {
        if let queue = AgentOpsFeature.approvals {
            ApprovalScreen(queue: queue, approvalId: String(approvalId.split(separator: "/").last ?? Substring(approvalId)))
        } else {
            AgentOpsUnavailableView()
        }
    }
}

struct ApprovalScreen: View {
    @Bindable var queue: ApprovalsQueue
    let approvalId: String
    @State private var lifetime: ApprovalLifetime = .once
    @State private var working = false
    @State private var outcome: String?

    var body: some View {
        Form {
            if let approval = queue.approval(id: approvalId) {
                requestSections(approval)
            } else if let outcome {
                Section {
                    Label(outcome, systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                }
            } else if queue.loading {
                Section {
                    ProgressView()
                        .frame(maxWidth: .infinity)
                }
            } else {
                Section {
                    ContentUnavailableView(
                        "Nothing to answer", systemImage: "checkmark.circle",
                        description: Text("This request was answered, or the agent stopped waiting."))
                }
            }
            if !queue.errorText.isEmpty {
                Section {
                    Label(queue.errorText, systemImage: "exclamationmark.triangle")
                        .font(.footnote)
                        .foregroundStyle(.red)
                        .fmMessageTools(queue.errorText)
                }
            }
        }
        .navigationTitle("Approval")
        .navigationBarTitleDisplayMode(.inline)
        .fmRefreshable { await queue.refresh() }
        .task {
            queue.start()
            if queue.approval(id: approvalId) == nil { await queue.refresh() }
        }
    }

    @ViewBuilder
    private func requestSections(_ approval: PendingApproval) -> some View {
        Section {
            Label(approval.kind.title, systemImage: approval.kind.systemImage)
                .font(.headline)
            if !approval.subtitle.isEmpty {
                Text(approval.subtitle)
                    .font(.footnote)
                    .foregroundStyle(.secondary)
            }
        }
        Section("Request") {
            if approval.command.isEmpty {
                Text(approval.question)
                    .textSelection(.enabled)
            } else {
                Text(approval.command)
                    .font(.system(.callout, design: .monospaced))
                    .textSelection(.enabled)
            }
        }
        Section {
            Picker("Allow", selection: $lifetime) {
                ForEach(approval.lifetimes, id: \.self) { choice in
                    Text(choice.title).tag(choice)
                }
            }
            .pickerStyle(.inline)
            .labelsHidden()
        } header: {
            Text("How long")
        } footer: {
            Text(lifetime.detail)
        }
        Section {
            Button {
                Task { await answer(allow: true) }
            } label: {
                HStack {
                    Text(lifetime.title)
                    Spacer()
                    if working { ProgressView() }
                }
            }
            .disabled(working)
            Button("Deny", role: .destructive) {
                Task { await answer(allow: false) }
            }
            .disabled(working)
        } footer: {
            Text("The desktop checks a fresh one-use confirmation and writes the answer to its log.")
        }
    }

    private func answer(allow: Bool) async {
        if allow {
            guard await AgentOpsFeature.authenticate(reason: "Approve the agent's request") else { return }
        }
        working = true
        defer { working = false }
        let ok: Bool
        if allow {
            ok = await queue.approve(approvalId, lifetime: lifetime)
        } else {
            ok = await queue.deny(approvalId)
        }
        if ok {
            outcome = allow ? "Approved" : "Denied"
            FmHaptics.commit()
        } else {
            FmHaptics.failure()
        }
    }
}

struct ApprovalRow: View {
    let approval: PendingApproval

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: approval.kind.systemImage)
                .foregroundStyle(.orange)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 3) {
                Text(approval.headline)
                    .font(.subheadline.weight(.semibold))
                    .lineLimit(2)
                if !approval.subtitle.isEmpty {
                    Text(approval.subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
        .accessibilityHint("Opens the request to approve or deny it")
    }
}

struct AgentApprovalsSection: View {
    @Bindable var queue: ApprovalsQueue

    var body: some View {
        Group {
            if !queue.isEmpty {
                FmCard(radius: FmRadius.lg) {
                    VStack(alignment: .leading, spacing: FmSpace.x3) {
                        FmSectionHeader(title: "Needs you", count: queue.count)
                        ForEach(queue.approvals) { approval in
                            NavigationLink {
                                ApprovalScreen(queue: queue, approvalId: approval.id)
                                    .shellPageClearance()
                            } label: {
                                ApprovalRow(approval: approval)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }
        }
        .task {
            queue.start()
            await queue.refresh()
        }
    }
}
