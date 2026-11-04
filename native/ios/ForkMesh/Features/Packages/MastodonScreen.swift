import AuthenticationServices
import FMBase
import FMNet
import FMPackages
import SwiftUI

struct MastodonScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var hub: MastodonHub

    init() {
        let store = Services.get(SecureStore.self) ?? KeychainStore()
        let http = Services.get(HTTPClient.self) ?? URLSessionHTTPClient()
        _hub = State(initialValue: MastodonHub(store: store, http: http))
    }

    var body: some View {
        Group {
            if hub.signedIn {
                MastodonHome(hub: hub)
                    .id(hub.generation)
            } else {
                MastodonSignIn(hub: hub)
            }
        }
        .navigationTitle(MastodonHub.title)
        .navigationBarTitleDisplayMode(.inline)
        .background(fm.canvas)
    }
}

private struct MastodonSignIn: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    @Environment(\.webAuthenticationSession) private var webAuthentication
    @Bindable var hub: MastodonHub

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: FmSpace.x3) {
                FmEmptyState(
                    icon: "alternate_email", title: "Sign in to Mastodon",
                    message: "Your server handles the sign-in. ForkMesh keeps the sign-in on this phone only.")
                FmNoticeLine(hub.notice?.message, isError: hub.notice?.isError ?? false, onDismiss: { hub.notice = nil })
                FmCard {
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        Text("Your server")
                            .fmText(.labelLarge, color: fm.textSecondary)
                        FmTextField(text: $hub.server, hint: "mastodon.social", onSubmit: signIn)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                            .keyboardType(.URL)
                            .accessibilityIdentifier("mastodon-server")
                        FmFilledButton("Sign in", fillWidth: true, busy: hub.busy, onPressed: hub.serverValid ? signIn : nil)
                            .accessibilityIdentifier("mastodon-sign-in")
                    }
                }
                if hub.showCode {
                    FmCard {
                        VStack(alignment: .leading, spacing: FmSpace.x2) {
                            Text("Code")
                                .fmText(.labelLarge, color: fm.textSecondary)
                            Text("Approve ForkMesh in the browser, then paste the code it shows (or the whole address).")
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)
                            FmTextField(text: $hub.code, hint: "Code", mono: true)
                                .textInputAutocapitalization(.never)
                                .autocorrectionDisabled()
                                .accessibilityIdentifier("mastodon-code")
                            FmFilledButton("Finish sign-in", fillWidth: true, busy: hub.busy, onPressed: finishAction)
                        }
                    }
                }
                if hub.showToken {
                    FmCard {
                        VStack(alignment: .leading, spacing: FmSpace.x2) {
                            Text("Access token")
                                .fmText(.labelLarge, color: fm.textSecondary)
                            Text("Make one on your server under Preferences → Development, with the read, write and follow scopes.")
                                .fmText(.bodySmall, color: fm.textSecondary)
                                .fixedSize(horizontal: false, vertical: true)
                            FmTextField(text: $hub.token, hint: "Access token", secure: true)
                                .accessibilityIdentifier("mastodon-token")
                            FmFilledButton("Sign in with token", fillWidth: true, busy: hub.busy, onPressed: tokenAction)
                        }
                    }
                }
                HStack(spacing: FmSpace.x2) {
                    if !hub.showCode {
                        FmTextButton("Paste a code instead", onPressed: hub.serverValid ? pasteCode : nil)
                    }
                    if !hub.showToken {
                        FmTextButton("Use an access token instead", onPressed: { hub.showToken = true })
                    }
                }
            }
            .padding(FmSpace.x4)
        }
    }

    private var finishAction: (() -> Void)? {
        guard !hub.code.isEmpty else { return nil }
        let hub = hub
        return { Task { await hub.finish(hub.code) } }
    }

    private var tokenAction: (() -> Void)? {
        guard !hub.token.isEmpty else { return nil }
        let hub = hub
        return { Task { await hub.signIn(token: hub.token) } }
    }

    private func signIn() {
        Task {
            guard let url = await hub.start() else { return }
            do {
                let callback = try await webAuthentication.authenticate(
                    using: url, callbackURLScheme: MastodonOAuth.callbackScheme, preferredBrowserSession: .shared)
                await hub.finish(callback.absoluteString)
            } catch let error as ASWebAuthenticationSessionError where error.code == .canceledLogin {
                return
            } catch {
                hub.notice = MastodonNotice("The browser could not finish the sign-in. Try “Paste a code instead”.", isError: true)
            }
        }
    }

    private func pasteCode() {
        Task {
            guard let url = await hub.start(outOfBand: true) else { return }
            openURL(url)
        }
    }
}

enum MastodonPush: Hashable, Identifiable {
    case thread(String)
    case profile(String)
    case tag(String)
    case search

    var id: String {
        switch self {
        case .thread(let value): return "thread-" + value
        case .profile(let value): return "profile-" + value
        case .tag(let value): return "tag-" + value
        case .search: return "search"
        }
    }
}

