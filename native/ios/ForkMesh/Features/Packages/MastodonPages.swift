import FMBase
import FMPackages
import PhotosUI
import SwiftUI
import UIKit

struct MastodonProfileScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let hub: MastodonHub
    @State private var profile: MastodonProfileModel
    @State private var push: MastodonPush?
    @State private var compose: MastodonCompose?

    init(hub: MastodonHub, accountId: String) {
        self.hub = hub
        _profile = State(initialValue: MastodonProfileModel(hub: hub, accountId: accountId))
    }

    var body: some View {
        MastodonFeedList(hub: hub, feed: profile.feed, empty: "No posts yet.", push: $push, compose: $compose, header: AnyView(header))
            .task { if profile.account == nil { await profile.load() } }
            .navigationTitle(profile.account.map { "@" + $0.acct } ?? "Profile")
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .navigationDestination(item: $push) { (target: MastodonPush) in
                MastodonPushed(hub: hub, target: target)
            }
            .sheet(item: $compose) { (target: MastodonCompose) in
                MastodonComposeSheet(hub: hub, replyTo: target.replyTo) { (status: MastodonStatus) in
                    profile.feed.posted(status)
                }
            }
    }

    private func flags(_ account: MastodonAccount) -> String {
        var parts: [String] = []
        if account.locked { parts.append("Approves followers") }
        if account.bot { parts.append("Automated") }
        return parts.joined(separator: " · ")
    }

    @ViewBuilder private var header: some View {
        if let account = profile.account {
            VStack(alignment: .leading, spacing: FmSpace.x2) {
                HStack(alignment: .center, spacing: FmSpace.x3) {
                    MastodonAvatar(url: account.avatar, name: account.displayName, size: 64)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(account.displayName)
                            .fmText(.titleMedium)
                            .lineLimit(2)
                        Text(account.handle(server: hub.session?.server ?? ""))
                            .fmText(.bodySmall, color: fm.textSecondary)
                            .lineLimit(1)
                            .textSelection(.enabled)
                        if account.locked || account.bot {
                            Text(flags(account))
                                .fmText(.labelSmall, color: fm.textTertiary)
                        }
                    }
                    Spacer(minLength: 0)
                }
                let bio = MastodonHTML.segments(account.note)
                if !bio.isEmpty {
                    Text(MastodonLinks.body(bio, link: fm.accent))
                        .fmText(.bodyMedium)
                        .tint(fm.accent)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Text("\(account.statuses) posts · \(account.following) following · \(account.followers) followers")
                    .fmText(.bodySmall, color: fm.textSecondary, tabular: true)
                HStack(spacing: FmSpace.x2) {
                    if !profile.isMe {
                        FmFilledButton(profile.followLabel, busy: profile.following, onPressed: {
                            Task { await profile.toggleFollow() }
                        })
                        .accessibilityIdentifier("mastodon-follow")
                    }
                    if let url = URL(string: account.url), !account.url.isEmpty {
                        FmOutlinedButton("Open in browser", onPressed: { openURL(url) })
                    }
                }
            }
            .padding(.vertical, FmSpace.x2)
        } else if !profile.error.isEmpty {
            FmNoticeLine(profile.error, isError: true)
        }
    }
}

struct MastodonSearchScreen: View {
    @Environment(\.fmPalette) private var fm
    let hub: MastodonHub
    @State private var search: MastodonSearchModel
    @State private var push: MastodonPush?

    init(hub: MastodonHub) {
        self.hub = hub
        _search = State(initialValue: MastodonSearchModel(hub: hub))
    }

