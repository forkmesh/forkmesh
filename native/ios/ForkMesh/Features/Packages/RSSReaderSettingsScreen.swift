import FMPackages
import SwiftUI
import UniformTypeIdentifiers

struct RSSReaderSettingsSheet: View {
    let model: RSSReaderModel
    @Environment(\.dismiss) private var dismiss
    @State private var importing = false
    @State private var exportFile: URL?
    @State private var message = ""
    @State private var addingBlog = false
    @State private var importURLPrompt = false
    @State private var importAddress = ""
    @State private var importingURL = false

    private func setting<Value>(_ path: WritableKeyPath<RSSReaderSettings, Value>) -> Binding<Value> {
        Binding(get: { model.settings[keyPath: path] }, set: { (value: Value) in model.updateSettings { $0[keyPath: path] = value } })
    }

    private var opmlTypes: [UTType] {
        var types: [UTType] = []
        if let opml = UTType(filenameExtension: "opml") { types.append(opml) }
        types.append(contentsOf: [.xml, .plainText, .data])
        return types
    }

    var body: some View {
        NavigationStack {
            Form {
                feedsSection
                timelineSection
                articlesSection
                Section("Appearance") {
                    Picker("Color Palette", selection: setting(\.palette)) {
                        ForEach(RSSColorPalette.allCases, id: \.self) { (palette: RSSColorPalette) in
                            Text(palette.title).tag(palette)
                        }
                    }
                }
                Section("Troubleshooting") {
                    NavigationLink("Error Log") { RSSErrorLogScreen(model: model) }
                    NavigationLink("Activity Log") { RSSActivityLogScreen(model: model) }
                    NavigationLink("Stats") { RSSStatsScreen(model: model) }
                }
                Section("Help") {
                    NavigationLink("About RSS Reader") { RSSAboutScreen() }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button {
                        model.saveNow()
                        dismiss()
                    } label: {
                        Image(systemName: "checkmark")
                    }
                    .accessibilityLabel("Done")
                }
            }
            .fileImporter(isPresented: $importing, allowedContentTypes: opmlTypes) { (result: Result<URL, Error>) in
                guard case .success(let url) = result else { return }
                let scoped: Bool = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                guard let data = try? Data(contentsOf: url), data.count <= 4 * 1024 * 1024 else {
                    message = RSSReaderError.unreadableOPML.message
                    return
                }
                message = model.importOPML(data)
                exportFile = writeExport()
                Task { await model.refresh() }
            }
            .alert("Import from URL", isPresented: $importURLPrompt) {
                TextField("OPML address", text: $importAddress)
                    .keyboardType(.URL)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                Button("Cancel", role: .cancel) {}
                Button("Import") { importFromURL() }
            } message: {
                Text("The address of an OPML subscription list.")
            }
            .alert("RSS Reader", isPresented: Binding(get: { !message.isEmpty }, set: { if !$0 { message = "" } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(message)
            }
            .task { exportFile = writeExport() }
        }
    }

    private var feedsSection: some View {
        Section("Feeds") {
            Button {
                importing = true
            } label: {
                Label("Import Subscriptions", systemImage: "square.and.arrow.down")
            }
            Button {
                importAddress = ""
                importURLPrompt = true
            } label: {
                HStack {
                    Label("Import from URL\u{2026}", systemImage: "link")
                    if importingURL {
                        Spacer()
                        ProgressView()
                    }
                }
            }
            .disabled(importingURL)
            if let exportFile {
                ShareLink(item: exportFile) {
                    Label("Export Subscriptions", systemImage: "square.and.arrow.up")
                }
                .disabled(model.feeds.isEmpty)
            }
            Button {
                addBlog()
            } label: {
                HStack {
                    Label("Add the ForkMesh Blog Feed", systemImage: "dot.radiowaves.up.forward")
                    if addingBlog {
                        Spacer()
                        ProgressView()
                    }
                }
            }
            .disabled(addingBlog || model.store.feed(matching: RSSReaderLimits.forkMeshBlogFeed) != nil)
            Toggle("Sort by Unread Count", isOn: setting(\.sortFeedsByUnread))
        }
    }

    private var timelineSection: some View {
        Section("Timeline") {
            Toggle("Sort Oldest to Newest", isOn: setting(\.sortOldestFirst))
            Toggle("Group by Feed", isOn: setting(\.groupByFeed))
            Toggle("Hide Read Articles", isOn: setting(\.hideReadArticles))
            Toggle("Refresh to Clear Read Articles", isOn: setting(\.refreshClearsRead))
            Toggle("Confirm Mark All as Read", isOn: setting(\.confirmMarkAllRead))
            Toggle("Mark All as Read Skips Starred", isOn: setting(\.markAllKeepsStarred))
            NavigationLink("Timeline Layout") { RSSTimelineLayoutScreen(model: model) }
            NavigationLink("Filters") { RSSFiltersScreen(model: model) }
        }
    }

    private var articlesSection: some View {
        Section("Articles") {
            Picker("Theme", selection: setting(\.articleTheme)) {
                ForEach(RSSArticleTheme.allCases, id: \.self) { (theme: RSSArticleTheme) in
                    Text(theme.title).tag(theme)
                }
            }
            Picker("Font", selection: setting(\.articleFont)) {
                ForEach(RSSArticleFont.allCases, id: \.self) { (font: RSSArticleFont) in
                    Text(font.title).tag(font)
                }
            }
            Stepper(value: setting(\.articleSize), in: RSSReaderSettings.textSizes) {
                LabeledContent("Text Size", value: "\(model.settings.articleSize) pt")
            }
            Picker("Open Links In", selection: setting(\.openLinksInApp)) {
                Text("RSS Reader").tag(true)
                Text("Safari").tag(false)
            }
            Toggle("Load Remote Images", isOn: setting(\.loadRemoteImages))
        }
    }

    private func importFromURL() {
        let address: String = importAddress
        guard !address.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return }
        importingURL = true
        Task {
            let result: String = await model.importOPML(fromURL: address)
            importingURL = false
            message = result
            exportFile = writeExport()
            await model.refresh()
        }
    }

    private func addBlog() {
        addingBlog = true
        Task {
            do {
                let feed: RSSFeed = try await model.addForkMeshBlog()
                message = "Subscribed to \(feed.displayTitle)."
            } catch {
                message = RSSReaderErrors.message(error)
            }
            addingBlog = false
        }
    }

    private func writeExport() -> URL? {
        let folder: URL = FileManager.default.temporaryDirectory.appendingPathComponent("rss-reader-export", isDirectory: true)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let file: URL = folder.appendingPathComponent("Subscriptions.opml")
        do {
            try Data(model.exportOPML().utf8).write(to: file, options: .atomic)
            return file
        } catch {
            return nil
        }
    }
}

struct RSSTimelineLayoutScreen: View {
    let model: RSSReaderModel