struct MastodonCompose: Identifiable {
    let id = UUID()
    let replyTo: MastodonStatus?
}

private struct MastodonHome: View {
    @Environment(\.fmPalette) private var fm
    @Bindable var hub: MastodonHub
    @State private var section: MastodonSection = .home
    @State private var feeds: [MastodonSection: MastodonFeedModel]
    @State private var push: MastodonPush?
    @State private var compose: MastodonCompose?
    @State private var confirmSignOut = false

    init(hub: MastodonHub) {
        self.hub = hub
        _feeds = State(initialValue: Dictionary(uniqueKeysWithValues: MastodonSection.allCases.map {
            ($0, MastodonFeedModel(hub: hub, source: $0.source))
        }))
    }

    private func feed(_ section: MastodonSection) -> MastodonFeedModel {
        feeds[section] ?? MastodonFeedModel(hub: hub, source: section.source)
    }

    var body: some View {
        VStack(spacing: 0) {
            ScrollView(.horizontal, showsIndicators: false) {
                FmSegmentTabBar(
                    tabs: MastodonSection.allCases.map { FmSegmentTab(label: $0.title) },
                    index: MastodonSection.allCases.firstIndex(of: section) ?? 0,
                    onChanged: { (index: Int) in
                        FmHaptics.tap()
                        section = MastodonSection.allCases[index]
                    })
            }
            MastodonFeedList(hub: hub, feed: feed(section), empty: section.empty, push: $push, compose: $compose)
                .id(section)
        }
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                FmIconButton(icon: "search", label: "Search", onPressed: { push = .search })
                FmIconButton(icon: "edit", label: "Write a post", onPressed: { compose = MastodonCompose(replyTo: nil) })
                Menu {
                    Text(hub.session?.handle ?? "")
                    Button(action: { if let id = hub.session?.accountId { push = .profile(id) } }) {
                        Label("Your profile", systemImage: "person.crop.circle")
                    }
                    Button(role: .destructive, action: { confirmSignOut = true }) {
                        Label("Sign out", systemImage: "rectangle.portrait.and.arrow.right")
                    }
                } label: {
                    MastodonAvatar(url: hub.session?.avatar ?? "", name: hub.session?.displayName ?? "", size: 28)
                }
                .accessibilityLabel("Account")
                .accessibilityIdentifier("mastodon-account")
            }
        }
        .task { await hub.refresh() }
        .navigationDestination(item: $push) { (target: MastodonPush) in
            MastodonPushed(hub: hub, target: target)
        }
        .sheet(item: $compose) { (target: MastodonCompose) in
            MastodonComposeSheet(hub: hub, replyTo: target.replyTo) { (status: MastodonStatus) in
                feeds[.home]?.posted(status)
                feed(section).notice = MastodonNotice("Posted.")
            }
        }
        .fmDialog(isPresented: $confirmSignOut) {
            FmConfirmDialog(
                title: "Sign out of Mastodon?",
                message: "ForkMesh forgets this sign-in and asks \(MastodonServer.host(hub.session?.server ?? "")) to revoke it.",
                confirmLabel: "Sign out",
                destructive: true,
                onCancel: { confirmSignOut = false },
                onConfirm: {
                    confirmSignOut = false
                    Task { await hub.signOut() }
                })
        }
    }
}

struct MastodonPushed: View {
    let hub: MastodonHub
    let target: MastodonPush

    var body: some View {
        switch target {
        case .thread(let id):
            MastodonFeedPage(hub: hub, source: .thread(id), title: "Post", empty: "This post could not be found.")
        case .tag(let name):
            MastodonFeedPage(hub: hub, source: .timeline(.tag(name)), title: "#" + name, empty: "No posts with #\(name) yet.")
        case .profile(let id):
            MastodonProfileScreen(hub: hub, accountId: id)
        case .search:
            MastodonSearchScreen(hub: hub)
        }
    }
}

struct MastodonFeedPage: View {
    @Environment(\.fmPalette) private var fm
    let hub: MastodonHub
    let title: String
    let empty: String
    @State private var feed: MastodonFeedModel
    @State private var push: MastodonPush?
    @State private var compose: MastodonCompose?

    init(hub: MastodonHub, source: MastodonSource, title: String, empty: String) {
        self.hub = hub
        self.title = title
        self.empty = empty
        _feed = State(initialValue: MastodonFeedModel(hub: hub, source: source))
    }

    var body: some View {
        MastodonFeedList(hub: hub, feed: feed, empty: empty, push: $push, compose: $compose)
            .navigationTitle(title)
            .navigationBarTitleDisplayMode(.inline)
            .background(fm.canvas)
            .navigationDestination(item: $push) { (target: MastodonPush) in
                MastodonPushed(hub: hub, target: target)
            }
            .sheet(item: $compose) { (target: MastodonCompose) in
                MastodonComposeSheet(hub: hub, replyTo: target.replyTo) { (status: MastodonStatus) in
                    feed.posted(status)
                    feed.notice = MastodonNotice("Posted.")
                }
            }
    }
}
