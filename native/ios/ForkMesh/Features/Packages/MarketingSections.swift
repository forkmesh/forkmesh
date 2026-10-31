import FMBase
import FMPackages
import SwiftUI
import UIKit

struct MarketingSectionBody: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let section: MarketingSection
    let present: (MarketingSheet) -> Void
    let select: (MarketingSection) -> Void

    var body: some View {
        switch section {
        case .overview: MarketingOverviewSection(model: model, overview: overview, present: present, select: select)
        case .plan: MarketingContentSection(model: model, overview: overview, present: present)
        case .campaigns: MarketingCampaignsSection(model: model, overview: overview, present: present)
        case .channels: MarketingChannelsSection(model: model, overview: overview, present: present)
        case .ads: MarketingAdsSection(model: model, overview: overview, present: present)
        case .growth: MarketingGrowthSection(model: model, overview: overview, present: present)
        case .reddit: MarketingRedditSection(model: model, overview: overview, present: present)
        case .performance: MarketingPerformanceSection(model: model, overview: overview, present: present)
        }
    }
}

struct MarketingRecordRow: View {
    let record: MarketingRecord
    let overview: MarketingOverview
    var working = false

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(record.title).font(.body.weight(.medium)).lineLimit(2)
                Spacer()
                if working { ProgressView() }
                if record.needsAttention {
                    Image(systemName: "exclamationmark.circle.fill").foregroundStyle(.orange).accessibilityLabel("Needs attention")
                }
            }
            let facts = record.facts(overview) + record.dates.map { "\($0.label) \(PackagesFormat.date($0.ms))" }
            if !facts.isEmpty {
                Text(facts.joined(separator: " · ")).font(.caption).foregroundStyle(.secondary)
            }
        }
        .accessibilityElement(children: .combine)
    }
}

struct MarketingStatusMenu: View {
    let model: MarketingModel
    let kind: MarketingKind
    let id: String
    let current: String

    var body: some View {
        Menu("Move to", systemImage: "arrow.right.circle") {
            ForEach(kind.statuses.filter { $0 != current }, id: \.self) { status in
                Button(MarketingText.title(status)) { Task { await model.setStatus(kind, id: id, status: status) } }
            }
        }
    }
}

struct MarketingOverviewSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void
    let select: (MarketingSection) -> Void

    var body: some View {
        Section("This organization") {
            LabeledContent("Content published", value: "\(overview.published)")
            LabeledContent("Queued or scheduled", value: "\(overview.queued)")
            LabeledContent("Followers", value: PackagesFormat.count(overview.followers))
            LabeledContent("Reach", value: PackagesFormat.count(overview.reach))
            LabeledContent("Engagements", value: PackagesFormat.count(overview.engagements))
            Button {
                select(.channels)
            } label: {
                LabeledContent("Channels on track",
                               value: "\(overview.healthCounts["good"] ?? 0) of \(overview.channels.count)")
            }
            Button {
                select(.ads)
            } label: {
                LabeledContent("Active ads", value: "\(overview.ads.filter { $0.status == "active" }.count)"
                               + (overview.adsNeedingAttention > 0 ? " · \(overview.adsNeedingAttention) off pace" : ""))
            }
            Button {
                select(.growth)
            } label: {
                LabeledContent("Experiments running", value: "\(overview.experimentsRunning) of \(overview.experimentsRunningLimit)")
            }
            Button {
                select(.reddit)
            } label: {
                LabeledContent("Reddit threads to answer", value: "\(overview.openRedditThreads.count)")
            }
        }
        .foregroundStyle(.primary)
        if !overview.suggestions.isEmpty {
            Section("Next steps") {
                ForEach(overview.suggestions) { suggestion in
                    Button {
                        open(suggestion)
                    } label: {
                        MarketingInsightRow(level: suggestion.level, title: suggestion.title, detail: suggestion.detail)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
        Section {
            if overview.assistantAvailable {
                Button("Ask the marketing assistant", systemImage: "sparkles") { present(.assistant("")) }
            }
            Button("Weekly report", systemImage: "doc.text") { present(.report) }
            Button("Build a tagged link", systemImage: "link") { present(.link) }
            if overview.canManageSettings {
                if overview.x.connected {
                    Button("Post to X as @\(overview.x.username)", systemImage: "paperplane") {
                        present(.form(MarketingForms.xPostDraft(overview)))
                    }
                }
                Button("Marketing settings", systemImage: "gearshape") { present(.settings) }
            }
        } header: {
            Text("Tools")
        } footer: {
            if overview.x.connected && overview.x.lastPostedAt > 0 {
                Text("Last posted to X \(PackagesFormat.relative(overview.x.lastPostedAt)).")
            }
        }
    }

    private func open(_ suggestion: MarketingSuggestion) {
        if suggestion.actionType == "channel" {
            present(.channel(suggestion.action.str("channel")))
        } else if let draft = MarketingForms.suggestionDraft(suggestion, overview: overview, zone: model.timeZone) {
            if suggestion.actionType == "campaign-edit" {
                present(.campaign(draft.id))
            } else {
                present(.form(draft))
            }
        }
    }
}

struct MarketingInsightRow: View {
    let level: String
    let title: String
    let detail: String

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Image(systemName: icon).foregroundStyle(color)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.subheadline.weight(.medium))
                if !detail.isEmpty { Text(detail).font(.caption).foregroundStyle(.secondary) }
            }
        }
        .accessibilityElement(children: .combine)
    }

    private var icon: String {
        switch level {
        case "warning": return "exclamationmark.triangle.fill"
        case "action": return "arrow.right.circle.fill"
        case "setup": return "wrench.and.screwdriver.fill"
        case "win": return "star.fill"
        default: return "info.circle.fill"
        }
    }

    private var color: Color {
        switch level {
        case "warning": return .orange
        case "action": return .blue
        case "win": return .green
        default: return .secondary
        }
    }
}

