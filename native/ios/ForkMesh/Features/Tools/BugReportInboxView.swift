import FMApi
import FMBase
import FMTools
import SwiftUI

struct BugReportInboxView: View {
    @Environment(\.fmPalette) private var fm
    let model: BugReportInboxModel

    var body: some View {
        VStack(spacing: 0) {
            filters
            if model.loading {
                FmLoadingBar()
            }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: FmSpace.x3) {
                    if !model.problem.isEmpty {
                        Text(model.problem)
                            .fmText(.bodySmall, color: fm.danger)
                            .fmMessageTools(model.problem)
                            .accessibilityIdentifier("bug-inbox-problem")
                    } else if model.loaded && model.reports.isEmpty {
                        FmEmptyState(icon: "bug_report_outlined", title: BugReportInboxModel.empty)
                    }
                    ForEach(model.reports) { (report: BugReportInboxItem) in
                        BugReportInboxRow(report: report)
                    }
                    if model.nextOffset != nil {
                        FmToneButton(label: "Load more", tone: .neutral, filled: false,
                                     onPressed: model.loading ? nil : { Task { await model.more() } })
                    }
                }
                .padding(.horizontal, FmSpace.x4)
                .padding(.vertical, FmSpace.x3)
            }
            .refreshable { await model.reload() }
        }
        .task { if !model.loaded { await model.reload() } }
        .accessibilityIdentifier("bug-inbox")
    }

    private var filters: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(BugReportStatusFilter.all, id: \.value) { (filter: BugReportStatusFilter) in
                    FmToneButton(
                        label: model.label(for: filter),
                        tone: filter.value == model.status ? .accent : .neutral,
                        filled: filter.value == model.status,
                        onPressed: { Task { await model.select(filter.value) } })
                        .accessibilityIdentifier("bug-inbox-status-\(filter.value.isEmpty ? "all" : filter.value)")
                }
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.vertical, FmSpace.x2)
        }
    }
}

private struct BugReportInboxRow: View {
    @Environment(\.fmPalette) private var fm
    let report: BugReportInboxItem

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    Text(BugReportInboxModel.severityLabel(report.severity))
                        .fmText(.labelMedium, color: report.severity == "crash" || report.severity == "high"
                            ? fm.danger : fm.textSecondary)
                    Text(report.status.capitalized)
                        .fmText(.labelMedium, color: report.status == "new" ? fm.accent : fm.textTertiary)
                    Spacer(minLength: 0)
                    Text(Format.relativeTimeFrom(report.createdAt))
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                Text(report.message.isEmpty ? "(no message)" : report.message)
                    .fmText(.bodyMedium)
                    .textSelection(.enabled)
                Text(BugReportInboxModel.details(report))
                    .fmText(.bodySmall, color: fm.textTertiary)
                if !report.contact.isEmpty {
                    Text("Reply to \(report.contact)")
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .textSelection(.enabled)
                }
                if !report.attachments.isEmpty {
                    Text(report.attachments.map(\.name).joined(separator: " · "))
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
            }
        }
        .accessibilityIdentifier("bug-inbox-report-\(report.id)")
    }
}
