import FMAccount
import FMBase
import FMNet
import FMPackages
import SwiftUI

@MainActor
enum RSSReaderFeature {
    private static var cached: (path: String, model: RSSReaderModel)?

    static func directory() -> URL {
        let manager: FileManager = FileManager.default
        if let support = manager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first,
           let folder = RSSReaderModel.defaultDirectory(base: AppProjectScope.router.scope.directory(support)) {
            return folder
        }
        return manager.temporaryDirectory.appendingPathComponent("rss-reader", isDirectory: true)
    }

    static var loaded: RSSReaderModel? {
        guard let cached, cached.path == directory().path else { return nil }
        return cached.model
    }

    static func model() -> RSSReaderModel {
        let folder: URL = directory()
        if let cached, cached.path == folder.path { return cached.model }
        cached?.model.saveNow()
        let store = RSSReaderStore(disk: RSSReaderFileDisk(directory: folder))
        let model = RSSReaderModel(store: store, fetcher: RSSReaderFetcher(http: URLSessionHTTPClient()))
        cached = (folder.path, model)
        return model
    }
}

extension View {
    @ViewBuilder func rssGlass<S: Shape>(_ shape: S) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            self.contentShape(shape).glassEffect(.regular.interactive(), in: shape)
        } else {
            self.contentShape(shape).background(shape.fill(.regularMaterial)).overlay(shape.stroke(Color.primary.opacity(0.08), lineWidth: 0.5))
                .shadow(color: Color.black.opacity(0.12), radius: 8, y: 2)
        }
        #else
        self.contentShape(shape).background(shape.fill(.regularMaterial)).overlay(shape.stroke(Color.primary.opacity(0.08), lineWidth: 0.5))
            .shadow(color: Color.black.opacity(0.12), radius: 8, y: 2)
        #endif
    }

    @ViewBuilder func rssPalette(_ palette: RSSColorPalette) -> some View {
        switch palette {
        case .automatic: self
        case .light: self.environment(\.colorScheme, .light)
        case .dark: self.environment(\.colorScheme, .dark)
        }
    }
}

@MainActor
@Observable
final class RSSFeedIconImages {
    static let shared = RSSFeedIconImages()
    static let maxConcurrent = 4
    nonisolated static let pixels: CGFloat = 144

    private(set) var images: [String: UIImage] = [:]
    @ObservationIgnored private var busy: Set<String> = []
    @ObservationIgnored private var active = 0
    @ObservationIgnored private var waiting: [CheckedContinuation<Void, Never>] = []

    nonisolated static func key(_ feed: RSSFeed) -> String { "\(feed.id)|\(feed.iconFound)" }

    func image(_ feed: RSSFeed) -> UIImage? { images[Self.key(feed)] }

    nonisolated static func decode(_ data: Data) -> UIImage? {
        guard let image = UIImage(data: data), image.size.width > 0, image.size.height > 0 else { return nil }
        let side: CGFloat = max(image.size.width, image.size.height) * image.scale
        if side <= pixels { return image }
        let scale: CGFloat = pixels / side
        let size = CGSize(width: image.size.width * image.scale * scale, height: image.size.height * image.scale * scale)
        return image.preparingThumbnail(of: size) ?? image
    }

    func load(_ feed: RSSFeed) {
        guard let model = RSSReaderFeature.loaded, !busy.contains(feed.id) else { return }
        let key: String = Self.key(feed)
        if images[key] == nil, let data = model.iconData(feed.id), let image = Self.decode(data) { images[key] = image }
        guard model.needsIcon(feed.id, hasImage: images[key] != nil) else { return }
        busy.insert(feed.id)
        let id: String = feed.id
        Task { [weak self] in
            guard let self else { return }
            await self.acquire()
            let data: Data? = await model.resolveIcon(id, decodes: { (bytes: Data) in RSSFeedIconImages.decode(bytes) != nil })
            self.release()
            self.busy.remove(id)
            guard let data, let current = model.feed(id), let image = Self.decode(data) else { return }
            self.images[Self.key(current)] = image
        }
    }

    private func acquire() async {
        if active < Self.maxConcurrent {
            active += 1
            return
        }
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in waiting.append(continuation) }
    }

    private func release() {
        if waiting.isEmpty {
            active -= 1
        } else {
            waiting.removeFirst().resume()
        }
    }
}