struct MarketingContentSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void
    @State private var query = ""
    @State private var status = ""
    @State private var channel = ""

    var body: some View {
        if model.readyX.available {
            MarketingReadyXSection(model: model, overview: overview)
        }
        Section {
            TextField("Search content", text: $query)
            Picker("Status", selection: $status) {
                Text("Every status").tag("")
                ForEach(MarketingContent.statuses, id: \.self) { Text(MarketingText.title($0)).tag($0) }
            }
            Picker("Channel", selection: $channel) {
                Text("Every channel").tag("")
                ForEach(Array(Set(overview.content.map(\.channel))).sorted(), id: \.self) { Text(overview.channelName($0)).tag($0) }
            }
        } footer: {
            Text("\(overview.published) published · \(overview.queued) queued")
        }
        let items = overview.content(query: query, status: status, channel: channel)
        if items.isEmpty {
            ContentUnavailableView("No content planned", systemImage: "calendar.badge.plus")
        }
        let statuses = status.isEmpty ? MarketingContent.statuses : [status]
        ForEach(statuses, id: \.self) { group in
            let rows = items.filter { $0.status == group }
            if !rows.isEmpty {
                Section(MarketingText.title(group)) {
                    ForEach(rows) { item in row(item) }
                }
            }
        }
    }

    private func row(_ item: MarketingContent) -> some View {
        Button {
            present(.form(MarketingDraft(kind: .content, id: item.id, overview: overview, json: item.json, zone: model.timeZone)))
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                HStack {
                    Text(item.title).font(.body.weight(.medium))
                    Spacer()
                    if model.isWorking(item.id) { ProgressView() }
                }
                Text([overview.channelName(item.channel), item.campaign,
                      item.scheduledAt > 0 ? PackagesFormat.date(item.scheduledAt) : "",
                      item.publishedAt > 0 ? "Published " + PackagesFormat.date(item.publishedAt) : ""]
                    .filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .buttonStyle(.plain)
        .contextMenu {
            MarketingStatusMenu(model: model, kind: .content, id: item.id, current: item.status)
            if item.channel == "x" && overview.x.connected && overview.canManageSettings && item.status != "published" {
                Button("Post to X now", systemImage: "paperplane") { present(.form(MarketingForms.xPostDraft(overview, content: item))) }
            }
            if let url = URL(string: item.url), item.url.hasPrefix("https://") {
                Link(destination: url) { Label("Open published post", systemImage: "safari") }
            }
            Button("Delete", systemImage: "trash", role: .destructive) { Task { _ = await model.delete(.content, id: item.id) } }
        }
        .swipeActions(edge: .leading) {
            if item.status != "published" {
                Button {
                    Task { await model.setStatus(.content, id: item.id, status: "published") }
                } label: {
                    Label("Published", systemImage: "checkmark.circle")
                }
                .tint(.green)
            }
        }
        .swipeActions(edge: .trailing) {
            Button(role: .destructive) {
                Task { _ = await model.delete(.content, id: item.id) }
            } label: {
                Label("Delete", systemImage: "trash")
            }
        }
    }
}

struct MarketingReadyXSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    @State private var picture: (id: String, image: UIImage)?
    @State private var sharing: AgentShareItem?
    @State private var sharedId = ""
    @State private var link = (id: "", url: "")

    var body: some View {
        let ready = model.readyX
        Section {
            if let post = ready.post {
                card(post)
            } else {
                Label(MarketingReadyState.emptyText, systemImage: "sparkles")
                    .foregroundStyle(.secondary)
                if let last = ready.lastClosed { closed(last) }
            }
        } header: {
            Text("Ready for X")
        }
    }

    @ViewBuilder
    private func card(_ post: MarketingReadyPost) -> some View {
        let image = picture?.id == post.id ? picture?.image : nil
        VStack(alignment: .leading, spacing: 8) {
            if let image {
                Image(uiImage: image)
                    .resizable()
                    .scaledToFit()
                    .frame(maxWidth: .infinity, maxHeight: 260)
                    .clipShape(RoundedRectangle(cornerRadius: 10, style: .continuous))
                    .accessibilityLabel(post.imageAlt.isEmpty ? "Post image" : post.imageAlt)
            } else if post.hasImage {
                ProgressView().frame(maxWidth: .infinity, minHeight: 80)
            }
            Text(post.text).textSelection(.enabled)
            HStack(alignment: .firstTextBaseline) {
                Text(post.lengthLabel)
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(post.tooLong ? Color.red : post.nearLimit ? Color.orange : Color.secondary)
                Spacer()
                Text(written(post)).font(.caption).foregroundStyle(.secondary)
            }
            if !post.why.isEmpty {
                Text(post.why).font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
        .task(id: post.id + (post.imageDataUrl.isEmpty ? "" : ":image")) { await loadImage() }
        Button {
            share(post, image: image)
        } label: {
            Label("Post to X", systemImage: "paperplane.fill").frame(maxWidth: .infinity)
        }
        .buttonStyle(.borderedProminent)
        .sheet(item: $sharing) { item in
            AgentActivityView(items: item.items, onFinish: { activity, completed in
                let id = sharedId
                Task { await model.sharedReadyPost(id: id, activity: activity, completed: completed) }
            }, onDone: { sharing = nil })
            .presentationDetents([.medium, .large])
        }
        HStack {
            Button {
                UIPasteboard.general.string = post.text
                model.notice = "Post text copied."
            } label: {
                Label("Copy text", systemImage: "doc.on.doc").frame(maxWidth: .infinity)
            }
            if let image {
                Button {
                    UIPasteboard.general.image = image
                    model.notice = "Image copied."
                } label: {
                    Label("Copy image", systemImage: "photo.on.rectangle").frame(maxWidth: .infinity)
                }
            }
        }
        .buttonStyle(.bordered)
        if overview.canCloseReadyPost {
            TextField("Link to the post (optional)", text: Binding(
                get: { link.id == post.id ? link.url : "" },
                set: { link = (id: post.id, url: $0) }))
                .keyboardType(.URL)
                .textContentType(.URL)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .submitLabel(.done)
                .accessibilityLabel("Link to the X post")
                .disabled(model.isWorking("ready-post"))
            HStack {
                Button {
                    let url = link.id == post.id ? link.url.trimmingCharacters(in: .whitespacesAndNewlines) : ""
                    Task { await model.closeReadyPost(id: post.id, status: "posted", url: url) }
                } label: {
                    Label("Mark posted", systemImage: "checkmark.circle").frame(maxWidth: .infinity)
                }
                Button {
                    Task { await model.closeReadyPost(id: post.id, status: "skipped") }
                } label: {
                    Label("Skip", systemImage: "forward").frame(maxWidth: .infinity)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.isWorking("ready-post"))
        }
    }

    private func closed(_ last: MarketingReadyClosed) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(["Last post: " + last.statusTitle, PackagesFormat.relative(last.closedAt), last.closedBy]
                .filter { !$0.isEmpty }.joined(separator: " · "))
                .font(.caption)
            if !last.text.isEmpty {
                Text(last.text).font(.caption).foregroundStyle(.secondary).lineLimit(3)
            }
            if let url = last.link {
                Link(destination: url) { Label("Open the post", systemImage: "safari") }.font(.caption)
            }
        }
    }

    private func written(_ post: MarketingReadyPost) -> String {
        var text = "Written"
        if post.createdAt > 0 { text += " " + PackagesFormat.relative(post.createdAt) }
        if !post.writer.isEmpty { text += " by " + post.writer }
        return text
    }

    private func loadImage() async {
        await model.loadReadyPost()
        guard let post = model.readyX.post else { return }
        if picture?.id == post.id { return }
        if let data = post.imageData, let image = UIImage(data: data) { picture = (post.id, image) }
    }

    private func share(_ post: MarketingReadyPost, image: UIImage?) {
        var items: [Any] = []
        if let image { items.append(image) }
        items.append(post.text)
        sharedId = post.id
        sharing = AgentShareItem(items: items, cleanup: {})
    }
}

struct MarketingCampaignsSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void

    var body: some View {
        let open = overview.campaigns.filter { !["completed", "archived"].contains($0.status) }
        let closed = overview.campaigns.filter { ["completed", "archived"].contains($0.status) }
        if overview.campaigns.isEmpty {
            ContentUnavailableView("No campaigns yet", systemImage: "flag",
                                   description: Text("A campaign names one audience, one message, dates and a target."))
        }
        if !overview.campaignTemplates.isEmpty {
            Section("Start from a template") {
                ForEach(overview.campaignTemplates.indices, id: \.self) { index in
                    let template = overview.campaignTemplates[index]
                    Button(template.str("name"), systemImage: "doc.on.doc") {
                        present(.form(MarketingForms.templateDraft(template, overview: overview, zone: model.timeZone)))
                    }
                }
            }
        }
        if !open.isEmpty { Section("Open") { ForEach(open) { row($0) } } }
        if !closed.isEmpty { Section("Finished") { ForEach(closed) { row($0) } } }
    }

    private func row(_ campaign: MarketingRecord) -> some View {
        Button {
            present(.campaign(campaign.id))
        } label: {
            VStack(alignment: .leading, spacing: 2) {
                Text(MarketingText.title(campaign.status)).font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
                MarketingRecordRow(record: campaign, overview: overview, working: model.isWorking(campaign.id))
            }
        }
        .buttonStyle(.plain)
        .contextMenu {
            MarketingStatusMenu(model: model, kind: .campaign, id: campaign.id, current: campaign.status)
            Button("Delete", systemImage: "trash", role: .destructive) { Task { _ = await model.delete(.campaign, id: campaign.id) } }
        }
    }
}

