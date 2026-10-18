import FMApi
import FMBase
import FMCode
import FMControl
import Observation
import SwiftUI
import UIKit

@MainActor
@Observable
final class PullMergeAllFlow {
    let owner: String
    let name: String
    var confirmation: PullMergeAllConfirmation?
    var message: String?
    var busy = false

    init(owner: String, name: String) {
        self.owner = owner
        self.name = name
    }

    static var remote: PullMergeAllRemote? {
        HostsFeature.hub.map { PullMergeAllRemote(channel: $0) }
    }

    static var desktopPaired: Bool {
        HostsFeature.hub?.desktop != nil
    }

    func prepare() async {
        guard let remote = Self.remote, Self.desktopPaired else {
            message = PullMergeAllRules.unpaired
            return
        }
        busy = true
        defer { busy = false }
        do {
            confirmation = try await remote.prepare(owner: owner, name: name)
        } catch {
            message = PullMergeAllRemote.message(for: error)
        }
    }

    func confirm() async {
        guard let remote = Self.remote, let token = confirmation?.token else { return }
        confirmation = nil
        busy = true
        defer { busy = false }
        do {
            let result = try await remote.run(owner: owner, name: name, token: token)
            message = result.message
            FmHaptics.commit()
        } catch {
            message = PullMergeAllRemote.message(for: error)
            FmHaptics.failure()
        }
    }

    func cancel() {
        confirmation = nil
    }
}

private struct PullMergeAllConfirmationModifier: ViewModifier {
    @Bindable var flow: PullMergeAllFlow

    func body(content: Content) -> some View {
        content
            .confirmationDialog(
                PullMergeAllRules.title,
                isPresented: Binding(get: { flow.confirmation != nil }, set: { if !$0 { flow.cancel() } }),
                titleVisibility: .visible
            ) {
                Button(PullMergeAllRules.title, role: .destructive) {
                    Task { await flow.confirm() }
                }
                Button("Cancel", role: .cancel) { flow.cancel() }
            } message: {
                Text(flow.confirmation?.summary ?? "")
            }
            .alert(PullMergeAllRules.title, isPresented: Binding(get: { flow.message != nil }, set: { if !$0 { flow.message = nil } })) {
                Button("Copy") { UIPasteboard.general.string = flow.message ?? "" }
                Button("OK", role: .cancel) {}
            } message: {
                Text(flow.message ?? "")
            }
    }
}

struct PullMergeAllToolbarButton: View {
    let flow: PullMergeAllFlow

    var body: some View {
        if flow.busy {
            ProgressView()
                .accessibilityLabel("Starting merge agent")
        } else {
            Button {
                Task { await flow.prepare() }
            } label: {
                Label("Merge all", systemImage: "sparkles")
                    .labelStyle(.titleAndIcon)
            }
            .disabled(HostsFeature.hub == nil)
            .accessibilityLabel(PullMergeAllRules.title)
            .accessibilityHint("Asks your desktop to review, merge and clean up every open pull request with an AI agent")
        }
    }
}

extension View {
    func pullMergeAllConfirmation(_ flow: PullMergeAllFlow) -> some View {
        modifier(PullMergeAllConfirmationModifier(flow: flow))
    }
}