struct RSSFeedIcon: View {
    let feed: RSSFeed?
    let title: String
    var size: CGFloat = 22
    var remote: Bool = true
    private var icons: RSSFeedIconImages { RSSFeedIconImages.shared }

    var body: some View {
        Group {
            if remote, let feed, let image = icons.image(feed) {
                Image(uiImage: image).resizable().interpolation(.high).scaledToFill()
            } else {
                letter
            }
        }
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.22, style: .continuous))
        .accessibilityHidden(true)
        .task(id: remote ? feed.map { (item: RSSFeed) in RSSFeedIconImages.key(item) } : nil) {
            if remote, let feed { icons.load(feed) }
        }
    }

    private var letter: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.22, style: .continuous).fill(Color.accentColor.opacity(0.18))
            Text(RSSText.letter(title))
                .font(.system(size: size * 0.55, weight: .semibold, design: .rounded))
                .foregroundStyle(Color.accentColor)
        }
    }
}

struct RSSCountLabel: View {
    let count: Int

    var body: some View {
        if count > 0 {
            Text(count.formatted())
                .font(.subheadline)
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
    }
}

struct RSSReaderScreen: View {
    @State private var model: RSSReaderModel = RSSReaderFeature.model()
    @Environment(\.scenePhase) private var scenePhase
    @State private var showSettings = false
    @State private var showAddFeed = false
    @State private var folderPrompt: RSSFolderPrompt?
    @State private var folderName = ""
    @State private var infoFeed: RSSFeed?
    @State private var unsubscribeTarget: RSSFeed?
    @State private var deleteFolderTarget: RSSFolder?
    @State private var markTarget: RSSScope?
    @State private var olderTarget: RSSOlderRequest?
    @State private var smartEditor: RSSSmartFeedDraft?
    @State private var deleteSmartTarget: RSSSmartFeed?
    @State private var feedQuery = ""
    @State private var problem = ""

