import FMApi
import FMBase
import FMTools
import QuickLook
import SwiftUI

struct BugReportInboxView: View {
    @Environment(\.fmPalette) private var fm
    let model: BugReportInboxModel
    @State private var preview: URL?

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
                        BugReportInboxRow(report: report, model: model, onOpen: { url in preview = url })
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
        .quickLookPreview($preview)
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
    let model: BugReportInboxModel
    let onOpen: (URL) -> Void

    var body: some View {
        FmCard {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(spacing: FmSpace.x2) {
                    Text(BugReportInboxModel.severityLabel(report.severity))
                        .fmText(.labelMedium, color: report.severity == "crash" || report.severity == "high"
                            ? fm.danger : fm.textSecondary)
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
                    attachments
                }
                statusRow
            }
        }
        .accessibilityIdentifier("bug-inbox-report-\(report.id)")
    }

    private var attachments: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(report.attachments, id: \.id) { (attachment: BugReportInboxAttachment) in
                    FmToneButton(
                        label: model.downloading.contains(attachment.id)
                            ? "Opening…" : "\(attachment.name) · \(BugReportInboxModel.sizeLabel(attachment.size))",
                        tone: .neutral, filled: false,
                        onPressed: model.downloading.contains(attachment.id) ? nil : {
                            Task {
                                if let url = await model.open(report: report.id, attachment: attachment) { onOpen(url) }
                            }
                        })
                        .accessibilityIdentifier("bug-inbox-attachment-\(attachment.id)")
                }
            }
        }
    }

    private var statusRow: some View {
        HStack(spacing: FmSpace.x2) {
            ForEach(BugReportStatusFilter.all.filter { !$0.value.isEmpty }, id: \.value) { (choice: BugReportStatusFilter) in
                FmToneButton(
                    label: choice.label,
                    tone: choice.value == report.status ? .accent : .neutral,
                    filled: choice.value == report.status,
                    onPressed: model.updating.contains(report.id) ? nil : {
                        Task { await model.setStatus(report.id, to: choice.value) }
                    })
                    .accessibilityIdentifier("bug-inbox-set-\(choice.value)-\(report.id)")
            }
        }
    }
}
