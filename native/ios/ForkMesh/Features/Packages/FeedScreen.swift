import FMApi
import FMBase
import FMTools
import SwiftUI
import UIKit
import UniformTypeIdentifiers

struct FeedScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: InstanceFeedModel?
    private let pushed: Bool
    private let start: InstanceFeedView

    init(post: String? = nil) {
        if let post, InstanceFeed.validPostId(post) {
            self.init(view: .thread(post), pushed: false)
        } else {
            self.init(view: .timeline, pushed: false)
        }
    }

    init(view: InstanceFeedView, pushed: Bool) {
        self.pushed = pushed
        start = view
        let model: InstanceFeedModel? = Services.get(ApiService.self).map { (api: ApiService) -> InstanceFeedModel in
            InstanceFeedModel(api: InstanceFeedAPI(api: api), view: view)
        }
        _model = State(initialValue: model)
    }

    private var title: String {
        guard pushed else { return InstanceFeedModel.title }
        switch start {
        case .timeline: return InstanceFeedModel.title
        case .thread: return "Post"
        case .profile(let name): return "@" + name
        }
    }

    var body: some View {
        content
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
    }

    @ViewBuilder private var content: some View {
        if let model {
            FeedContent(model: model, pushed: pushed)
        } else {
            FmEmptyState(icon: "forum_outlined", title: InstanceFeedModel.unavailableTitle)
        }
    }
}

private enum FeedPush: Hashable, Identifiable {
    case thread(String)
    case profile(String)

    var id: String {
        switch self {
        case .thread(let post): return "post-" + post
        case .profile(let name): return "author-" + name
        }
    }

    var view: InstanceFeedView {
        switch self {
        case .thread(let post): return .thread(post)
        case .profile(let name): return .profile(name)
        }
    }
}

private enum FeedLinks {
    static let authorScheme = "fm-feed-author"

    static func author(_ name: String) -> URL? {
        URL(string: authorScheme + ":" + name)
    }

    static func authorName(_ url: URL) -> String? {
        guard url.scheme == authorScheme else { return nil }
        let name = String(url.absoluteString.dropFirst(authorScheme.count + 1))
        return InstanceFeed.validAuthor(name) ? name : nil
    }

    static func body(_ text: String, link: Color) -> AttributedString {
        var out = AttributedString()
        for segment in InstanceFeed.segments(text) {
            switch segment {
            case .text(let value):
                out += AttributedString(value)
            case .link(let value):
                var part = AttributedString(value)
                if let url = URL(string: value) {
                    part.link = url
                    part.foregroundColor = link
                }
                out += part
            case .mention(let name):
                var part = AttributedString("@" + name)
                if let url = author(name) {
                    part.link = url
                    part.foregroundColor = link
                }
                out += part
            }
        }
        return out
    }

    static let since: DateFormatter = {
        let formatter = DateFormatter()
        formatter.setLocalizedDateFormatFromTemplate("MMMM yyyy")
        return formatter
    }()

    static func profileLine(_ profile: InstanceFeedProfile) -> String {
        var line = "@\(profile.name) · \(profile.posts) \(profile.posts == 1 ? "post" : "posts")"
        if profile.since > 0 {
            line += " · posting since " + since.string(from: Date(timeIntervalSince1970: TimeInterval(profile.since) / 1000))
        }
        return line
    }
}