struct MarketingChannelsSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void
    @State private var filter = ""

    var body: some View {
        Section {
            Picker("Show", selection: $filter) {
                Text("All channels").tag("")
                ForEach(["good", "attention", "stale", "setup", "paused"], id: \.self) { state in
                    Text("\(MarketingChannelStats.healthTitles[state] ?? state) (\(overview.healthCounts[state] ?? 0))").tag(state)
                }
            }
            if !overview.staleChannels.isEmpty {
                Button("Update stale channels", systemImage: "arrow.clockwise") { Task { await model.refreshStale() } }
            }
            if overview.canManageSettings {
                Button("Add channels", systemImage: "plus.circle") { present(.addChannels) }
            }
        } footer: {
            Text("Tap a channel to log its numbers, refresh it, or change its link. Channels the server cannot read are updated with the browser extension on a computer, or by hand here.")
        }
        let rows = overview.channels.filter { filter.isEmpty || $0.health == filter }
        Section("Channels") {
            if rows.isEmpty { Text("No channels here.").foregroundStyle(.secondary) }
            ForEach(rows) { channel in
                Button {
                    present(.channel(channel.id))
                } label: {
                    MarketingChannelRow(overview: overview, channel: channel, refreshing: model.isWorking("refresh:" + channel.id))
                }
                .buttonStyle(.plain)
                .swipeActions(edge: .leading) {
                    if overview.refreshable.contains(channel.id) {
                        Button {
                            Task { await model.refreshChannel(channel.id) }
                        } label: {
                            Label("Refresh", systemImage: "arrow.clockwise")
                        }
                        .tint(.blue)
                    }
                }
            }
        }
        Section {
            if overview.posts.isEmpty { Text("No posted links yet.").foregroundStyle(.secondary) }
            ForEach(overview.posts) { post in
                Button {
                    present(.form(MarketingDraft(kind: .post, id: post.id, overview: overview, json: post.json, zone: model.timeZone)))
                } label: {
                    MarketingRecordRow(record: post, overview: overview, working: model.isWorking(post.id))
                }
                .buttonStyle(.plain)
                .swipeActions(edge: .trailing) {
                    Button(role: .destructive) {
                        Task { _ = await model.delete(.post, id: post.id) }
                    } label: {
                        Label("Delete", systemImage: "trash")
                    }
                }
            }
        } header: {
            Text("Posted links")
        } footer: {
            Text("Track the views, likes and replies of things you posted on any channel.")
        }
    }
}

