import FMBase
import FMPackages
import SwiftUI
import UIKit
import UniformTypeIdentifiers

enum MastodonLinks {
    static let accountScheme = "fm-mastodon-account"
    static let tagScheme = "fm-mastodon-tag"

    static func body(_ segments: [MastodonSegment], link: Color) -> AttributedString {
        var out = AttributedString()
        for segment in segments {
            switch segment {
            case .text(let value):
                out += AttributedString(value)
            case .link(let label, let url):
                out += linked(label, URL(string: url), link)
            case .mention(let label, let account):
                out += linked(label, URL(string: accountScheme + ":" + account), link)
            case .hashtag(let label, let name):
                let escaped = name.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? name
                out += linked(label, URL(string: tagScheme + ":" + escaped), link)
            }
        }
        return out
    }

    private static func linked(_ label: String, _ url: URL?, _ color: Color) -> AttributedString {
        var part = AttributedString(label)
        if let url {
            part.link = url
            part.foregroundColor = color
        }
        return part
    }

    static func push(_ url: URL) -> MastodonPush? {
        let rest = String(url.absoluteString.drop { $0 != ":" }.dropFirst())
        guard !rest.isEmpty else { return nil }
        if url.scheme == accountScheme { return .profile(rest) }
        if url.scheme == tagScheme { return .tag(rest.removingPercentEncoding ?? rest) }
        return nil
    }
}

struct MastodonAvatar: View {
    let url: String
    let name: String
    let size: CGFloat

    init(url: String, name: String, size: CGFloat = 40) {
        self.url = url
        self.name = name
        self.size = size
    }

    var body: some View {
        AsyncImage(url: URL(string: url)) { (phase: AsyncImagePhase) in
            if let image = phase.image {
                image.resizable().scaledToFill()
            } else {
                FmAvatar(name: name, size: size, fontSize: size * 0.42)
            }
        }
        .frame(width: size, height: size)
        .clipShape(Circle())
        .accessibilityHidden(true)
    }
}

