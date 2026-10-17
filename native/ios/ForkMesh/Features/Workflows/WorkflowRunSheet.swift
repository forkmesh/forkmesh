import FMBase
import FMWorkflows
import SwiftUI

struct WorkflowRunSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let store: WorkflowsStore
    let run: WorkflowRun
    let draft: WorkflowDraft

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    header
                    if !run.error.isEmpty {
                        FmCard(color: fm.danger.fmWithAlpha(fm.dark ? 41 : 26)) {
                            Text(run.error)
                                .fmText(.bodyMedium)
                                .fixedSize(horizontal: false, vertical: true)
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    let steps = run.steps(in: draft)
                    if steps.isEmpty {
                        Text("This run has no step details.")
                            .fmText(.bodyMedium, color: fm.textSecondary)
                    }
                    ForEach(steps) { (step: WorkflowStepResult) in
                        stepCard(step)
                    }
                }
                .padding(FmSpace.x4)
                .padding(.bottom, FmSpace.x6)
            }
            .navigationTitle(WorkflowsText.status(run.status))
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    FmTextButton("Done", onPressed: { dismiss() })
                }
            }
        }
    }

    private var header: some View {
        HStack(spacing: FmSpace.x3) {
            FmIconTile(icon: run.place == .phone ? "smartphone" : "cloud_outlined", tint: run.succeeded ? fm.success : fm.danger)
            VStack(alignment: .leading, spacing: 2) {
                Text(run.place == .phone ? "Ran on this phone" : "Ran on ForkMesh").fmText(.titleMedium)
                Text(details).fmText(.bodySmall, color: fm.textSecondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var details: String {
        var parts: [String] = []
        if run.startedAt > 0 { parts.append(Format.relativeTime(run.startedAt)) }
        if run.finishedAt >= run.startedAt && run.startedAt > 0 { parts.append("\(run.finishedAt - run.startedAt) ms") }
        parts.append(run.requests == 1 ? "1 request" : "\(run.requests) requests")
        return parts.joined(separator: " · ")
    }

    private func stepCard(_ step: WorkflowStepResult) -> some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    FmIcon(WorkflowLook.statusIcon(step.status), size: 18, color: color(step.status))
                    Text(step.name).fmText(.titleSmall).lineLimit(1)
                    Spacer(minLength: 0)
                    Text(step.status == "skipped" ? "Skipped" : "\(step.ms) ms")
                        .fmText(.labelSmall, color: fm.textSecondary)
                }
                if !step.error.isEmpty {
                    Text(step.error)
                        .fmText(.bodySmall, color: fm.danger)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
                ForEach(Array(step.outputs.enumerated()), id: \.offset) { (entry: (offset: Int, element: (name: String, count: Int, items: [JSONValue]))) in
                    output(entry.element.name, count: entry.element.count, items: entry.element.items, single: step.outputs.count == 1)
                }
            }
        }
    }

    private func output(_ name: String, count: Int, items: [JSONValue], single: Bool) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text((single ? "" : name.capitalized + ": ") + (count == 1 ? "1 item" : "\(count) items"))
                .fmText(.labelMedium, color: fm.textSecondary)
            ForEach(Array(items.enumerated()), id: \.offset) { (entry: (offset: Int, element: JSONValue)) in
                Text(JSON.encode(entry.element))
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(fm.textSecondary)
                    .lineLimit(6)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            if count > items.count {
                Text("and \(count - items.count) more").fmText(.bodySmall, color: fm.textTertiary)
            }
        }
    }

    private func color(_ status: String) -> Color {
        switch status {
        case "success": return fm.success
        case "error": return fm.danger
        default: return fm.textSecondary
        }
    }
}
