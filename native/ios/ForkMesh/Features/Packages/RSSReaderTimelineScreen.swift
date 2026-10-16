import FMPackages
import SwiftUI

struct RSSReaderTimelineScreen: View {
    let model: RSSReaderModel
    let scope: RSSScope
    @State private var query = ""
    @State private var unreadOnly: Bool
    @State private var shown: Set<String> = []
    @State private var started = false
    @State private var confirmMarkAll = false

    init(model: RSSReaderModel, scope: RSSScope) {
        self.model = model
        self.scope = scope
        _unreadOnly = State(initialValue: scope.unreadOnly)
    }

    private var articles: [RSSArticle] {
        model.timeline(scope, query: query, unreadOnly: unreadOnly, keep: shown)
    }

    private var grouped: Bool {
        if case .feed = scope { return false }
        return model.settings.groupByFeed
    }

    var body: some View {
        content
            .bareShellTop()
            .navigationTitle(model.title(scope))
            .navigationBarTitleDisplayMode(.inline)
            .searchable(text: $query, placement: .navigationBarDrawer(displayMode: .automatic), prompt: "Search Articles")
            .toolbar {
                ToolbarItemGroup(placement: .topBarTrailing) {
                    if !scope.unreadOnly {
                        Button {
                            unreadOnly.toggle()
                            snapshot(reset: true)
                        } label: {
                            Image(systemName: unreadOnly ? "line.3.horizontal.decrease.circle.fill" : "line.3.horizontal.decrease.circle")
                        }
                        .accessibilityLabel(unreadOnly ? "Show read articles" : "Show only unread articles")
                    }
                    Button {
                        askMarkAll()
                    } label: {
                        Image(systemName: "checkmark.circle")
                    }
                    .disabled(articles.allSatisfy(\.read))
                    .accessibilityLabel("Mark All as Read")
                    .accessibilityIdentifier("rss-mark-all")
                }
            }
            .refreshable {
                await model.refresh(force: true)
                snapshot(reset: model.settings.refreshClearsRead)
            }
            .onAppear {
                if !started {
                    started = true
                    snapshot(reset: true)
                }
            }
            .alert("Mark All as Read?", isPresented: $confirmMarkAll) {
                Button("Cancel", role: .cancel) {}
                Button("Mark All as Read") { markAll() }
            } message: {
                Text(RSSReaderText.markAllPrompt(articles.filter { !$0.read }.count, scope: model.title(scope)))
            }
            .rssPalette(model.settings.palette)
    }

    private func snapshot(reset: Bool) {
        let current: [String] = model.timeline(scope, query: "", unreadOnly: unreadOnly).map(\.id)
        if reset {
            shown = Set(current)
        } else {
            shown.formUnion(current)
        }
    }

    private func askMarkAll() {
        if model.settings.confirmMarkAllRead {
            confirmMarkAll = true
        } else {
            markAll()
        }
    }

    private func markAll() {
        if query.isEmpty {
            model.markAllRead(scope)
        } else {
            model.markRead(articles)
        }
    }