struct MarketingChannelRow: View {
    let overview: MarketingOverview
    let channel: MarketingChannelStats
    var refreshing = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(overview.channelName(channel.id)).font(.body.weight(.medium))
                Spacer()
                if refreshing { ProgressView() }
                if !channel.healthTitle.isEmpty {
                    Text(channel.healthTitle)
                        .font(.caption.weight(.medium))
                        .foregroundStyle(channel.health == "good" ? .green : channel.health == "paused" ? .secondary : .orange)
                }
            }
            Text(MarketingChannelFacts.text(channel)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            if !channel.missingChecks.isEmpty {
                Text(channel.missingChecks.joined(separator: " · ")).font(.caption2).foregroundStyle(.orange)
                    .fmMessageTools(channel.missingChecks.joined(separator: " · "))
            }
            Text(channel.observedAt > 0 ? "Updated \(PackagesFormat.date(channel.observedAt))" : "Never updated")
                .font(.caption2)
                .foregroundStyle(.secondary)
        }
        .accessibilityElement(children: .combine)
    }
}

enum MarketingChannelFacts {
    static func text(_ channel: MarketingChannelStats) -> String {
        if channel.isEmail {
            var parts = ["\(PackagesFormat.count(channel.outbound)) emails sent in 30d", "\(PackagesFormat.count(channel.closedDeals)) closed deals"]
            if channel.closedValue > 0 { parts.append("\(PackagesFormat.count(channel.closedValue)) won") }
            return parts.joined(separator: " · ")
        }
        if channel.isGoogle {
            return ["\(PackagesFormat.count(channel.visits)) visits from Google in 30d",
                    "\(PackagesFormat.count(channel.indexedPages)) pages on Google"].joined(separator: " · ")
        }
        if channel.isAppStore {
            var parts = ["\(PackagesFormat.count(channel.downloads)) downloads in 30d", "\(PackagesFormat.count(channel.ratings)) ratings"]
            if channel.rating > 0 { parts.append(MarketingAppStore.ratingText(channel.rating)) }
            return parts.joined(separator: " · ")
        }
        var parts: [String] = []
        if channel.isGitHub {
            parts.append("\(PackagesFormat.count(channel.stars)) stars")
            if let change = channel.starsChange { parts.append(MarketingText.signed(change) + " in 30d") }
            parts.append("\(PackagesFormat.count(channel.forks)) forks")
        }
        parts.append("\(PackagesFormat.count(channel.followers)) followers")
        if let change = channel.followersChange { parts.append(MarketingText.signed(change) + " in 30d") }
        if let rate = channel.engagementRate { parts.append(String(format: "%.1f%% engagement", rate)) }
        parts.append("\(channel.postsLast30d) posts in 30d")
        if channel.rating > 0 { parts.append(String(format: "%.1f★ from %@ reviews", channel.rating, PackagesFormat.count(channel.reviews))) }
        return parts.joined(separator: " · ")
    }
}