    var body: some View {
        list
            .bareShellTop()
            .navigationTitle(RSSReaderText.feedsTitle)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .principal) {
                    VStack(spacing: 0) {
                        Text(RSSReaderText.feedsTitle).font(.headline)
                        TimelineView(.everyMinute) { (_: TimelineViewDefaultContext) in
                            Text(model.status())
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                    }
                    .accessibilityElement(children: .combine)
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Menu {
                            ForEach(RSSCatchUp.allCases) { (choice: RSSCatchUp) in
                                Button(choice.title) { askOlder(nil, choice) }
                            }
                        } label: {
                            Label("Mark Older as Read\u{2026}", systemImage: "clock.arrow.circlepath")
                        }
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("More")
                    .accessibilityIdentifier("rss-feeds-more")
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Button {
                        model.updateSettings { $0.feedsUnreadOnly.toggle() }
                    } label: {
                        Image(systemName: model.settings.feedsUnreadOnly
                            ? "line.3.horizontal.decrease.circle.fill" : "line.3.horizontal.decrease.circle")
                    }
                    .accessibilityLabel(model.settings.feedsUnreadOnly ? "Show all feeds" : "Show only feeds with unread articles")
                    .accessibilityIdentifier("rss-feeds-filter")
                }
            }
            .searchable(text: $feedQuery, placement: .navigationBarDrawer(displayMode: .automatic), prompt: "Find a Feed")
            .safeAreaInset(edge: .bottom, spacing: 0) { bottomBar }
            .refreshable { await model.refresh(force: true) }
            .task { await model.refresh() }
            .onChange(of: scenePhase) { (_: ScenePhase, phase: ScenePhase) in
                if phase != .active { model.saveNow() }
            }
            .onDisappear { model.saveNow() }
            .sheet(isPresented: $showSettings) {
                RSSReaderSettingsSheet(model: model)
                    .rssPalette(model.settings.palette)
            }
            .sheet(isPresented: $showAddFeed) {
                RSSAddFeedSheet(model: model)
                    .rssPalette(model.settings.palette)
            }
            .sheet(item: $infoFeed) { (feed: RSSFeed) in
                RSSFeedInfoSheet(model: model, feedId: feed.id)
                    .rssPalette(model.settings.palette)
            }
            .sheet(item: $smartEditor) { (draft: RSSSmartFeedDraft) in
                RSSSmartFeedEditor(model: model, draft: draft.smart)
                    .rssPalette(model.settings.palette)
            }
            .confirmationDialog(
                "Delete \u{201C}\(deleteSmartTarget?.name ?? "")\u{201D}?", isPresented: deleteSmartShown, titleVisibility: .visible
            ) {
                Button("Delete Smart Feed", role: .destructive) {
                    if let smart = deleteSmartTarget { model.deleteSmartFeed(smart.id) }
                }
            }
            .alert("Mark Older as Read?", isPresented: olderShown) {
                Button("Cancel", role: .cancel) {}
                Button("Mark as Read") {
                    if let request = olderTarget { model.markOlderRead(request.scope, request.choice) }
                }
            } message: {
                Text(olderTarget.map { (request: RSSOlderRequest) in
                    RSSReaderText.markOlderPrompt(
                        model.olderUnreadCount(request.scope, request.choice), request.choice,
                        scope: request.scope.map { (scope: RSSScope) in model.title(scope) })
                } ?? "")
            }
            .alert(folderPrompt?.title ?? "", isPresented: folderPromptShown) {
                TextField("Name", text: $folderName)
                Button("Cancel", role: .cancel) {}
                Button(folderPrompt?.action ?? "Save") { saveFolder() }
            }
            .confirmationDialog(
                "Unsubscribe from \u{201C}\(unsubscribeTarget?.displayTitle ?? "")\u{201D}?", isPresented: unsubscribeShown,
                titleVisibility: .visible
            ) {
                Button("Unsubscribe", role: .destructive) {
                    if let feed = unsubscribeTarget { model.unsubscribe(feed.id) }
                }
            } message: {
                Text("Its articles, including starred ones, are removed from this iPhone.")
            }
            .confirmationDialog(
                "Delete \u{201C}\(deleteFolderTarget?.name ?? "")\u{201D}?", isPresented: deleteFolderShown, titleVisibility: .visible
            ) {
                Button("Delete Folder", role: .destructive) {
                    if let folder = deleteFolderTarget { model.deleteFolder(folder.id) }
                }
            } message: {
                Text("The feeds in it move to the top level.")
            }
            .alert("Mark All as Read?", isPresented: markShown) {
                Button("Cancel", role: .cancel) {}
                Button("Mark All as Read") {
                    if let scope = markTarget { model.markAllRead(scope) }
                }
            } message: {
                Text(markTarget.map { RSSReaderText.markAllPrompt(model.unread($0), scope: model.title($0)) } ?? "")
            }
            .alert("RSS Reader", isPresented: problemShown) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(problem)
            }
    }

    private var folderPromptShown: Binding<Bool> {
        Binding(get: { folderPrompt != nil }, set: { if !$0 { folderPrompt = nil } })
    }

    private var unsubscribeShown: Binding<Bool> {
        Binding(get: { unsubscribeTarget != nil }, set: { if !$0 { unsubscribeTarget = nil } })
    }

    private var deleteFolderShown: Binding<Bool> {
        Binding(get: { deleteFolderTarget != nil }, set: { if !$0 { deleteFolderTarget = nil } })
    }

    private var markShown: Binding<Bool> {
        Binding(get: { markTarget != nil }, set: { if !$0 { markTarget = nil } })
    }

    private var olderShown: Binding<Bool> {
        Binding(get: { olderTarget != nil }, set: { if !$0 { olderTarget = nil } })
    }

    private var deleteSmartShown: Binding<Bool> {
        Binding(get: { deleteSmartTarget != nil }, set: { if !$0 { deleteSmartTarget = nil } })
    }

    private var problemShown: Binding<Bool> {
        Binding(get: { !problem.isEmpty }, set: { if !$0 { problem = "" } })
    }

    private var list: some View {
        List {
            if feedQuery.isEmpty {
                Section {
                    if !model.isCollapsed("smart") {
                        smartRow(.today, symbol: "sun.max.fill", tint: .orange)
                        smartRow(.unread, symbol: "circle.fill", tint: .blue)
                        smartRow(.starred, symbol: "star.fill", tint: .yellow)
                        if model.counts.readLaterTotal > 0 {
                            smartRow(.readLater, symbol: "bookmark.fill", tint: .purple)
                        }
                        ForEach(model.smartFeeds) { (smart: RSSSmartFeed) in
                            smartRow(.smart(smart.id), symbol: "gearshape.fill", tint: .gray, custom: smart)
                        }
                    }
                } header: {
                    sectionHeader(RSSReaderText.smartFeeds, key: "smart")
                }
            }
            Section {
                if !model.isCollapsed("subscriptions") || !feedQuery.isEmpty {
                    let entries: [RSSSidebarEntry] = model.sidebar(search: feedQuery)
                    if model.feeds.isEmpty {
                        emptyRows
                    } else if entries.isEmpty {
                        Text(feedQuery.isEmpty ? "No feeds have unread articles." : "No feeds match.")
                            .foregroundStyle(.secondary)
                    }
                    ForEach(entries) { (entry: RSSSidebarEntry) in
                        sidebarRow(entry)
                    }
                }
            } header: {
                sectionHeader(RSSReaderText.onThisIPhone, key: "subscriptions")
            }
        }
        .listStyle(.insetGrouped)
        .rssPalette(model.settings.palette)
    }

    private func sectionHeader(_ title: String, key: String) -> some View {
        Button {
            withAnimation(.snappy) { model.toggleCollapsed(key) }
        } label: {
            HStack {
                Text(title)
                Spacer()
                Image(systemName: "chevron.down")
                    .font(.caption.weight(.semibold))
                    .rotationEffect(.degrees(model.isCollapsed(key) ? -90 : 0))
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(title), \(model.isCollapsed(key) ? "collapsed" : "expanded")")
        .accessibilityHint("Shows or hides the section")
    }

    private func smartRow(_ scope: RSSScope, symbol: String, tint: Color, custom: RSSSmartFeed? = nil) -> some View {
        ShellPaneLink {
            RSSReaderTimelineScreen(model: model, scope: scope)
        } label: {
            HStack(spacing: 12) {
                Image(systemName: symbol)
                    .foregroundStyle(tint)
                    .font(.system(size: 17))
                    .frame(width: 24)
                Text(model.title(scope))
                Spacer()
                RSSCountLabel(count: model.unread(scope))
            }
        }
        .contextMenu {
            if scope != .starred {
                Button {
                    askMarkAll(scope)
                } label: {
                    Label("Mark All as Read", systemImage: "checkmark.circle")
                }
            }
            if let custom {
                Button {
                    smartEditor = RSSSmartFeedDraft(smart: custom)
                } label: {
                    Label("Edit Smart Feed", systemImage: "pencil")
                }
                Button(role: .destructive) {
                    deleteSmartTarget = custom
                } label: {
                    Label("Delete Smart Feed", systemImage: "trash")
                }
            }
        }
        .accessibilityIdentifier("rss-smart-\(scope.key)")
    }

    @ViewBuilder private var emptyRows: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("No feeds yet").font(.headline)
            Text("Add a feed or a site address, or import an OPML file from Settings.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .padding(.vertical, 4)
        Button {
            showAddFeed = true
        } label: {
            Label("Add Feed", systemImage: "plus")
        }
        Button {
            Task { await addForkMeshBlog() }
        } label: {
            Label("Add the ForkMesh Blog Feed", systemImage: "dot.radiowaves.up.forward")
        }
    }

    private func addForkMeshBlog() async {
        do {
            _ = try await model.addForkMeshBlog()
        } catch {
            problem = RSSReaderErrors.message(error)
        }
    }

    @ViewBuilder private func sidebarRow(_ entry: RSSSidebarEntry) -> some View {
        switch entry {
        case let .feed(feed, unread, indented):
            ShellPaneLink {
                RSSReaderTimelineScreen(model: model, scope: .feed(feed.id))
            } label: {
                HStack(spacing: 12) {
                    RSSFeedIcon(feed: feed, title: feed.displayTitle, size: 22, remote: model.settings.loadRemoteImages)
                        .opacity(feed.paused ? 0.5 : 1)
                    Text(feed.displayTitle)
                        .lineLimit(1)
                        .foregroundStyle(feed.paused ? Color.secondary : Color.primary)
                    if feed.paused {
                        Image(systemName: "pause.circle")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .accessibilityLabel("Updates Paused")
                    }
                    Spacer()
                    if feed.errorCount > 0 {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .font(.caption)
                            .foregroundStyle(.orange)
                            .accessibilityLabel(RSSReaderText.lastCheckFailed(feed.error))
                    }
                    RSSCountLabel(count: unread)
                }
                .padding(.leading, indented ? 28 : 0)
            }
            .contextMenu { feedMenu(feed) }
            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                Button(role: .destructive) {
                    unsubscribeTarget = feed
                } label: {
                    Label("Unsubscribe", systemImage: "trash")
                }
                Button {
                    infoFeed = feed
                } label: {
                    Label("Info", systemImage: "info.circle")
                }
                .tint(.gray)
            }
            .swipeActions(edge: .leading) {
                Button {
                    askMarkAll(.feed(feed.id))
                } label: {
                    Label("Mark All as Read", systemImage: "checkmark.circle")
                }
                .tint(.blue)
            }
        case let .folder(folder, unread, collapsed):
            ShellPaneLink {
                RSSReaderTimelineScreen(model: model, scope: .folder(folder.id))
            } label: {
                HStack(spacing: 12) {
                    Button {
                        withAnimation(.snappy) { model.toggleCollapsed("folder:\(folder.id)") }
                    } label: {
                        Image(systemName: "chevron.down")
                            .font(.caption.weight(.semibold))
                            .rotationEffect(.degrees(collapsed ? -90 : 0))
                            .frame(width: 22, height: 22)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.borderless)
                    .accessibilityLabel(collapsed ? "Expand \(folder.name)" : "Collapse \(folder.name)")
                    Image(systemName: "folder")
                        .foregroundStyle(Color.accentColor)
                    Text(folder.name)
                        .lineLimit(1)
                    Spacer()
                    RSSCountLabel(count: unread)
                }
            }
            .contextMenu {
                Button {
                    askMarkAll(.folder(folder.id))
                } label: {
                    Label("Mark All as Read", systemImage: "checkmark.circle")
                }
                Button {
                    folderName = folder.name
                    folderPrompt = RSSFolderPrompt(folderId: folder.id)
                } label: {
                    Label("Rename", systemImage: "pencil")
                }
                Button(role: .destructive) {
                    deleteFolderTarget = folder
                } label: {
                    Label("Delete", systemImage: "trash")
                }
            }
            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                Button(role: .destructive) {
                    deleteFolderTarget = folder
                } label: {
                    Label("Delete", systemImage: "trash")
                }
                Button {
                    folderName = folder.name
                    folderPrompt = RSSFolderPrompt(folderId: folder.id)
                } label: {
                    Label("Rename", systemImage: "pencil")
                }
                .tint(.gray)
            }
        }
    }

    @ViewBuilder private func feedMenu(_ feed: RSSFeed) -> some View {
        Button {
            askMarkAll(.feed(feed.id))
        } label: {
            Label("Mark All as Read", systemImage: "checkmark.circle")
        }
        Button {
            infoFeed = feed
        } label: {
            Label("Feed Info", systemImage: "info.circle")
        }
        Button(role: .destructive) {
            unsubscribeTarget = feed
        } label: {
            Label("Unsubscribe", systemImage: "trash")
        }
    }

    private func askMarkAll(_ scope: RSSScope) {
        guard model.unread(scope) > 0 else { return }
        if model.settings.confirmMarkAllRead {
            markTarget = scope
        } else {
            model.markAllRead(scope)
        }
    }

    private func askOlder(_ scope: RSSScope?, _ choice: RSSCatchUp) {
        guard model.olderUnreadCount(scope, choice) > 0 else { return }
        if model.settings.confirmMarkAllRead {
            olderTarget = RSSOlderRequest(scope: scope, choice: choice)
        } else {
            model.markOlderRead(scope, choice)
        }
    }

    private func saveFolder() {
        guard let prompt = folderPrompt else { return }
        do {
            if let id = prompt.folderId {
                try model.renameFolder(id, to: folderName)
            } else {
                _ = try model.addFolder(folderName)
            }
        } catch {
            problem = RSSReaderErrors.message(error)
        }
        folderPrompt = nil
    }

    private var bottomBar: some View {
        HStack {
            Button {
                showSettings = true
            } label: {
                Image(systemName: "gearshape")
                    .font(.system(size: 19, weight: .medium))
                    .frame(width: 48, height: 48)
            }
            .buttonStyle(.plain)
            .rssGlass(Capsule())
            .accessibilityLabel("Settings")
            .accessibilityIdentifier("rss-settings")
            Spacer()
            Menu {
                Button {
                    showAddFeed = true
                } label: {
                    Label("Add Feed", systemImage: "dot.radiowaves.up.forward")
                }
                Button {
                    folderName = ""
                    folderPrompt = RSSFolderPrompt(folderId: nil)
                } label: {
                    Label("Add Folder", systemImage: "folder.badge.plus")
                }
                Button {
                    smartEditor = RSSSmartFeedDraft(smart: RSSSmartFeed(id: ""))
                } label: {
                    Label("New Smart Feed\u{2026}", systemImage: "gearshape")
                }
                .disabled(model.smartFeeds.count >= RSSReaderLimits.maxSmartFeeds)
            } label: {
                Image(systemName: "plus")
                    .font(.system(size: 21, weight: .semibold))
                    .frame(width: 48, height: 48)
            }
            .buttonStyle(.plain)
            .rssGlass(Circle())
            .accessibilityLabel("Add")
            .accessibilityIdentifier("rss-add")
        }
        .padding(.horizontal, 16)
        .padding(.bottom, 8)
    }
}