struct MastodonFeedList: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let hub: MastodonHub
    @Bindable var feed: MastodonFeedModel
    let empty: String
    @Binding var push: MastodonPush?
    @Binding var compose: MastodonCompose?
    let header: AnyView?
    @State private var revealed: Set<String> = []
    @State private var deleting: MastodonStatus?

    init(hub: MastodonHub, feed: MastodonFeedModel, empty: String, push: Binding<MastodonPush?>,
         compose: Binding<MastodonCompose?>, header: AnyView? = nil) {
        self.hub = hub
        self.feed = feed
        self.empty = empty
        _push = push
        _compose = compose
        self.header = header
    }

    var body: some View {
        content
            .task {
                if !feed.loaded && !feed.loading { await feed.load() }
            }
            .fmDialog(isPresented: confirmingDelete) {
                FmConfirmDialog(
                    title: "Delete this post?",
                    message: "It is removed from your server and the servers it reached.",
                    confirmLabel: "Delete",
                    destructive: true,
                    onCancel: { deleting = nil },
                    onConfirm: {
                        let status = deleting
                        deleting = nil
                        if let status { Task { await feed.delete(status) } }
                    })
            }
            .environment(\.openURL, OpenURLAction { (url: URL) -> OpenURLAction.Result in
                guard let target = MastodonLinks.push(url) else { return .systemAction }
                MainActor.assumeIsolated { open(target) }
                return .handled
            })
    }

    private var confirmingDelete: Binding<Bool> {
        Binding<Bool>(get: { deleting != nil }, set: { (shown: Bool) in if !shown { deleting = nil } })
    }

    @ViewBuilder private var content: some View {
        if !feed.loaded && feed.isEmpty {
            FmSkeletonList(label: "Loading Mastodon")
        } else {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        if let header { header }
                        FmNoticeLine(feed.notice?.message, isError: feed.notice?.isError ?? false, onDismiss: { feed.notice = nil })
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.top, FmSpace.x2)
                    if feed.isEmpty {
                        FmEmptyState(
                            icon: feed.error.isEmpty ? "forum_outlined" : "error_outline",
                            title: feed.error.isEmpty ? "Nothing here yet" : "Mastodon is not answering",
                            message: feed.error.isEmpty ? empty : feed.error)
                            .padding(FmSpace.x4)
                    } else if case .notifications = feed.source {
                        ForEach(feed.notifications) { (item: MastodonNotification) in
                            notification(item)
                            FmHairline()
                        }
                    } else {
                        ForEach(feed.statuses) { (status: MastodonStatus) in
                            row(status)
                            FmHairline()
                        }
                    }
                    footer
                }
            }
            .fmRefreshable { await feed.load() }
        }
    }

    @ViewBuilder private var footer: some View {
        if feed.next != nil {
            HStack {
                Spacer(minLength: 0)
                if feed.loading {
                    ProgressView().tint(fm.accent).frame(minHeight: 44)
                } else {
                    FmTextButton("Load older posts", onPressed: { Task { await feed.load(append: true) } })
                }
                Spacer(minLength: 0)
            }
            .padding(FmSpace.x3)
            .onAppear { Task { await feed.load(append: true) } }
        } else if !feed.error.isEmpty && !feed.isEmpty {
            FmNoticeLine(feed.error, isError: true)
                .padding(FmSpace.x4)
        }
    }

    private func row(_ status: MastodonStatus) -> some View {
        MastodonStatusRow(
            status: status,
            server: hub.session?.server ?? "",
            focused: status.shown.id == feed.focusedId,
            revealed: revealed.contains(status.shown.id),
            mine: hub.isMine(status.shown.account),
            onReveal: { toggleReveal(status.shown.id) },
            onOpen: { (target: MastodonPush) in open(target) },
            onReply: { compose = MastodonCompose(replyTo: status.shown) },
            onBoost: { Task { await feed.toggleBoost(status.shown) } },
            onFavourite: { Task { await feed.toggleFavourite(status.shown) } },
            onBookmark: { Task { await feed.toggleBookmark(status.shown) } },
            onCopyLink: { copy(status.shown) },
            onBrowser: { if let url = URL(string: status.shown.url) { openURL(url) } },
            onDelete: { deleting = status.shown })
    }

    private func notification(_ item: MastodonNotification) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            Button(action: {
                if let status = item.status { open(.thread(status.id)) } else { open(.profile(item.account.id)) }
            }) {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    Image(systemName: item.symbol)
                        .font(.system(size: 15, weight: .semibold))
                        .foregroundStyle(item.type == "favourite" ? fm.warning : fm.accent)
                        .frame(width: 24)
                    MastodonAvatar(url: item.account.avatar, name: item.account.displayName, size: 32)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(item.summary)
                            .fmText(.titleSmall)
                            .fixedSize(horizontal: false, vertical: true)
                        if let status = item.status, item.type != "mention" && item.type != "status" {
                            Text(status.plainText)
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .lineLimit(3)
                        }
                    }
                    Spacer(minLength: 0)
                    Text(Format.relativeTime(item.createdAt))
                        .fmText(.bodySmall, color: fm.textTertiary)
                        .fixedSize()
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .padding(.horizontal, FmSpace.x4)
            .padding(.top, FmSpace.x3)
            .padding(.bottom, item.status != nil && (item.type == "mention" || item.type == "status") ? 0 : FmSpace.x3)
            if let status = item.status, item.type == "mention" || item.type == "status" {
                row(status)
            }
        }
    }

    private func toggleReveal(_ id: String) {
        if revealed.contains(id) { revealed.remove(id) } else { revealed.insert(id) }
    }

    private func open(_ target: MastodonPush) {
        if case .thread(let id) = target, id == feed.focusedId { return }
        FmHaptics.tap()
        push = target
    }

    private func copy(_ status: MastodonStatus) {
        guard let url = URL(string: status.url) else { return }
        UIPasteboard.general.items = [[UTType.url.identifier: url, UTType.plainText.identifier: url.absoluteString]]
        feed.notice = MastodonNotice("Link copied.")
        FmHaptics.commit()
    }
}

