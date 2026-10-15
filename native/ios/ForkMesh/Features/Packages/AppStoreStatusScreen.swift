import FMApi
import FMBase
import FMPackages
import SwiftUI

struct AppStoreStatusScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: AppStoreStatusModel?

    init() {
        if let api = Services.get(ApiService.self) {
            _model = State(initialValue: AppStoreStatusModel(api: AppStoreStatusAPI(api: api)))
        } else {
            _model = State(initialValue: nil)
        }
    }

    var body: some View {
        content
            .navigationTitle(AppStoreStatusText.title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let model = model {
            AppStoreStatusContent(model: model)
                .task { await model.load() }
        } else {
            FmEmptyState(icon: "phone_iphone", title: AppStoreStatusText.unavailableTitle)
        }
    }
}

private struct AppStoreStatusContent: View {
    let model: AppStoreStatusModel

    var body: some View {
        main
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    FmIconButton(icon: "refresh", label: "Refresh", onPressed: {
                        guard !model.loading else { return }
                        Task { await model.load() }
                    })
                }
            }
    }

    @ViewBuilder private var main: some View {
        if model.showsSpinner {
            FmCircularProgressIndicator()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else if model.showsError {
            refreshableMessage(icon: "error_outline", title: AppStoreStatusText.unavailableTitle, message: model.error)
        } else if model.showsEmpty {
            refreshableMessage(
                icon: "phone_iphone", title: AppStoreStatusText.emptyTitle, message: AppStoreStatusText.emptyMessage)
        } else if let response = model.response, let snapshot = response.snapshot {
            ScrollView {
                AppStoreStatusDashboard(
                    snapshot: snapshot, publishedAt: response.publishedAt, notice: model.refreshError,
                    onDismissNotice: { model.dismissError() })
                    .padding(FmSpace.x4)
            }
            .fmRefreshable { await model.load() }
        }
    }

    private func refreshableMessage(icon: String, title: String, message: String) -> some View {
        ScrollView {
            FmEmptyState(icon: icon, title: title, message: message)
                .containerRelativeFrame([.horizontal, .vertical])
        }
        .fmRefreshable { await model.load() }
    }
}

