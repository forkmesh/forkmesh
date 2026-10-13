import FMAgents
import SwiftUI

struct BulkAgentActionsBar: View {
    let candidates: [BulkCandidate]
    var onFinished: (String) -> Void = { _ in }
    @State private var model: BulkActionsModel
    @State private var confirming = false

    init(client: AgentOpsClient, candidates: [BulkCandidate], onFinished: @escaping (String) -> Void = { _ in }) {
        self.candidates = candidates
        self.onFinished = onFinished
        _model = State(initialValue: BulkActionsModel(client: client))
    }

    private var stoppable: Int { model.eligible(.stop, in: candidates).count }
    private var continuable: Int { model.eligible(.continueRun, in: candidates).count }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                FmToneButton(
                    label: stoppable > 0 ? "Stop all (\(stoppable))" : "Stop all", tone: .danger,
                    onPressed: handler(.stop, available: stoppable))
                FmToneButton(
                    label: continuable > 0 ? "Continue all (\(continuable))" : "Continue all", tone: .accent,
                    onPressed: handler(.continueRun, available: continuable))
                if model.running { ProgressView() }
            }
            if !model.errorText.isEmpty {
                Text(model.errorText)
                    .font(.footnote)
                    .foregroundStyle(.red)
                    .fmMessageTools(model.errorText)
            }
        }
        .confirmationDialog(
            model.confirmation?.summary ?? "", isPresented: $confirming, titleVisibility: .visible
        ) {
            Button(model.pendingAction?.title ?? "Confirm", role: model.pendingAction == .stop ? .destructive : nil) {
                Task {
                    if let outcome = await model.run() {
                        FmHaptics.commit()
                        onFinished(outcome.message)
                    }
                }
            }
            Button("Cancel", role: .cancel) { model.cancel() }
        }
    }

    private func handler(_ action: BulkAgentAction, available: Int) -> (() -> Void)? {
        guard available > 0, !model.running else { return nil }
        return { begin(action) }
    }

    private func begin(_ action: BulkAgentAction) {
        Task {
            await model.prepare(action, in: candidates)
            confirming = model.confirmation != nil
        }
    }
}