struct MastodonStatusRow: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let status: MastodonStatus
    let server: String
    let focused: Bool
    let revealed: Bool
    let mine: Bool
    let onReveal: () -> Void
    let onOpen: (MastodonPush) -> Void
    let onReply: () -> Void
    let onBoost: () -> Void
    let onFavourite: () -> Void
    let onBookmark: () -> Void
    let onCopyLink: () -> Void
    let onBrowser: () -> Void
    let onDelete: () -> Void
    @State private var showSensitive = false

    private var post: MastodonStatus { status.shown }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            if status.reblog != nil {
                Button(action: { onOpen(.profile(status.account.id)) }) {
                    HStack(spacing: FmSpace.x1) {
                        Image(systemName: "arrow.2.squarepath")
                            .font(.system(size: 12, weight: .semibold))
                            .foregroundStyle(fm.textTertiary)
                        Text("\(status.account.displayName) boosted")
                            .fmText(.labelMedium, color: fm.textSecondary)
                            .lineLimit(1)
                    }
                    .padding(.leading, 48)
                }
                .buttonStyle(.plain)
            }
            HStack(alignment: .top, spacing: FmSpace.x3) {
                Button(action: { onOpen(.profile(post.account.id)) }) {
                    MastodonAvatar(url: post.account.avatar, name: post.account.displayName, size: 36)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(post.account.displayName)
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    header
                    if post.spoiler.isEmpty {
                        details
                    } else {
                        warning
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
        .onTapGesture { if !focused { onOpen(.thread(post.id)) } }
        .accessibilityIdentifier("mastodon-post-\(post.id)")
    }

    private var header: some View {
        HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
            Button(action: { onOpen(.profile(post.account.id)) }) {
                HStack(alignment: .firstTextBaseline, spacing: FmSpace.x1) {
                    Text(post.account.displayName)
                        .fmText(.titleSmall)
                        .lineLimit(1)
                    Text("@" + post.account.acct)
                        .fmText(.bodySmall, color: fm.textSecondary)
                        .lineLimit(1)
                }
            }
            .buttonStyle(.plain)
            .layoutPriority(1)
            Spacer(minLength: FmSpace.x1)
            if post.visibility == "private" || post.visibility == "direct" {
                Image(systemName: post.visibility == "direct" ? "at" : "lock")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
                    .accessibilityLabel(post.visibility == "direct" ? "Mentioned people only" : "Followers only")
            }
            Text(Format.relativeTime(post.createdAt))
                .fmText(.bodySmall, color: fm.textTertiary)
                .lineLimit(1)
                .fixedSize()
        }
    }

    @ViewBuilder private var details: some View {
        let segments = post.segments
        if !segments.isEmpty {
            Text(MastodonLinks.body(segments, link: fm.accent))
                .fmText(focused ? .bodyLarge : .bodyMedium)
                .tint(fm.accent)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
                .textSelection(.enabled)
        }
        if !post.media.isEmpty { media }
        if let poll = post.poll { pollView(poll) }
    }

    private var warning: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            HStack(alignment: .center, spacing: FmSpace.x2) {
                FmIcon("warning_amber", size: 16, color: fm.warning)
                Text(post.spoiler)
                    .fmText(.bodyMedium, weight: .semibold)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmTextButton(revealed ? "Show less" : "Show more", minHeight: 32, onPressed: onReveal)
            }
            if revealed { details }
        }
    }

    private var media: some View {
        let hidden = post.sensitive && !showSensitive
        let columns = [GridItem(.flexible(), spacing: FmSpace.x1), GridItem(.flexible(), spacing: FmSpace.x1)]
        return ZStack {
            LazyVGrid(columns: post.media.count == 1 ? [GridItem(.flexible())] : columns, spacing: FmSpace.x1) {
                ForEach(post.media) { (item: MastodonAttachment) in
                    Button(action: { if let url = URL(string: item.url.isEmpty ? item.previewUrl : item.url) { openURL(url) } }) {
                        AsyncImage(url: URL(string: item.previewUrl)) { (phase: AsyncImagePhase) in
                            if let image = phase.image {
                                image.resizable().scaledToFill()
                            } else {
                                Rectangle().fill(fm.bgElevated)
                            }
                        }
                        .frame(height: post.media.count == 1 ? 200 : 120)
                        .frame(maxWidth: .infinity)
                        .clipped()
                        .overlay(alignment: .bottomLeading) {
                            if item.type == "video" || item.type == "gifv" {
                                Image(systemName: "play.circle.fill")
                                    .font(.system(size: 22))
                                    .foregroundStyle(.white)
                                    .padding(FmSpace.x1)
                            }
                        }
                        .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                        .blur(radius: hidden ? 24 : 0)
                    }
                    .buttonStyle(.plain)
                    .disabled(hidden)
                    .accessibilityLabel(item.description.isEmpty ? "Attachment" : item.description)
                }
            }
            if hidden {
                Button(action: { showSensitive = true }) {
                    Text("Sensitive media · tap to show")
                        .fmText(.labelLarge)
                        .padding(.horizontal, FmSpace.x3)
                        .padding(.vertical, FmSpace.x2)
                        .shellGlass(Capsule())
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.top, FmSpace.x1)
    }

    private func pollView(_ poll: MastodonPoll) -> some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            ForEach(poll.options.indices, id: \.self) { (index: Int) in
                let option = poll.options[index]
                HStack(spacing: FmSpace.x2) {
                    Text("\(poll.percent(option))%")
                        .fmText(.labelMedium, color: fm.textSecondary, tabular: true)
                        .frame(width: 40, alignment: .leading)
                    Text(option.title)
                        .fmText(.bodyMedium)
                        .lineLimit(2)
                    Spacer(minLength: 0)
                }
                .padding(.vertical, 2)
                .background(alignment: .leading) {
                    GeometryReader { (proxy: GeometryProxy) in
                        RoundedRectangle(cornerRadius: FmRadius.sm, style: .continuous)
                            .fill(fm.accentSubtle)
                            .frame(width: proxy.size.width * CGFloat(poll.percent(option)) / 100)
                    }
                }
            }
            Text("\(poll.votes) \(poll.votes == 1 ? "vote" : "votes")\(poll.expired ? " · Closed" : "")")
                .fmText(.bodySmall, color: fm.textTertiary)
        }
        .padding(.top, FmSpace.x1)
    }

    private var actions: some View {
        HStack(spacing: FmSpace.x3) {
            MastodonActionButton(symbol: "arrowshape.turn.up.left", label: "Reply", count: post.replies,
                                 active: false, activeColor: fm.accent, onPressed: onReply)
            MastodonActionButton(symbol: "arrow.2.squarepath", label: post.boosted ? "Undo boost" : "Boost", count: post.boosts,
                                 active: post.boosted, activeColor: fm.accent,
                                 onPressed: post.visibility == "private" || post.visibility == "direct" ? nil : onBoost)
            MastodonActionButton(symbol: post.favourited ? "star.fill" : "star", label: post.favourited ? "Remove favourite" : "Favourite",
                                 count: post.favourites, active: post.favourited, activeColor: fm.warning, onPressed: onFavourite)
            MastodonActionButton(symbol: post.bookmarked ? "bookmark.fill" : "bookmark", label: post.bookmarked ? "Remove bookmark" : "Bookmark",
                                 count: 0, active: post.bookmarked, activeColor: fm.accent, onPressed: onBookmark)
            Spacer(minLength: 0)
            Menu {
                Button(action: onBrowser) { Label("Open in browser", systemImage: "safari") }
                Button(action: onCopyLink) { Label("Copy link", systemImage: "link") }
                if mine {
                    Button(role: .destructive, action: onDelete) { Label("Delete", systemImage: "trash") }
                }
            } label: {
                Image(systemName: "ellipsis")
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(fm.textSecondary)
                    .frame(minWidth: 44, minHeight: 36)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("More")
        }
        .padding(.top, FmSpace.x1)
    }
}

private struct MastodonActionButton: View {
    @Environment(\.fmPalette) private var fm
    let symbol: String
    let label: String
    let count: Int
    let active: Bool
    let activeColor: Color
    let onPressed: (() -> Void)?

    var body: some View {
        Button(action: {
            FmHaptics.tap()
            onPressed?()
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
            .opacity(onPressed == nil ? 0.4 : 1)
            .frame(minWidth: 40, minHeight: 36, alignment: .leading)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .disabled(onPressed == nil)
        .accessibilityLabel(count > 0 ? "\(label), \(count)" : label)
        .accessibilityAddTraits(active ? .isSelected : [])
    }
}