struct MarketingAdsSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void
    @State private var status = ""
    @State private var query = ""

    var body: some View {
        if !overview.adTotals.isEmpty {
            Section("Totals") {
                ForEach(overview.adTotals) { totals in
                    VStack(alignment: .leading, spacing: 2) {
                        Text("\(totals.currency) · \(totals.active) active of \(totals.ads)").font(.subheadline.weight(.medium))
                        Text(totals.facts.joined(separator: " · ")).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    }
                }
            }
        }
        Section {
            TextField("Search ads", text: $query)
            Picker("Status", selection: $status) {
                Text("Every status").tag("")
                ForEach(MarketingCampaignAd.statuses, id: \.self) { Text(MarketingText.title($0)).tag($0) }
            }
        }
        let ads = overview.ads(status: status, query: query)
        Section {
            if ads.isEmpty { Text("No ads here.").foregroundStyle(.secondary) }
            ForEach(ads) { ad in row(ad) }
        }
    }

    private func row(_ ad: MarketingCampaignAd) -> some View {
        Button {
            present(.form(MarketingDraft(kind: .ad, id: ad.id, overview: overview, json: ad.json, zone: model.timeZone)))
        } label: {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text(ad.name.isEmpty ? ad.headline : ad.name).font(.body.weight(.medium))
                    Spacer()
                    if model.isWorking(ad.id) { ProgressView() }
                    Text(MarketingText.title(ad.status)).font(.caption).foregroundStyle(.secondary)
                }
                Text([overview.channelName(ad.channel), ad.platform, ad.campaign].filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                HStack(spacing: 12) {
                    Text(ad.spendLabel)
                    Text("\(PackagesFormat.count(ad.impressions)) impressions")
                    Text("\(PackagesFormat.count(ad.clicks)) clicks")
                    if !ad.pacing.isEmpty { Text(MarketingText.title(ad.pacing)) }
                }
                .font(.caption2.monospacedDigit())
                .foregroundStyle(ad.needsAttention ? .orange : .secondary)
                if !ad.linkChecks.isEmpty {
                    Text(ad.linkChecks.joined(separator: " ")).font(.caption2).foregroundStyle(.orange)
                        .fmMessageTools(ad.linkChecks.joined(separator: " "))
                }
            }
        }
        .buttonStyle(.plain)
        .contextMenu {
            Button("Log results", systemImage: "chart.bar.doc.horizontal") {
                present(.form(MarketingDraft(kind: .adResults, id: ad.id, overview: overview, json: ad.json, zone: model.timeZone)))
            }
            MarketingStatusMenu(model: model, kind: .ad, id: ad.id, current: ad.status)
            if let url = URL(string: ad.consoleUrl), ad.consoleUrl.hasPrefix("https://") {
                Link(destination: url) { Label("Open the ads manager", systemImage: "safari") }
            }
            Button("Delete", systemImage: "trash", role: .destructive) { Task { _ = await model.delete(.ad, id: ad.id) } }
        }
        .swipeActions(edge: .leading) {
            if ad.status == "active" {
                Button("Pause") { Task { await model.setStatus(.ad, id: ad.id, status: "paused") } }.tint(.orange)
            } else if ad.status == "paused" {
                Button("Resume") { Task { await model.setStatus(.ad, id: ad.id, status: "active") } }.tint(.green)
            }
            Button("Results") {
                present(.form(MarketingDraft(kind: .adResults, id: ad.id, overview: overview, json: ad.json, zone: model.timeZone)))
            }
            .tint(.blue)
        }
    }
}