private struct AppStoreStatusDashboard: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL

    let snapshot: AppStoreSnapshot
    let publishedAt: Int64
    let notice: String
    let onDismissNotice: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            header
            FmNoticeLine(notice.isEmpty ? nil : notice, isError: true, onDismiss: onDismissNotice)
            if !snapshot.attention.isEmpty { attentionCard }
            section(.versions, count: snapshot.versions.count) { versions }
            section(.submissions, count: snapshot.submissions.count) { submissions }
            downloadsSection
            section(.builds, count: snapshot.builds.count) { builds }
            section(.groups, count: snapshot.groups.count) { groups }
            section(.feedback, count: snapshot.feedback.count) { feedback }
            section(.reviews, count: snapshot.reviews.count) { reviews }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var header: some View {
        HStack(spacing: FmSpace.x3) {
            FmIconTile(icon: "phone_iphone", tint: fm.accent)
            VStack(alignment: .leading, spacing: 2) {
                Text(snapshot.bundleId.isEmpty ? AppStoreStatusText.title : snapshot.bundleId)
                    .fmText(.titleMedium)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .textSelection(.enabled)
                TimelineView(.everyMinute) { (context: TimelineViewDefaultContext) in
                    Text(AppStoreStatusText.publishedLine(
                        publishedAt, nowMillis: Int64(context.date.timeIntervalSince1970 * 1000)))
                        .fmText(.bodySmall, color: fm.textSecondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .accessibilityElement(children: .combine)
    }

    private var attentionCard: some View {
        FmCard(color: fm.danger.fmWithAlpha(fm.dark ? 41 : 26)) {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                FmIcon("error_outline", size: 20, color: fm.danger)
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    Text("Needs action")
                        .fmText(.labelMedium, color: fm.danger)
                    ForEach(Array(snapshot.attention.enumerated()), id: \.offset) { (entry: (offset: Int, element: String)) in
                        Text(entry.element)
                            .fmText(.bodyMedium)
                            .fixedSize(horizontal: false, vertical: true)
                            .textSelection(.enabled)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .accessibilityIdentifier("app-store-status-attention")
    }

    @ViewBuilder
    private func section<Rows: View>(_ section: AppStoreSection, count: Int, @ViewBuilder rows: () -> Rows) -> some View {
        FmSectionHeader(title: section.title, count: count > 0 ? count : nil)
            .padding(.top, FmSpace.x2)
        if let reason = snapshot.unavailable(section) {
            unavailableCard(reason)
        } else if count == 0 {
            emptyLine(section.emptyText)
        } else {
            FmCard(padding: EdgeInsets(top: 0, leading: FmSpace.x4, bottom: 0, trailing: FmSpace.x4)) {
                VStack(alignment: .leading, spacing: 0) {
                    rows()
                }
            }
        }
    }

    private func unavailableCard(_ reason: String) -> some View {
        FmCard {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                FmIcon("cloud_off", size: 16, color: fm.warning)
                Text(reason)
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    private func emptyLine(_ text: String) -> some View {
        Text(text)
            .fmText(.bodyMedium, color: fm.textSecondary)
            .padding(.vertical, FmSpace.x1)
    }

    private func row<Content: View>(_ index: Int, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            if index > 0 { FmHairline() }
            VStack(alignment: .leading, spacing: FmSpace.x1) {
                content()
            }
            .padding(.vertical, FmSpace.x3)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private func toneColor(_ tone: AppStoreTone) -> Color {
        switch tone {
        case .action: return fm.danger
        case .pending: return fm.warning
        case .good: return fm.success
        case .neutral: return fm.textSecondary
        }
    }

    private func badge(_ status: AppStoreState) -> some View {
        FmStatusBadge(label: status.text, color: toneColor(status.tone))
            .fixedSize()
            .help(status.state)
    }

    private func meta(_ parts: [String]) -> some View {
        Text(parts.filter { !$0.isEmpty }.joined(separator: " \u{00B7} "))
            .fmText(.bodySmall, color: fm.textSecondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private func titleLine(_ title: String, status: AppStoreState?) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
            Text(title)
                .fmText(.titleSmall)
                .lineLimit(2)
                .frame(maxWidth: .infinity, alignment: .leading)
            if let status = status { badge(status) }
        }
    }

    private var versions: some View {
        ForEach(Array(snapshot.versions.enumerated()), id: \.offset) { (entry: (offset: Int, element: AppStoreVersion)) in
            let version = entry.element
            row(entry.offset) {
                titleLine(AppStoreStatusText.versionTitle(version: version.version, build: version.build), status: version.status)
                meta([
                    AppStoreStatusText.releaseType(version.releaseType),
                    version.createdAt.isEmpty ? "" : "Created \(AppStoreStatusText.date(version.createdAt))",
                ])
            }
        }
    }

    private var submissions: some View {
        ForEach(Array(snapshot.submissions.enumerated()), id: \.offset) { (entry: (offset: Int, element: AppStoreSubmission)) in
            let submission = entry.element
            row(entry.offset) {
                titleLine(
                    submission.submittedAt.isEmpty
                        ? "Submission" : "Submitted \(AppStoreStatusText.date(submission.submittedAt))",
                    status: submission.status)
                ForEach(Array(submission.items.enumerated()), id: \.offset) { (item: (offset: Int, element: AppStoreSubmissionItem)) in
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        FmIcon("chevron_right", size: 14, color: fm.textTertiary)
                        Text(item.element.name.isEmpty ? "Item" : item.element.name)
                            .fmText(.bodyMedium)
                            .frame(maxWidth: .infinity, alignment: .leading)
                        badge(item.element.status)
                    }
                    .padding(.leading, FmSpace.x2)
                }
            }
        }
    }

    @ViewBuilder private var downloadsSection: some View {
        let downloads = snapshot.downloads
        FmSectionHeader(title: AppStoreSection.downloads.title)
            .padding(.top, FmSpace.x2)
        if let reason = snapshot.unavailable(.downloads) {
            unavailableCard(reason)
        } else if !downloads.available {
            unavailableCard(downloads.note.isEmpty ? AppStoreSection.downloads.emptyText : downloads.note)
        } else {
            FmCard {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    Text(AppStoreStatusText.totalTitle(downloads))
                        .fmText(.labelSmall)
                    HStack(alignment: .top, spacing: FmSpace.x3) {
                        figure(downloads.total.downloads, "Downloads")
                        figure(downloads.total.redownloads, "Redownloads")
                        figure(downloads.total.updates, "Updates")
                    }
                    if !downloads.recent.isEmpty {
                        FmHairline()
                        recentGrid(downloads.recent)
                    }
                    if !downloads.note.isEmpty {
                        Text(downloads.note)
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
        }
    }

    private func figure(_ value: Int, _ caption: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value.formatted())
                .fmText(.titleLarge, tabular: true)
                .lineLimit(1)
                .minimumScaleFactor(0.6)
            Text(caption)
                .fmText(.bodySmall, color: fm.textSecondary)
                .lineLimit(1)
                .minimumScaleFactor(0.8)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityElement(children: .combine)
    }

    private func recentGrid(_ spans: [AppStoreDownloadFigures]) -> some View {
        Grid(alignment: .leading, horizontalSpacing: FmSpace.x3, verticalSpacing: FmSpace.x2) {
            GridRow {
                Text("Period").fmText(.labelSmall)
                Text("Down").fmText(.labelSmall).gridColumnAlignment(.trailing)
                Text("Re").fmText(.labelSmall).gridColumnAlignment(.trailing)
                Text("Updates").fmText(.labelSmall).gridColumnAlignment(.trailing)
            }
            ForEach(Array(spans.enumerated()), id: \.offset) { (entry: (offset: Int, element: AppStoreDownloadFigures)) in
                GridRow {
                    Text(AppStoreStatusText.spanTitle(entry.element))
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    Text(entry.element.downloads.formatted()).fmText(.bodyMedium, tabular: true)
                    Text(entry.element.redownloads.formatted()).fmText(.bodyMedium, tabular: true)
                    Text(entry.element.updates.formatted()).fmText(.bodyMedium, tabular: true)
                }
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(
                    "\(AppStoreStatusText.spanTitle(entry.element)): \(entry.element.downloads) downloads, \(entry.element.redownloads) redownloads, \(entry.element.updates) updates"))
            }
        }
    }

    private var builds: some View {
        ForEach(Array(snapshot.builds.enumerated()), id: \.offset) { (entry: (offset: Int, element: AppStoreBuild)) in
            let build = entry.element
            row(entry.offset) {
                titleLine(
                    AppStoreStatusText.versionTitle(version: build.version, build: build.build),
                    status: build.expired ? AppStoreState(state: "EXPIRED", label: "Expired", tone: .neutral) : nil)
                if !build.uploadedAt.isEmpty {
                    meta(["Uploaded \(AppStoreStatusText.date(build.uploadedAt))"])
                }
                Grid(alignment: .leading, horizontalSpacing: FmSpace.x3, verticalSpacing: FmSpace.x1) {
                    ForEach(buildStates(build), id: \.label) { (state: (label: String, status: AppStoreState)) in
                        GridRow {
                            Text(state.label).fmText(.bodySmall, color: fm.textSecondary)
                            badge(state.status)
                        }
                    }
                }
                .padding(.top, 2)
                if !build.groups.isEmpty {
                    meta(["Groups: \(build.groups.joined(separator: ", "))"])
                }
            }
        }
    }

    private func buildStates(_ build: AppStoreBuild) -> [(label: String, status: AppStoreState)] {
        let states: [(label: String, status: AppStoreState)] = [
            ("Processing", build.processing), ("Internal", build.internalTesting),
            ("External", build.externalTesting), ("Beta review", build.betaReview),
        ]
        return states.filter { !$0.status.isEmpty }
    }

    private var groups: some View {
        let sorted = snapshot.sortedGroups
        return ForEach(Array(sorted.enumerated()), id: \.offset) { (entry: (offset: Int, element: AppStoreGroup)) in
            let group = entry.element
            row(entry.offset) {
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                    Text(group.name.isEmpty ? "Group" : group.name)
                        .fmText(.titleSmall)
                        .frame(maxWidth: .infinity, alignment: .leading)
                    FmStatusBadge(label: AppStoreStatusText.groupKind(group), color: fm.textSecondary)
                        .fixedSize()
                }
                meta([
                    AppStoreStatusText.testers(group.testers), AppStoreStatusText.groupBuilds(group),
                    AppStoreStatusText.groupFeedback(group),
                ])
                if let url = group.publicURL {
                    Button {
                        FmHaptics.tap()
                        openURL(url)
                    } label: {
                        HStack(spacing: FmSpace.x1) {
                            FmIcon("link", size: 14, color: fm.accent)
                            Text(group.publicLink)
                                .fmText(.bodySmall, color: fm.accent)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        }
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(Text("Open the public TestFlight link for \(group.name)"))
                    let limit = AppStoreStatusText.publicLinkLimit(group)
                    if !limit.isEmpty { meta([limit]) }
                } else if group.publicLinkEnabled {
                    meta(["Public link on"])
                }
            }
        }
    }

    private var feedback: some View {
        ForEach(Array(snapshot.feedback.enumerated()), id: \.offset) { (entry: (offset: Int, element: AppStoreFeedback)) in
            let item = entry.element
            row(entry.offset) {
                meta([
                    item.kind.isEmpty ? "Feedback" : AppStoreStatusText.humanize(item.kind),
                    item.createdAt.isEmpty ? "" : AppStoreStatusText.date(item.createdAt),
                ])
                if !item.comment.isEmpty {
                    Text(item.comment)
                        .fmText(.bodyMedium)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
                meta([item.from, item.device])
            }
        }
    }

    private var reviews: some View {
        ForEach(Array(snapshot.reviews.enumerated()), id: \.offset) { (entry: (offset: Int, element: AppStoreReview)) in
            let review = entry.element
            row(entry.offset) {
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                    Text(review.stars)
                        .fmText(.bodyMedium, color: fm.warning)
                        .accessibilityLabel(Text("\(review.rating) of 5 stars"))
                    Text(review.title)
                        .fmText(.titleSmall)
                        .lineLimit(2)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                if !review.body.isEmpty {
                    Text(review.body)
                        .fmText(.bodyMedium)
                        .fixedSize(horizontal: false, vertical: true)
                        .textSelection(.enabled)
                }
                meta([review.territory, review.createdAt.isEmpty ? "" : AppStoreStatusText.date(review.createdAt)])
            }
        }
    }
}