private struct FeedContent: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @Bindable var model: InstanceFeedModel
    let pushed: Bool
    @State private var push: FeedPush?
    @State private var revealed: Set<String> = []
    @State private var deleting: InstanceFeedPost?

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
            .task {
                if !model.checked && !model.loading { await model.load() }
            }
            .navigationDestination(item: $push) { (target: FeedPush) in
                FeedScreen(view: target.view, pushed: true)
            }
            .sheet(isPresented: replying) {
                FeedReplySheet(model: model)
            }
            .fmDialog(isPresented: confirmingDelete) {
                FmConfirmDialog(
                    title: "Delete this post?",
                    message: "Its favourites and boosts are removed with it.",
                    confirmLabel: "Delete",
                    destructive: true,
                    onCancel: { deleting = nil },
                    onConfirm: {
                        let post = deleting
                        deleting = nil
                        if let post { Task { await model.delete(post) } }
                    })
            }
            .onChange(of: model.view) { (_: InstanceFeedView, view: InstanceFeedView) in
                if pushed && view == .timeline { dismiss() }
            }
            .environment(\.openURL, OpenURLAction { (url: URL) -> OpenURLAction.Result in
                guard let name = FeedLinks.authorName(url) else { return .systemAction }
                MainActor.assumeIsolated { openProfile(name) }
                return .handled
            })
    }

    private var replying: Binding<Bool> {
        Binding<Bool>(get: { model.replyTo != nil }, set: { (shown: Bool) in if !shown { model.cancelReply() } })
    }

    private var confirmingDelete: Binding<Bool> {
        Binding<Bool>(get: { deleting != nil }, set: { (shown: Bool) in if !shown { deleting = nil } })
    }

    private var isThread: Bool {
        if case .thread = model.view { return true }
        return false
    }

    @ViewBuilder private var main: some View {
        if model.showsInstall {
            notInstalled
        } else if model.showsSpinner || (!model.checked && model.error.isEmpty) {
            FmSkeletonList(label: "Loading the feed")
        } else if model.showsError {
            refreshableMessage(
                icon: "error_outline",
                title: isThread ? "Post not found" : InstanceFeedModel.unavailableTitle,
                message: model.error)
        } else {
            list
        }
    }

    private var notInstalled: some View {
        ScrollView {
            VStack(spacing: FmSpace.x3) {
                FmEmptyState(
                    icon: "forum_outlined",
                    title: InstanceFeedModel.notInstalledTitle,
                    message: InstanceFeedModel.notInstalledMessage + "\n\nInstall it from the Market, then pull to refresh.")
                FmFilledButton("Open in the Market", onPressed: {
                    FmHaptics.tap()
                    Services.get(AppRouter.self)?.open(.package(id: InstanceFeed.packageId))
                })
                .accessibilityIdentifier("feed-open-market")
            }
            .padding(FmSpace.x4)
            .containerRelativeFrame([.horizontal, .vertical])
        }
        .fmRefreshable { await model.load() }
    }

    private func refreshableMessage(icon: String, title: String, message: String) -> some View {
        ScrollView {
            VStack(spacing: FmSpace.x3) {
                FmEmptyState(icon: icon, title: title, message: message)
                if isThread && !pushed {
                    FmTextButton("Back to the feed", onPressed: { Task { await model.showTimeline() } })
                }
            }
            .containerRelativeFrame([.horizontal, .vertical])
        }
        .fmRefreshable { await model.load() }
    }

    private var list: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
                    if !model.error.isEmpty {
                        FmNoticeLine(model.error, isError: true)
                    }
                    if isThread && !pushed {
                        FmTextButton("Back to the feed", minHeight: 36, onPressed: { Task { await model.showTimeline() } })
                            .accessibilityIdentifier("feed-back")
                    }
                    if model.view == .timeline {
                        FmCard {
                            FeedComposer(model: model, replying: false)
                        }
                    }
                    if case .profile = model.view, let profile = model.profile {
                        profileHeader(profile)
                    }
                }
                .padding(FmSpace.x4)
                rows
                footer
            }
        }
        .fmRefreshable { await model.load() }
    }

    @ViewBuilder private var rows: some View {
        if isThread {
            if let thread = model.thread {
                ForEach(thread.posts) { (post: InstanceFeedPost) in
                    row(post, booster: nil, focused: post.id == thread.post.id)
                }
            } else {
                FmEmptyState(icon: "forum_outlined", title: "Post not found", message: "It may have been deleted.")
            }
        } else if model.entries.isEmpty {
            if case .profile(let name) = model.view {
                FmEmptyState(icon: "forum_outlined", title: "No posts yet", message: "@\(name) has not posted to the feed.")
            } else {
                FmEmptyState(icon: "forum_outlined", title: "No posts yet", message: "Be the first to post to this instance.")
            }
        } else {
            ForEach(model.entries) { (entry: InstanceFeedEntry) in
                row(entry.post, booster: entry.isBoost ? FeedBooster(name: entry.booster, display: entry.boosterName) : nil, focused: false)
            }
        }
    }

    @ViewBuilder private var footer: some View {
        if !isThread && !model.next.isEmpty {
            HStack {
                Spacer(minLength: 0)
                if model.loading {
                    ProgressView()
                        .tint(fm.accent)
                        .frame(minHeight: 44)
                } else {
                    FmTextButton("Load older posts", onPressed: { Task { await model.load(append: true) } })
                        .accessibilityIdentifier("feed-load-more")
                }
                Spacer(minLength: 0)
            }
            .padding(FmSpace.x3)
            .onAppear { Task { await model.load(append: true) } }
        }
    }

    private func profileHeader(_ profile: InstanceFeedProfile) -> some View {
        HStack(spacing: FmSpace.x3) {
            FmAvatar(name: profile.displayName, size: 56)
            VStack(alignment: .leading, spacing: 2) {
                Text(profile.displayName)
                    .fmText(.titleMedium)
                    .lineLimit(1)
                Text(FeedLinks.profileLine(profile))
                    .fmText(.bodySmall, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .combine)
        .accessibilityIdentifier("feed-profile")
    }

    private func row(_ post: InstanceFeedPost, booster: FeedBooster?, focused: Bool) -> some View {
        VStack(spacing: 0) {
            FeedPostRow(
                post: post,
                booster: booster,
                focused: focused,
                revealed: revealed.contains(post.id),
                onReveal: {
                    if revealed.contains(post.id) { revealed.remove(post.id) } else { revealed.insert(post.id) }
                },
                onOpenThread: { openThread(post.id) },
                onOpenProfile: { (name: String) in openProfile(name) },
                onReply: { model.startReply(post) },
                onBoost: { Task { await model.toggleBoost(post) } },
                onFavourite: { Task { await model.toggleFavourite(post) } },
                onDelete: { deleting = post },
                publicURL: model.publicURL(post),
                onCopyLink: { copyLink(post) },
                onOpenWebsite: {
                    if let url = model.publicURL(post) { openURL(url) }
                })
            FmHairline()
        }
    }

    private func copyLink(_ post: InstanceFeedPost) {
        guard let url = model.publicURL(post) else { return }
        UIPasteboard.general.items = [[UTType.url.identifier: url, UTType.plainText.identifier: url.absoluteString]]
        model.linkCopied()
        FmHaptics.commit()
    }

    private func openThread(_ id: String) {
        guard InstanceFeed.validPostId(id), model.thread?.post.id != id else { return }
        FmHaptics.tap()
        push = .thread(id)
    }

    private func openProfile(_ name: String) {
        let name = name.lowercased()
        guard InstanceFeed.validAuthor(name), model.view != .profile(name) else { return }
        FmHaptics.tap()
        push = .profile(name)
    }
}

private struct FeedBooster: Equatable {
    let name: String
    let display: String
}

private struct FeedPostRow: View {
    @Environment(\.fmPalette) private var fm
    let post: InstanceFeedPost
    let booster: FeedBooster?
    let focused: Bool
    let revealed: Bool
    let onReveal: () -> Void
    let onOpenThread: () -> Void
    let onOpenProfile: (String) -> Void
    let onReply: () -> Void
    let onBoost: () -> Void
    let onFavourite: () -> Void
    let onDelete: () -> Void
    let publicURL: URL?
    let onCopyLink: () -> Void
    let onOpenWebsite: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if let booster {
                Button(action: { onOpenProfile(booster.name) }) {
                    HStack(spacing: FmSpace.x1) {
                        Image(systemName: "arrow.2.squarepath")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(fm.textTertiary)
                            .accessibilityHidden(true)
                        Text("\(booster.display) boosted")
                            .fmText(.labelMedium, color: fm.textSecondary)
                            .lineLimit(1)
                    }
                    .padding(.leading, 48)
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("feed-boosted-by")
            }
            HStack(alignment: .top, spacing: FmSpace.x3) {
                Button(action: { onOpenProfile(post.author) }) {
                    FmAvatar(name: post.authorName, size: 36, fontSize: 15)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(post.authorName)
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    header
                    if !post.replyTo.isEmpty { replyingLine }
                    if post.spoiler.isEmpty {
                        bodyText
                    } else {
                        spoilerBlock
                    }
                    actions
                }
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x3)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(focused ? fm.accentSubtle : Color.clear)
        .contentShape(Rectangle())
        .onTapGesture {
            if !focused { onOpenThread() }
        }
        .accessibilityIdentifier("feed-post-\(post.id)")
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
            Button(action: { onOpenProfile(post.author) }) {
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
                    Text(post.authorName)
                        .fmText(.titleSmall)
                        .lineLimit(1)
                    Text("@" + post.author)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(1)
                }
            }
            .buttonStyle(.plain)
            .layoutPriority(1)
            Spacer(minLength: FmSpace.x1)
            Text(Format.relativeTime(post.createdAt))
                .fmText(.bodySmall, color: fm.textTertiary)
                .lineLimit(1)
                .fixedSize()
        }
    }

    @ViewBuilder private var replyingLine: some View {
        if post.replyToAuthor.isEmpty {
            Text("Replying to a removed post")
                .fmText(.bodySmall, color: fm.textSecondary)
        } else {
            Button(action: { onOpenProfile(post.replyToAuthor) }) {
                HStack(spacing: 0) {
                    Text("Replying to ")
                        .fmText(.bodySmall, color: fm.textSecondary)
                    Text("@" + post.replyToAuthor)
                        .fmText(.bodySmall, color: fm.accent)
                }
            }
            .buttonStyle(.plain)
        }
    }

    private var bodyText: some View {
        Text(FeedLinks.body(post.text, link: fm.accent))
            .fmText(focused ? .bodyLarge : .bodyMedium)
            .tint(fm.accent)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .textSelection(.enabled)
    }

    private var spoilerBlock: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack(alignment: .center, spacing: FmSpace.x2) {
                FmIcon("warning_amber", size: 16, color: fm.warning)
                Text(post.spoiler)
                    .fmText(.bodyMedium, weight: .semibold)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmTextButton(revealed ? "Show less" : "Show more", minHeight: 32, onPressed: onReveal)
                    .accessibilityIdentifier("feed-spoiler-toggle")
            }
            if revealed { bodyText }
        }
    }

    private var actions: some View {
        HStack(spacing: FmSpace.x4) {
            FeedActionButton(
                symbol: "arrowshape.turn.up.left", label: "Reply", count: post.replies,
                active: false, activeColor: fm.accent, onPressed: onReply)
            FeedActionButton(
                symbol: "arrow.2.squarepath", label: post.boosted ? "Undo boost" : "Boost", count: post.boosts,
                active: post.boosted, activeColor: fm.accent, onPressed: onBoost)
            FeedActionButton(
                symbol: post.favourited ? "heart.fill" : "heart",
                label: post.favourited ? "Remove favourite" : "Favourite", count: post.favourites,
                active: post.favourited, activeColor: fm.warning, onPressed: onFavourite)
            Spacer(minLength: 0)
            if publicURL != nil { linkMenu }
            if post.canDelete {
                FeedActionButton(
                    symbol: "trash", label: "Delete post", count: 0,
                    active: false, activeColor: fm.danger, onPressed: onDelete)
            }
        }
        .padding(.top, FmSpace.x1)
    }
}