struct MarketingGrowthSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void

    var body: some View {
        Section {
            if !overview.funnelReport.bottleneckReason.isEmpty {
                Text(overview.funnelReport.bottleneckReason).font(.subheadline)
            }
            ForEach(overview.funnelReport.rates) { rate in
                LabeledContent(rate.label) {
                    Text(rate.text).foregroundStyle(rate.id == overview.funnelReport.bottleneck || rate.belowTarget ? .orange : .secondary)
                }
            }
            if let draft = model.newDraft(.funnelWeek) {
                Button("Log a week", systemImage: "plus.circle") { present(.form(draft)) }
            }
            if overview.canManageSettings, let draft = model.newDraft(.targets) {
                Button("Funnel targets", systemImage: "target") { present(.form(draft)) }
            }
        } header: {
            Text("Funnel")
        }
        if !overview.funnel.isEmpty {
            Section("Weeks") {
                ForEach(overview.funnel) { week in
                    Button {
                        present(.form(MarketingDraft(kind: .funnelWeek, id: week.id, overview: overview, json: week.json, zone: model.timeZone)))
                    } label: {
                        MarketingRecordRow(record: week, overview: overview, working: model.isWorking(week.id))
                    }
                    .buttonStyle(.plain)
                    .swipeActions(edge: .trailing) {
                        Button(role: .destructive) {
                            Task { _ = await model.delete(.funnelWeek, id: week.id) }
                        } label: {
                            Label("Delete", systemImage: "trash")
                        }
                    }
                }
            }
        }
        Section {
            ForEach(overview.experiments) { experiment in
                Button {
                    present(.form(MarketingDraft(kind: .experiment, id: experiment.id, overview: overview, json: experiment.json,
                                                 zone: model.timeZone)))
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(MarketingText.title(experiment.status)).font(.caption2.weight(.semibold)).foregroundStyle(.secondary)
                        MarketingRecordRow(record: experiment, overview: overview, working: model.isWorking(experiment.id))
                    }
                }
                .buttonStyle(.plain)
                .contextMenu {
                    MarketingStatusMenu(model: model, kind: .experiment, id: experiment.id, current: experiment.status)
                    Button("Delete", systemImage: "trash", role: .destructive) {
                        Task { _ = await model.delete(.experiment, id: experiment.id) }
                    }
                }
            }
            if overview.experiments.isEmpty { Text("No experiments yet.").foregroundStyle(.secondary) }
        } header: {
            Text("Experiments")
        } footer: {
            Text("\(overview.experimentsRunning) running of \(overview.experimentsRunningLimit) at a time. Score ideas by impact, confidence and ease.")
        }
        Section {
            ForEach(overview.keywords) { keyword in
                Button {
                    present(.form(MarketingDraft(kind: .keyword, id: keyword.id, overview: overview, json: keyword.json, zone: model.timeZone)))
                } label: {
                    MarketingRecordRow(record: keyword, overview: overview, working: model.isWorking(keyword.id))
                }
                .buttonStyle(.plain)
                .swipeActions(edge: .trailing) {
                    Button(role: .destructive) {
                        Task { _ = await model.delete(.keyword, id: keyword.id) }
                    } label: {
                        Label("Delete", systemImage: "trash")
                    }
                }
            }
            if let draft = model.newDraft(.keyword) {
                Button("Add a keyword", systemImage: "plus.circle") { present(.form(draft)) }
            }
            Button("Import from Search Console", systemImage: "square.and.arrow.down") { present(.importKeywords) }
        } header: {
            Text("Keywords")
        }
        Section {
            Button("Build a tagged link", systemImage: "link") { present(.link) }
        }
    }
}