    @ViewBuilder private var content: some View {
        let list: [RSSArticle] = articles
        if list.isEmpty {
            ScrollView {
                VStack(spacing: 10) {
                    Image(systemName: query.isEmpty ? "tray" : "magnifyingglass")
                        .font(.system(size: 34))
                        .foregroundStyle(.secondary)
                    Text(query.isEmpty ? (unreadOnly ? "No Unread Articles" : "No Articles") : "No Results")
                        .font(.headline)
                    if query.isEmpty && unreadOnly {
                        Text(RSSReaderText.caughtUp)
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                }
                .frame(maxWidth: .infinity)
                .padding(.top, 120)
            }
        } else {
            List {
                if grouped {
                    ForEach(groups(list)) { (group: RSSTimelineGroup) in
                        Section(model.feed(group.feedId)?.displayTitle ?? "Feed") {
                            ForEach(group.articles) { (article: RSSArticle) in
                                row(article, list: list)
                            }
                        }
                    }
                } else {
                    ForEach(list) { (article: RSSArticle) in
                        row(article, list: list)
                    }
                }
            }
            .listStyle(.plain)
        }
    }

    private func groups(_ list: [RSSArticle]) -> [RSSTimelineGroup] {
        var result: [RSSTimelineGroup] = []
        for article in list {
            if let last = result.last, last.feedId == article.feedId {
                result[result.count - 1].articles.append(article)
            } else {
                result.append(RSSTimelineGroup(feedId: article.feedId, articles: [article]))
            }
        }
        return result
    }

    private func row(_ article: RSSArticle, list: [RSSArticle]) -> some View {
        NavigationLink {
            RSSReaderArticleScreen(model: model, scope: scope, list: list, articleId: article.id)
        } label: {
            RSSTimelineRow(
                article: article, feed: model.feed(article.feedId), settings: model.settings, now: model.store.now)
        }
        .swipeActions(edge: .leading, allowsFullSwipe: true) {
            Button {
                model.toggleRead(article.id)
            } label: {
                Label(article.read ? "Unread" : "Read", systemImage: article.read ? "circle.fill" : "circle")
            }
            .tint(.blue)
        }
        .swipeActions(edge: .trailing, allowsFullSwipe: true) {
            Button {
                model.toggleStar(article.id)
            } label: {
                Label(article.starred ? "Unstar" : "Star", systemImage: article.starred ? "star.slash" : "star.fill")
            }
            .tint(.yellow)
        }
        .contextMenu {
            Button {
                model.toggleRead(article.id)
            } label: {
                Label(article.read ? "Mark as Unread" : "Mark as Read", systemImage: article.read ? "circle.fill" : "circle")
            }
            Button {
                model.toggleStar(article.id)
            } label: {
                Label(article.starred ? "Mark as Unstarred" : "Mark as Starred", systemImage: article.starred ? "star.slash" : "star")
            }
            if let url = URL(string: RSSURL.http(article.link)), !article.link.isEmpty {
                ShareLink(item: url) {
                    Label("Share", systemImage: "square.and.arrow.up")
                }
            }
        }
    }
}

struct RSSTimelineGroup: Identifiable {
    var feedId: String
    var articles: [RSSArticle]

    var id: String { feedId }
}

struct RSSTimelineRow: View {
    let article: RSSArticle
    let feed: RSSFeed?
    let settings: RSSReaderSettings
    let now: Int64

    private var feedName: String { feed?.displayTitle ?? "Feed" }

    private var byline: String {
        article.author.isEmpty || article.author == feedName ? feedName : "\(feedName) \u{00B7} \(article.author)"
    }

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            VStack(spacing: 6) {
                Circle()
                    .fill(article.read ? Color.clear : Color.accentColor)
                    .frame(width: 9, height: 9)
                if article.starred {
                    Image(systemName: "star.fill")
                        .font(.system(size: 9))
                        .foregroundStyle(.yellow)
                }
            }
            .frame(width: 12)
            .padding(.top, 6)
            if settings.showFeedIcons {
                RSSFeedIcon(feed: feed, title: feedName, size: 28, remote: settings.loadRemoteImages)
                    .padding(.top, 2)
            }
            VStack(alignment: .leading, spacing: 3) {
                Text(article.title.isEmpty ? "Untitled" : article.title)
                    .font(.headline)
                    .foregroundStyle(article.read ? Color.secondary : Color.primary)
                    .lineLimit(3)
                if settings.timelineLines > 0 && !article.summary.isEmpty {
                    Text(article.summary)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .lineLimit(settings.timelineLines)
                }
                HStack(alignment: .firstTextBaseline) {
                    Text(byline)
                        .lineLimit(1)
                    Spacer(minLength: 8)
                    Text(RSSReaderText.timelineDate(article.published, now: now))
                        .monospacedDigit()
                }
                .font(.caption)
                .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 4)
        .accessibilityElement(children: .combine)
        .accessibilityValue(Text([article.read ? "" : "Unread", article.starred ? "Starred" : ""].filter { !$0.isEmpty }.joined(separator: ", ")))
    }
}