    var body: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 0) {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    FmTextField(text: $search.query, hint: "People, #hashtags, posts or addresses", autofocus: true, onSubmit: run)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()
                        .submitLabel(.search)
                        .accessibilityIdentifier("mastodon-search")
                    if search.searching { ProgressView().tint(fm.accent) }
                    if !search.error.isEmpty { FmNoticeLine(search.error, isError: true) }
                }
                .padding(FmSpace.x4)
                results
            }
        }
        .navigationTitle("Search")
        .navigationBarTitleDisplayMode(.inline)
        .background(fm.canvas)
        .navigationDestination(item: $push) { (target: MastodonPush) in
            MastodonPushed(hub: hub, target: target)
        }
    }

    private func run() {
        Task { await search.run() }
    }

    @ViewBuilder private var results: some View {
        let found = search.results
        if !search.searched.isEmpty && found.isEmpty && !search.searching {
            FmEmptyState(icon: "search", title: "No results", message: "Nothing on your server matches “\(search.searched)”.")
                .padding(FmSpace.x4)
        }
        if !found.accounts.isEmpty {
            FmSectionHeader(title: "People")
            ForEach(found.accounts) { (account: MastodonAccount) in
                Button(action: { push = .profile(account.id) }) {
                    HStack(spacing: FmSpace.x3) {
                        MastodonAvatar(url: account.avatar, name: account.displayName, size: 36)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(account.displayName).fmText(.titleSmall).lineLimit(1)
                            Text("@" + account.acct).fmText(.bodySmall, color: fm.textSecondary).lineLimit(1)
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
        if !found.hashtags.isEmpty {
            FmSectionHeader(title: "Hashtags")
            ForEach(found.hashtags, id: \.self) { (tag: String) in
                Button(action: { push = .tag(tag.lowercased()) }) {
                    HStack {
                        Text("#" + tag).fmText(.titleSmall)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
        if !found.statuses.isEmpty {
            FmSectionHeader(title: "Posts")
            ForEach(found.statuses) { (status: MastodonStatus) in
                Button(action: { push = .thread(status.id) }) {
                    HStack(alignment: .top, spacing: FmSpace.x3) {
                        MastodonAvatar(url: status.account.avatar, name: status.account.displayName, size: 32)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(status.account.displayName).fmText(.titleSmall).lineLimit(1)
                            Text(status.spoiler.isEmpty ? status.plainText : status.spoiler)
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .lineLimit(3)
                        }
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, FmSpace.x4)
                    .padding(.vertical, FmSpace.x2)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }
}

struct MastodonComposeSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var composer: MastodonComposer
    @State private var picked: [PhotosPickerItem] = []
    let onPosted: (MastodonStatus) -> Void

    init(hub: MastodonHub, replyTo: MastodonStatus?, onPosted: @escaping (MastodonStatus) -> Void) {
        _composer = State(initialValue: MastodonComposer(hub: hub, replyTo: replyTo))
        self.onPosted = onPosted
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    if !composer.error.isEmpty { FmNoticeLine(composer.error, isError: true) }
                    if let parent = composer.replyTo {
                        Text("Replying to @\(parent.account.acct)")
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    if composer.warning {
                        TextField("Content warning", text: $composer.spoiler)
                            .fmText(.bodyMedium)
                            .accessibilityIdentifier("mastodon-compose-spoiler")
                        FmHairline()
                    }
                    TextField(composer.replyTo == nil ? "What’s on your mind?" : "Write your reply", text: $composer.text, axis: .vertical)
                        .fmText(.bodyLarge)
                        .lineLimit(4...14)
                        .accessibilityIdentifier("mastodon-compose-text")
                    if !composer.media.isEmpty { thumbnails }
                    HStack(spacing: FmSpace.x2) {
                        PhotosPicker(selection: $picked, maxSelectionCount: max(1, composer.maxMedia - composer.media.count), matching: .images) {
                            Image(systemName: "photo.on.rectangle")
                                .font(.system(size: 17, weight: .medium))
                                .foregroundStyle(composer.media.count >= composer.maxMedia ? fm.textDisabled : fm.textSecondary)
                                .frame(minWidth: 44, minHeight: 44)
                        }
                        .disabled(composer.media.count >= composer.maxMedia)
                        .accessibilityLabel("Add pictures")
                        Button(action: { composer.warning.toggle() }) {
                            Image(systemName: "exclamationmark.triangle")
                                .font(.system(size: 17, weight: .medium))
                                .foregroundStyle(composer.warning ? fm.accent : fm.textSecondary)
                                .frame(minWidth: 44, minHeight: 44)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel(composer.warning ? "Remove content warning" : "Content warning")
                        Menu {
                            ForEach(MastodonComposer.visibilities) { (option: MastodonVisibility) in
                                Button(action: { composer.visibility = option.value }) {
                                    if composer.visibility == option.value {
                                        Label(option.label, systemImage: "checkmark")
                                    } else {
                                        Text(option.label)
                                    }
                                }
                            }
                        } label: {
                            Text(composer.visibilityLabel)
                                .fmText(.labelLarge, color: fm.accent)
                                .frame(minHeight: 44)
                        }
                        .accessibilityLabel("Who can see this: \(composer.visibilityLabel)")
                        Spacer(minLength: 0)
                        Text("\(composer.remaining)")
                            .fmText(.labelMedium, color: composer.remaining < 0 ? fm.danger : fm.textTertiary, tabular: true)
                            .accessibilityLabel("\(composer.remaining) characters left")
                    }
                }
                .padding(FmSpace.x4)
            }
            .background(fm.canvas)
            .navigationTitle(composer.replyTo == nil ? "New post" : "Reply")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    FmFilledButton(composer.posting ? "Posting…" : "Post", busy: composer.posting, onPressed: composer.canPost ? post : nil)
                        .accessibilityIdentifier("mastodon-compose-post")
                }
            }
            .onChange(of: picked) { (_: [PhotosPickerItem], items: [PhotosPickerItem]) in
                guard !items.isEmpty else { return }
                picked = []
                Task { await load(items) }
            }
        }
        .presentationDetents([.large])
    }

    private var thumbnails: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: FmSpace.x2) {
                ForEach(composer.media) { (item: MastodonPicked) in
                    ZStack(alignment: .topTrailing) {
                        if let image = UIImage(data: item.upload.data) {
                            Image(uiImage: image)
                                .resizable()
                                .scaledToFill()
                                .frame(width: 88, height: 88)
                                .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                        }
                        Button(action: { composer.remove(item.id) }) {
                            Image(systemName: "xmark.circle.fill")
                                .font(.system(size: 20))
                                .foregroundStyle(.white, .black.opacity(0.6))
                                .frame(minWidth: 32, minHeight: 32)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Remove picture")
                    }
                }
            }
        }
    }

    private func load(_ items: [PhotosPickerItem]) async {
        for item in items {
            guard composer.media.count < composer.maxMedia,
                  let data = try? await item.loadTransferable(type: Data.self),
                  let image = UIImage(data: data),
                  let jpeg = image.jpegData(compressionQuality: 0.85) else { continue }
            composer.add(MastodonUpload(data: jpeg, filename: "photo.jpg", mimeType: "image/jpeg"))
        }
    }

    private func post() {
        Task {
            guard let status = await composer.submit() else { return }
            FmHaptics.send()
            onPosted(status)
            dismiss()
        }
    }
}