struct MarketingRedditSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void

    var body: some View {
        Section {
            LabeledContent("To answer", value: "\(overview.redditSummary.int("open"))")
            LabeledContent("Replied this week", value: "\(overview.redditSummary.int("repliedThisWeek"))")
            LabeledContent("Skipped this week", value: "\(overview.redditSummary.int("skippedThisWeek"))")
            LabeledContent("Monitored", value: "\(overview.redditSummary.int("monitored"))")
        } header: {
            Text("Reddit desk")
        } footer: {
            Text("Read each subreddit's rules, disclose who you work for, and link with the tagged landing link.")
        }
        Section {
            if overview.redditAccounts.isEmpty {
                Text("No Reddit accounts yet.").foregroundStyle(.secondary)
            }
            ForEach(overview.redditAccounts) { account in
                if let url = URL(string: account.url) {
                    Link("u/" + account.name, destination: url)
                } else {
                    Text("u/" + account.name)
                }
            }
            if overview.canManageSettings, let draft = model.newDraft(.redditWatch) {
                Button("Edit accounts", systemImage: "person.crop.circle") { present(.form(draft)) }
            }
        } header: {
            Text("Your accounts")
        }
        Section {
            ForEach(overview.redditSubreddits) { subreddit in
                HStack {
                    if let url = URL(string: subreddit.url) {
                        Link("r/" + subreddit.name, destination: url)
                    } else {
                        Text("r/" + subreddit.name)
                    }
                    Spacer()
                    if let rules = URL(string: subreddit.rulesUrl) {
                        Link(subreddit.rulesRead ? "Rules on file" : "Read rules", destination: rules)
                            .font(.caption)
                            .foregroundStyle(subreddit.rulesRead ? Color.secondary : Color.orange)
                    }
                }
            }
            ForEach(overview.redditQueries) { item in
                if let url = URL(string: item.url) {
                    Link(destination: url) { Label(item.name, systemImage: "magnifyingglass") }
                }
            }
            if overview.canManageSettings, let draft = model.newDraft(.redditWatch) {
                Button("Edit the watch list", systemImage: "pencil") { present(.form(draft)) }
            }
        } header: {
            Text("Watch")
        }
        ForEach(MarketingKind.redditThread.statuses, id: \.self) { status in
            let threads = overview.redditThreads.filter { $0.status == status }
            if !threads.isEmpty {
                Section(MarketingText.title(status)) {
                    ForEach(threads) { thread in
                        VStack(alignment: .leading, spacing: 6) {
                            Button {
                                present(.form(MarketingDraft(kind: .redditThread, id: thread.id, overview: overview, json: thread.json,
                                                             zone: model.timeZone)))
                            } label: {
                                VStack(alignment: .leading, spacing: 3) {
                                    MarketingRecordRow(record: thread, overview: overview, working: model.isWorking(thread.id))
                                    Text(thread.redditActivity()).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                                }
                            }
                            .buttonStyle(.plain)
                            Button("Copy ChatGPT reply prompt", systemImage: "doc.on.doc") {
                                UIPasteboard.general.string = thread.redditReplyPrompt
                                model.notice = "ChatGPT reply prompt copied."
                            }
                            .font(.caption)
                            .buttonStyle(.borderless)
                        }
                        .contextMenu {
                            MarketingStatusMenu(model: model, kind: .redditThread, id: thread.id, current: thread.status)
                            if let url = URL(string: thread.json.str("url")) {
                                Link(destination: url) { Label("Open thread", systemImage: "safari") }
                            }
                            if !thread.json.str("taggedLink").isEmpty {
                                Button("Copy tagged link", systemImage: "doc.on.doc") {
                                    UIPasteboard.general.string = thread.json.str("taggedLink")
                                    model.notice = "Tagged link copied."
                                }
                            }
                            Button("Delete", systemImage: "trash", role: .destructive) {
                                Task { _ = await model.delete(.redditThread, id: thread.id) }
                            }
                        }
                    }
                }
            }
        }
        if overview.redditThreads.isEmpty {
            ContentUnavailableView("No threads logged", systemImage: "bubble.left.and.bubble.right",
                                   description: Text("Log a thread worth answering from the watch list."))
        }
    }
}