    private var sample: RSSArticle {
        var article = RSSArticle(feedId: "preview", guid: "preview")
        article.title = "Open Source Weekly: what shipped this week"
        article.summary = "A short look at the releases, fixes and new projects from the community, with links to each announcement and the people behind them."
        article.author = "The Editors"
        article.published = model.store.now - 3_600_000
        return article
    }

    var body: some View {
        Form {
            Section {
                RSSTimelineRow(
                    article: sample,
                    feed: RSSFeed(id: "preview", url: "https://weekly.example.net/rss", title: "Open Source Weekly"),
                    settings: model.settings, now: model.store.now)
                    .allowsHitTesting(false)
            } header: {
                Text("Preview")
            }
            Section {
                Stepper(
                    value: Binding(
                        get: { model.settings.timelineLines },
                        set: { (value: Int) in model.updateSettings { $0.timelineLines = value } }),
                    in: 0...4
                ) {
                    LabeledContent("Summary Lines", value: "\(model.settings.timelineLines)")
                }
                Toggle(
                    "Show Feed Icons",
                    isOn: Binding(
                        get: { model.settings.showFeedIcons },
                        set: { (value: Bool) in model.updateSettings { $0.showFeedIcons = value } }))
                Toggle(
                    "Show Thumbnails",
                    isOn: Binding(
                        get: { model.settings.showThumbnails },
                        set: { (value: Bool) in model.updateSettings { $0.showThumbnails = value } }))
            }
        }
        .navigationTitle("Timeline Layout")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct RSSErrorLogScreen: View {
    let model: RSSReaderModel

    var body: some View {
        List {
            let feeds: [RSSFeed] = model.failingFeeds
            if feeds.isEmpty {
                Text("No feed failed its last update.")
                    .foregroundStyle(.secondary)
            }
            ForEach(feeds) { (feed: RSSFeed) in
                VStack(alignment: .leading, spacing: 4) {
                    Text(feed.displayTitle).font(.headline)
                    Text(RSSReaderText.feedError(feed.error))
                        .font(.subheadline)
                        .foregroundStyle(.red)
                    Text("\(RSSReaderText.stamp(feed.errorAt)) \u{00B7} \(RSSReaderText.plural(feed.errorCount, "failure")) in a row")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(feed.url)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .textSelection(.enabled)
                }
                .padding(.vertical, 2)
            }
        }
        .navigationTitle("Error Log")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct RSSActivityLogScreen: View {
    let model: RSSReaderModel

    var body: some View {
        List {
            let entries: [RSSActivity] = model.activity
            if entries.isEmpty {
                Text("Nothing has happened yet.")
                    .foregroundStyle(.secondary)
            }
            ForEach(entries) { (entry: RSSActivity) in
                VStack(alignment: .leading, spacing: 2) {
                    Text(entry.text)
                    Text(RSSReaderText.stamp(entry.at))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .navigationTitle("Activity Log")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct RSSStatsScreen: View {
    let model: RSSReaderModel

    var body: some View {
        let stats: RSSStats = model.stats
        List {
            Section {
                LabeledContent("Feeds", value: "\(stats.feeds.formatted()) of \(RSSReaderLimits.maxFeeds)")
                LabeledContent("Folders", value: "\(stats.folders.formatted()) of \(RSSReaderLimits.maxFolders)")
                LabeledContent("Articles", value: stats.articles.formatted())
                LabeledContent("Unread", value: stats.unread.formatted())
                LabeledContent("Starred", value: stats.starred.formatted())
                LabeledContent("Feeds with Errors", value: stats.failingFeeds.formatted())
            }
            Section {
                LabeledContent("Oldest Article", value: RSSReaderText.stamp(stats.oldestArticle))
                LabeledContent("Last Refresh", value: RSSReaderText.stamp(stats.lastRefresh))
            } footer: {
                Text("Everything is stored on this iPhone. Each feed keeps its newest \(RSSReaderLimits.maxArticlesPerFeed) articles plus every starred one.")
            }
        }
        .navigationTitle("Stats")
        .navigationBarTitleDisplayMode(.inline)
    }
}

struct RSSAboutScreen: View {
    var body: some View {
        List {
            Section {
                VStack(spacing: 10) {
                    Image(systemName: "dot.radiowaves.up.forward")
                        .font(.system(size: 40, weight: .semibold))
                        .foregroundStyle(Color.accentColor)
                    Text(RSSReaderText.title).font(.title2.bold())
                    Text(RSSReaderText.about)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 8)
            }
            Section("Credits") {
                Text(RSSReaderText.credit)
                    .font(.subheadline)
                if let site = URL(string: RSSReaderText.netNewsWireURL) {
                    Link(destination: site) {
                        Label("NetNewsWire website", systemImage: "safari")
                    }
                }
                if let source = URL(string: RSSReaderText.netNewsWireSourceURL) {
                    Link(destination: source) {
                        Label("NetNewsWire source code", systemImage: "chevron.left.forwardslash.chevron.right")
                    }
                }
            }
        }
        .navigationTitle("About RSS Reader")
        .navigationBarTitleDisplayMode(.inline)
    }
}