struct RSSOlderRequest: Equatable {
    var scope: RSSScope?
    var choice: RSSCatchUp
}

struct RSSSmartFeedDraft: Identifiable {
    var smart: RSSSmartFeed

    var id: String { smart.id.isEmpty ? "new" : smart.id }
}

struct RSSFolderPrompt: Equatable {
    var folderId: String?

    var title: String { folderId == nil ? "Add Folder" : "Rename Folder" }
    var action: String { folderId == nil ? "Add" : "Rename" }
}

struct RSSAddFeedSheet: View {
    let model: RSSReaderModel
    @Environment(\.dismiss) private var dismiss
    @State private var address = ""
    @State private var name = ""
    @State private var folderId = ""
    @State private var adding = false
    @State private var error = ""

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Feed or site address", text: $address)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .submitLabel(.go)
                        .onSubmit { add() }
                        .accessibilityIdentifier("rss-add-address")
                    TextField("Name (optional)", text: $name)
                } footer: {
                    Text("A feed address such as example.org/feed.xml, or a site address: RSS Reader looks for its feed.")
                }
                Section {
                    Picker("Folder", selection: $folderId) {
                        Text("None").tag("")
                        ForEach(model.folders) { (folder: RSSFolder) in
                            Text(folder.name).tag(folder.id)
                        }
                    }
                }
                if !error.isEmpty {
                    Section {
                        Label(error, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle("Add Feed")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    if adding {
                        ProgressView()
                    } else {
                        Button("Add") { add() }
                            .disabled(address.trimmingCharacters(in: .whitespaces).isEmpty)
                    }
                }
            }
            .interactiveDismissDisabled(adding)
        }
    }

    private func add() {
        guard !adding else { return }
        adding = true
        error = ""
        Task {
            do {
                _ = try await model.addFeed(address, title: name, folderId: folderId)
                adding = false
                dismiss()
            } catch {
                adding = false
                self.error = RSSReaderErrors.message(error)
            }
        }
    }
}