extension FeedPostRow {
    fileprivate var linkMenu: some View {
        Menu {
            Button(action: onOpenWebsite) {
                Label("View on the website", systemImage: "globe")
            }
            .accessibilityIdentifier("feed-post-view-website")
            Button(action: onCopyLink) {
                Label("Copy link", systemImage: "link")
            }
            .accessibilityIdentifier("feed-post-copy-link")
        } label: {
            Image(systemName: "square.and.arrow.up")
                .font(.system(size: 15, weight: .medium))
                .foregroundStyle(fm.textSecondary)
                .frame(minWidth: 44, minHeight: 36, alignment: .leading)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel("Post link")
        .accessibilityIdentifier("feed-post-link")
    }
}

private struct FeedActionButton: View {
    @Environment(\.fmPalette) private var fm
    let symbol: String
    let label: String
    let count: Int
    let active: Bool
    let activeColor: Color
    let onPressed: () -> Void

    var body: some View {
        Button(action: {
            FmHaptics.tap()
            onPressed()
        }) {
            HStack(spacing: FmSpace.x1) {
                Image(systemName: symbol)
                    .font(.system(size: 15, weight: .medium))
                if count > 0 {
                    Text("\(count)")
                        .fmText(.labelMedium, color: active ? activeColor : fm.textSecondary, tabular: true)
                }
            }
            .foregroundStyle(active ? activeColor : fm.textSecondary)
            .frame(minWidth: 44, minHeight: 36, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(count > 0 ? "\(label), \(count)" : label)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}

private struct FeedComposer: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: InstanceFeedModel
    let replying: Bool

    private var postAction: (() -> Void)? {
        guard model.canPost else { return nil }
        let model = model
        return { Task { await model.submit() } }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if replying, let parent = model.replyTo {
                Text("Replying to @\(parent.author)")
                    .fmText(.bodySmall, color: fm.textSecondary)
            }
            if model.showSpoiler {
                TextField("Content warning", text: $model.spoiler)
                    .fmText(.bodyMedium)
                    .onChange(of: model.spoiler) { (_: String, value: String) in
                        if value.count > InstanceFeed.maxSpoiler {
                            model.spoiler = String(value.prefix(InstanceFeed.maxSpoiler))
                        }
                    }
                    .accessibilityIdentifier("feed-compose-spoiler")
                FmHairline()
            }
            TextField(replying ? "Write your reply" : "What’s happening on the instance?", text: $model.text, axis: .vertical)
                .fmText(.bodyLarge)
                .lineLimit(3...10)
                .accessibilityIdentifier("feed-compose-text")
            HStack(spacing: FmSpace.x2) {
                FmTextButton(
                    model.showSpoiler ? "Remove warning" : "Content warning", icon: "warning_amber",
                    color: model.showSpoiler ? fm.accent : fm.textSecondary, minHeight: 36,
                    onPressed: { model.showSpoiler.toggle() })
                    .accessibilityIdentifier("feed-compose-spoiler-toggle")
                Spacer(minLength: 0)
                Text("\(model.remaining)")
                    .fmText(.labelMedium, color: model.remaining < 0 ? fm.danger : fm.textTertiary, tabular: true)
                    .accessibilityLabel("\(model.remaining) characters left")
                FmFilledButton(model.posting ? "Posting…" : "Post", busy: model.posting, onPressed: postAction)
                    .accessibilityIdentifier("feed-compose-post")
            }
        }
    }
}

private struct FeedReplySheet: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var model: InstanceFeedModel

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    FmNoticeLine(model.notice?.message, isError: model.notice?.isError ?? false, onDismiss: { model.notice = nil })
                    FeedComposer(model: model, replying: true)
                }
                .padding(FmSpace.x4)
            }
            .background(fm.canvas)
            .navigationTitle("Reply")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { model.cancelReply() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}
