import FMApi
import FMBase
import FMTools
import SwiftUI

struct MyBugReportsView: View {
    @Environment(\.fmPalette) private var fm
    let model: MyBugReportsModel

    var body: some View {
        VStack(spacing: 0) {
            if model.loading {
                FmLoadingBar()
            }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: FmSpace.x3) {
                    if !model.problem.isEmpty {
                        Text(model.problem)
                            .fmText(.bodySmall, color: fm.danger)
                            .fmMessageTools(model.problem)
                            .accessibilityIdentifier("bug-mine-problem")
                    } else if model.loaded && model.reports.isEmpty {
                        FmEmptyState(icon: "bug_report_outlined", title: MyBugReportsModel.empty)
                    }
                    ForEach(model.reports) { (report: MyBugReport) in
                        MyBugReportRow(report: report, model: model)
                    }
                    if model.nextOffset != nil {
                        FmToneButton(label: "Load more", tone: .neutral, filled: false,
                                     onPressed: model.loading ? nil : { Task { await model.more() } })
                    }
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
            }
            .refreshable { await Task { await model.reload() }.value }
        }
        .task { if !model.loaded { await model.reload() } }
        .accessibilityIdentifier("bug-mine")
    }
}

private struct MyBugReportRow: View {
    @Environment(\.fmPalette) private var fm
    let report: MyBugReport
    let model: MyBugReportsModel

    private var statusColor: Color {
        if report.confirmedFixed { return fm.success }
        if report.status == "triaged" { return fm.accent }
        return fm.textSecondary
    }

    private var details: String { MyBugReportsModel.details(report) }

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    Text(MyBugReportsModel.statusLabel(report))
                        .fmText(.labelMedium, color: statusColor)
                    Spacer(minLength: 0)
                    Text(Format.relativeTimeFrom(report.createdAt))
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                Text(report.message.isEmpty ? "(no message)" : report.message)
                    .fmText(.bodyMedium)
                    .textSelection(.enabled)
                if !details.isEmpty {
                    Text(details)
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                if !report.attachments.isEmpty {
                    Text(report.attachments.map(\.name).joined(separator: " · "))
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                if !report.confirmedFixed {
                    FmToneButton(
                        label: model.confirming.contains(report.id)
                            ? MyBugReportsModel.confirmingLabel : MyBugReportsModel.confirmLabel,
                        tone: .accent, filled: false,
                        busy: model.confirming.contains(report.id),
                        onPressed: model.confirming.contains(report.id) ? nil : {
                            Task { await model.confirmFixed(report.id) }
                        })
                        .accessibilityIdentifier("bug-mine-confirm-\(report.id)")
                }
            }
        }
        .accessibilityIdentifier("bug-mine-report-\(report.id)")
    }
}