struct RSSFeedInfoSheet: View {
    let model: RSSReaderModel
    let feedId: String
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @State private var name = ""
    @State private var folderId = ""
    @State private var address = ""
    @State private var addressError = ""
    @State private var checking = false
    @State private var checkLine = ""
    @State private var confirmUnsubscribe = false

    var body: some View {
        NavigationStack {
            Group {
                if let feed = model.feed(feedId) {
                    form(feed)
                } else {
                    Text("This feed is no longer on this iPhone.")
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("Feed Info")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        save()
                        if saveAddress() { dismiss() }
                    }
                }
            }
            .onAppear {
                if let feed = model.feed(feedId) {
                    name = feed.displayTitle
                    folderId = feed.folderId
                    address = feed.url
                }
            }
        }
    }

    private func form(_ feed: RSSFeed) -> some View {
        Form {
            Section {
                HStack(spacing: 12) {
                    RSSFeedIcon(feed: feed, title: feed.displayTitle, size: 40, remote: model.settings.loadRemoteImages)
                    TextField("Name", text: $name)
                        .font(.headline)
                        .submitLabel(.done)
                        .onSubmit { save() }
                }
                Picker("Folder", selection: $folderId) {
                    Text("None").tag("")
                    ForEach(model.folders) { (folder: RSSFolder) in
                        Text(folder.name).tag(folder.id)
                    }
                }
            }
            Section("Addresses") {
                if let home = URL(string: feed.homePage), !feed.homePage.isEmpty {
                    Button {
                        openURL(home)
                    } label: {
                        LabeledContent("Home Page") {
                            Text(feed.homePage).lineLimit(1).truncationMode(.middle)
                        }
                    }
                }
                VStack(alignment: .leading, spacing: 4) {
                    Text("Feed Address")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    TextField("Feed Address", text: $address)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .submitLabel(.done)
                        .onSubmit { _ = saveAddress() }
                        .accessibilityIdentifier("rss-feed-address")
                }
                .contextMenu {
                    Button {
                        UIPasteboard.general.string = feed.url
                    } label: {
                        Label("Copy Feed Address", systemImage: "doc.on.doc")
                    }
                }
                if address.trimmingCharacters(in: .whitespacesAndNewlines) != feed.url {
                    Button("Save Address") { _ = saveAddress() }
                }
                if !addressError.isEmpty {
                    Label(addressError, systemImage: "exclamationmark.triangle")
                        .foregroundStyle(.red)
                }
            }
            Section("Options") {
                Toggle("Pause Updates", isOn: flag(\.paused))
                Toggle("Hide from Today", isOn: flag(\.excludeFromToday))
                Toggle("Hide from Unread Counts", isOn: flag(\.excludeFromUnread))
            }
            Section("Status") {
                LabeledContent("Last Check", value: RSSReaderText.stamp(feed.fetchedAt))
                if feed.errorCount > 0 {
                    LabeledContent("Last Error") {
                        Text(RSSReaderText.feedError(feed.error))
                            .foregroundStyle(.red)
                            .multilineTextAlignment(.trailing)
                    }
                }
                Button {
                    checkNow()
                } label: {
                    HStack {
                        Text("Check Now")
                        if checking {
                            Spacer()
                            ProgressView()
                        }
                    }
                }
                .disabled(checking)
                .accessibilityIdentifier("rss-check-now")
                if !checkLine.isEmpty {
                    Text(checkLine)
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            Section {
                Button("Unsubscribe", role: .destructive) { confirmUnsubscribe = true }
            }
        }
        .confirmationDialog("Unsubscribe from \u{201C}\(feed.displayTitle)\u{201D}?", isPresented: $confirmUnsubscribe, titleVisibility: .visible) {
            Button("Unsubscribe", role: .destructive) {
                model.unsubscribe(feed.id)
                dismiss()
            }
        }
    }

    private func flag(_ path: WritableKeyPath<RSSFeed, Bool>) -> Binding<Bool> {
        Binding(
            get: { model.feed(feedId)?[keyPath: path] ?? false },
            set: { (value: Bool) in model.setFeedFlag(feedId, path, value) })
    }

    private func checkNow() {
        guard !checking else { return }
        checking = true
        Task {
            let line: String = await model.checkNow(feedId)
            checkLine = line
            checking = false
        }
    }

    private func saveAddress() -> Bool {
        guard let feed = model.feed(feedId) else { return true }
        let trimmed: String = address.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed != feed.url else {
            addressError = ""
            return true
        }
        do {
            addressError = ""
            if try model.changeAddress(feedId, to: trimmed) {
                address = model.feed(feedId)?.url ?? trimmed
                checkNow()
            }
            return true
        } catch {
            addressError = RSSReaderErrors.message(error)
            return false
        }
    }

    private func save() {
        guard let feed = model.feed(feedId) else { return }
        let trimmed: String = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty && trimmed != feed.displayTitle { model.renameFeed(feed.id, to: trimmed) }
        if folderId != feed.folderId { model.moveFeed(feed.id, toFolder: folderId) }
    }
}