struct MarketingPerformanceSection: View {
    let model: MarketingModel
    let overview: MarketingOverview
    let present: (MarketingSheet) -> Void

    var body: some View {
        Section {
            Picker("Window", selection: Bindable(model).performanceDays) {
                Text("7 days").tag(7)
                Text("28 days").tag(28)
                Text("90 days").tag(90)
            }
            .pickerStyle(.segmented)
            .onChange(of: model.performanceDays) { _, _ in Task { await model.loadPerformance() } }
            if let projects = model.performance?.projects, projects.count > 1 {
                Picker("Analytics project", selection: Bindable(model).performanceProject) {
                    ForEach(projects) { Text($0.name).tag($0.id) }
                }
                .onChange(of: model.performanceProject) { _, _ in Task { await model.loadPerformance() } }
            }
            if overview.canManageSettings, let draft = model.newDraft(.goals) {
                Button("Conversion goals", systemImage: "flag.checkered") { present(.form(draft)) }
            }
        }
        .task { if model.performance == nil { await model.loadPerformance() } }
        if model.loadingPerformance && model.performance == nil {
            Section { ProgressView() }
        }
        if let performance = model.performance {
            if performance.ok {
                Section("Visits from campaigns") {
                    LabeledContent("Visits", value: PackagesFormat.count(performance.sessions))
                    LabeledContent("Change", value: MarketingText.signed(performance.sessionsChange))
                    if let conversions = performance.conversions {
                        LabeledContent("Conversions", value: PackagesFormat.count(conversions))
                    }
                }
            }
            if !performance.insights.isEmpty {
                Section("Insights") {
                    ForEach(performance.insights) { insight in
                        MarketingInsightRow(level: insight.level, title: insight.title, detail: insight.detail)
                    }
                }
            }
            rows("Campaigns", performance.campaigns)
            rows("Channels", performance.channels)
            rows("Landing pages", performance.landing)
        }
    }

    @ViewBuilder private func rows(_ title: String, _ rows: [MarketingPerformanceRow]) -> some View {
        if !rows.isEmpty {
            Section(title) {
                ForEach(rows.prefix(25)) { row in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(row.label).font(.subheadline).lineLimit(2)
                        Text(row.facts).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }
}
