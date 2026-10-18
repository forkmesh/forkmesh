import FMChat
import FMPackages
import SafariServices
import SwiftUI
import WebKit

struct RSSReaderArticleScreen: View {
    let model: RSSReaderModel
    @State private var scope: RSSScope
    @State private var list: [RSSArticle]
    @State private var articleId: String
    @State private var reader = false
    @State private var loadingReader = false
    @State private var readerFailed = false
    @State private var browser: RSSBrowserLink?
    @State private var notice = ""
    @State private var pageDirection = 1
    @State private var chatShare: RSSChatShare?
    @Environment(\.openURL) private var openURL

    init(model: RSSReaderModel, scope: RSSScope, list: [RSSArticle], articleId: String) {
        self.model = model
        _scope = State(initialValue: scope)
        _list = State(initialValue: list)
        _articleId = State(initialValue: articleId)
    }

    private var article: RSSArticle? { model.article(articleId) }

    private var index: Int? { list.firstIndex { $0.id == articleId } }

    private var link: URL? {
        guard let article else { return nil }
        let text: String = RSSURL.http(article.link)
        return text.isEmpty ? nil : URL(string: text)
    }

    var body: some View {
        content
            .bareShellTop()
            .navigationTitle("")
            .navigationBarTitleDisplayMode(.inline)
            .modifier(RSSUnreadCountToolbar(count: model.unread(scope)))
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    HStack(spacing: 0) {
                        Button {
                            move(-1)
                        } label: {
                            Image(systemName: "chevron.up").frame(width: 36, height: 32)
                        }
                        .disabled((index ?? 0) <= 0)
                        .accessibilityLabel("Previous Article")
                        Button {
                            move(1)
                        } label: {
                            Image(systemName: "chevron.down").frame(width: 36, height: 32)
                        }
                        .disabled((index ?? list.count) >= list.count - 1)
                        .accessibilityLabel("Next Article")
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    Menu {
                        Button {
                            model.toggleReadLater(articleId)
                        } label: {
                            if article?.readLater ?? false {
                                Label("Remove from Read Later", systemImage: "bookmark.slash")
                            } else {
                                Label("Read Later", systemImage: "bookmark")
                            }
                        }
                        .disabled(article == nil)
                    } label: {
                        Image(systemName: "ellipsis.circle")
                    }
                    .accessibilityLabel("More")
                    .accessibilityIdentifier("rss-article-more")
                }
            }
            .safeAreaInset(edge: .bottom, spacing: 0) { toolbar }
            .overlay(alignment: .top) { noticeView }
            .onAppear { model.open(articleId) }
            .onChange(of: articleId) { (_: String, id: String) in
                reader = false
                model.open(id)
            }
            .sheet(item: $browser) { (target: RSSBrowserLink) in
                RSSSafariView(url: target.url)
                    .ignoresSafeArea()
            }
            .sheet(item: $chatShare) { (share: RSSChatShare) in
                if let hub = ChatFeature.hub() {
                    RSSReaderShareSheet(hub: hub, share: share) { (place: String) in
                        withAnimation { notice = "Sent to \(place)." }
                    }
                }
            }
            .alert("Reader View", isPresented: $readerFailed) {
                if let link {
                    Button("Open Original") { openLink(link) }
                }
                Button("OK", role: .cancel) {}
            } message: {
                Text(RSSReaderText.readerFailed)
            }
            .rssPalette(model.settings.palette)
    }

    @ViewBuilder private var content: some View {
        ZStack {
            if let article {
                RSSArticleWebView(
                    html: model.page(for: article, reader: reader), articleKey: "\(article.id)|\(reader)",
                    onLink: { (url: URL) in openLink(url) },
                    canSwipe: { (step: Int) in canMove(step) },
                    onSwipe: { (step: Int) in move(step) })
                    .ignoresSafeArea(edges: .bottom)
                    .accessibilityIdentifier("rss-article-body")
                    .id(article.id)
                    .transition(pageTransition)
            } else {
                Text("This article is no longer on this iPhone.")
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 12).onEnded { value in
                            guard abs(value.translation.width) > abs(value.translation.height) else { return }
                            move(value.translation.width < 0 ? 1 : -1)
                        })
            }
        }
        .clipped()
    }

    private var pageTransition: AnyTransition {
        let entering: Edge = pageDirection > 0 ? .trailing : .leading
        let leaving: Edge = pageDirection > 0 ? .leading : .trailing
        return .asymmetric(
            insertion: .move(edge: entering).combined(with: .opacity),
            removal: .move(edge: leaving).combined(with: .opacity))
    }

    @ViewBuilder private var noticeView: some View {
        if !notice.isEmpty {
            Text(notice)
                .font(.subheadline.weight(.medium))
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .rssGlass(Capsule())
                .padding(.top, 8)
                .transition(.move(edge: .top).combined(with: .opacity))
                .task {
                    try? await Task.sleep(nanoseconds: 2_000_000_000)
                    withAnimation { notice = "" }
                }
        }
    }

    private var toolbar: some View {
        HStack(spacing: 12) {
            HStack(spacing: 0) {
                Button {
                    model.toggleRead(articleId)
                } label: {
                    Image(systemName: article?.read ?? true ? "circle" : "circle.fill")
                        .frame(width: 46, height: 46)
                }
                .accessibilityLabel(article?.read ?? true ? "Mark as Unread" : "Mark as Read")
                Button {
                    model.toggleStar(articleId)
                } label: {
                    Image(systemName: article?.starred ?? false ? "star.fill" : "star")
                        .frame(width: 46, height: 46)
                }
                .accessibilityLabel(article?.starred ?? false ? "Mark as Unstarred" : "Mark as Starred")
            }
            .rssGlass(Capsule())
            Spacer(minLength: 0)
            Button {
                nextUnread()
            } label: {
                Image(systemName: "chevron.down.circle")
                    .frame(width: 46, height: 46)
            }
            .rssGlass(Circle())
            .accessibilityLabel("Next Unread")
            .accessibilityIdentifier("rss-next-unread")
            Spacer(minLength: 0)
            HStack(spacing: 0) {
                Button {
                    toggleReader()
                } label: {
                    Group {
                        if loadingReader {
                            ProgressView()
                        } else {
                            Image(systemName: reader ? "doc.plaintext.fill" : "doc.plaintext")
                        }
                    }
                    .frame(width: 46, height: 46)
                }
                .disabled(link == nil || loadingReader)
                .accessibilityLabel(reader ? "Show Feed Content" : "Reader View")
                Button {
                    shareToChat()
                } label: {
                    Image(systemName: "paperplane")
                        .frame(width: 46, height: 46)
                }
                .disabled(link == nil || ChatFeature.hub() == nil)
                .accessibilityLabel("Send to Chat")
                .accessibilityIdentifier("rss-share-chat")
                if let link {
                    ShareLink(item: link) {
                        Image(systemName: "square.and.arrow.up")
                            .frame(width: 46, height: 46)
                    }
                    .accessibilityLabel("Share")
                } else {
                    Image(systemName: "square.and.arrow.up")
                        .frame(width: 46, height: 46)
                        .foregroundStyle(.tertiary)
                        .accessibilityHidden(true)
                }
            }
            .rssGlass(Capsule())
        }
        .font(.system(size: 18, weight: .medium))
        .buttonStyle(.plain)
        .padding(.horizontal, 16)
        .padding(.bottom, 8)
    }

    private func canMove(_ step: Int) -> Bool {
        guard let index else { return false }
        let next: Int = index + step
        return next >= 0 && next < list.count
    }

    private func move(_ step: Int) {
        guard let index else { return }
        let next: Int = index + step
        guard next >= 0 && next < list.count else { return }
        pageDirection = step
        withAnimation(.easeInOut(duration: 0.24)) {
            articleId = list[next].id
        }
    }

    private func nextUnread() {
        guard let found = model.nextUnread(after: articleId, in: list, scope: scope) else {
            withAnimation { notice = RSSReaderText.caughtUp }
            return
        }
        if found.scope != scope {
            scope = found.scope
            list = model.timeline(found.scope)
        }
        pageDirection = 1
        withAnimation(.easeInOut(duration: 0.24)) {
            articleId = found.article.id
        }
    }

    private func toggleReader() {
        if reader {
            reader = false
            return
        }
        guard let article else { return }
        if !model.body(for: article).readerHtml.isEmpty {
            reader = true
            return
        }
        loadingReader = true
        let id: String = article.id
        Task {
            let ok: Bool = await model.loadReader(id)
            loadingReader = false
            if id == articleId {
                if ok { reader = true } else { readerFailed = true }
            }
        }
    }

    private func shareToChat() {
        guard let article, let link else { return }
        let feed: String = model.feed(article.feedId)?.displayTitle ?? ""
        chatShare = RSSChatShare(title: article.title, link: link.absoluteString, feed: feed)
    }

    private func openLink(_ url: URL) {
        let scheme: String = url.scheme?.lowercased() ?? ""
        if (scheme == "http" || scheme == "https") && model.settings.openLinksInApp {
            browser = RSSBrowserLink(url: url)
        } else if scheme == "http" || scheme == "https" || scheme == "mailto" {
            openURL(url)
        }
    }
}

private struct RSSUnreadCountToolbar: ViewModifier {
    let count: Int

    func body(content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.toolbar {
                countItem.sharedBackgroundVisibility(.hidden)
            }
        } else {
            content.toolbar {
                countItem
            }
        }
        #else
        content.toolbar {
            countItem
        }
        #endif
    }

    private var countItem: some ToolbarContent {
        ToolbarItem(placement: .topBarLeading) {
            RSSCountLabel(count: count)
                .accessibilityLabel("\(count.formatted()) unread")
                .accessibilityIdentifier("rss-article-unread-count")
        }
    }
}

struct RSSBrowserLink: Identifiable {
    let url: URL

    var id: String { url.absoluteString }
}

struct RSSSafariView: UIViewControllerRepresentable {
    let url: URL

    func makeUIViewController(context: Context) -> SFSafariViewController {
        let configuration = SFSafariViewController.Configuration()
        configuration.entersReaderIfAvailable = false
        return SFSafariViewController(url: url, configuration: configuration)
    }

    func updateUIViewController(_ controller: SFSafariViewController, context: Context) {}
}

struct RSSArticleWebView: UIViewRepresentable {
    let html: String
    let articleKey: String
    let onLink: (URL) -> Void
    let canSwipe: (Int) -> Bool
    let onSwipe: (Int) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(onLink: onLink, canSwipe: canSwipe, onSwipe: onSwipe) }

    func makeUIView(context: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.defaultWebpagePreferences.allowsContentJavaScript = false
        configuration.preferences.javaScriptCanOpenWindowsAutomatically = false
        configuration.websiteDataStore = .nonPersistent()
        configuration.allowsInlineMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = .all
        configuration.dataDetectorTypes = []
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.navigationDelegate = context.coordinator
        view.uiDelegate = context.coordinator
        view.isOpaque = false
        view.backgroundColor = .clear
        view.scrollView.backgroundColor = .clear
        view.scrollView.contentInsetAdjustmentBehavior = .automatic
        view.scrollView.alwaysBounceHorizontal = false
        view.scrollView.showsHorizontalScrollIndicator = false
        view.scrollView.isDirectionalLockEnabled = true
        view.scrollView.delegate = context.coordinator
        let swipe = UIPanGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.swiped(_:)))
        swipe.delegate = context.coordinator
        view.addGestureRecognizer(swipe)
        view.scrollView.panGestureRecognizer.require(toFail: swipe)
        context.coordinator.load(html, key: articleKey, into: view)
        return view
    }

    func updateUIView(_ view: WKWebView, context: Context) {
        context.coordinator.onLink = onLink
        context.coordinator.canSwipe = canSwipe
        context.coordinator.onSwipe = onSwipe
        context.coordinator.load(html, key: articleKey, into: view)
    }

    final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate, UIGestureRecognizerDelegate, UIScrollViewDelegate {
        var onLink: (URL) -> Void
        var canSwipe: (Int) -> Bool
        var onSwipe: (Int) -> Void
        private var html = ""
        private var key = ""

        init(onLink: @escaping (URL) -> Void, canSwipe: @escaping (Int) -> Bool, onSwipe: @escaping (Int) -> Void) {
            self.onLink = onLink
            self.canSwipe = canSwipe
            self.onSwipe = onSwipe
        }

        func gestureRecognizerShouldBegin(_ gesture: UIGestureRecognizer) -> Bool {
            guard let pan = gesture as? UIPanGestureRecognizer, let view = pan.view else { return true }
            let moved: CGPoint = pan.translation(in: view.superview ?? view)
            if moved.x != 0 || moved.y != 0 { return abs(moved.x) > abs(moved.y) }
            let speed: CGPoint = pan.velocity(in: view.superview ?? view)
            return abs(speed.x) > abs(speed.y)
        }

        private func offset(_ moved: CGFloat) -> CGFloat {
            let step: Int = moved < 0 ? 1 : -1
            return canSwipe(step) ? moved : moved * 0.25
        }

        @objc func swiped(_ pan: UIPanGestureRecognizer) {
            guard let view = pan.view else { return }
            let space: UIView = view.superview ?? view
            let moved: CGFloat = pan.translation(in: space).x
            switch pan.state {
            case .began, .changed:
                view.transform = CGAffineTransform(translationX: offset(moved), y: 0)
            case .ended:
                let speed: CGFloat = pan.velocity(in: space).x
                let step: Int = moved < 0 ? 1 : -1
                let far: Bool = abs(moved) > max(60, view.bounds.width * 0.22)
                let flung: Bool = abs(speed) > 500 && (speed < 0) == (moved < 0) && abs(moved) > 12
                if (far || flung) && canSwipe(step) {
                    onSwipe(step)
                } else {
                    settle(view)
                }
            default:
                settle(view)
            }
        }

        private func settle(_ view: UIView) {
            UIView.animate(
                withDuration: 0.28, delay: 0, usingSpringWithDamping: 0.9, initialSpringVelocity: 0, options: [.allowUserInteraction, .beginFromCurrentState]
            ) {
                view.transform = .identity
            }
        }

        func scrollViewDidScroll(_ scrollView: UIScrollView) {
            guard scrollView.zoomScale <= scrollView.minimumZoomScale + 0.001 else { return }
            let left: CGFloat = -scrollView.adjustedContentInset.left
            if scrollView.contentOffset.x != left { scrollView.contentOffset.x = left }
        }

        func load(_ html: String, key: String, into view: WKWebView) {
            guard html != self.html || key != self.key else { return }
            let moved: Bool = key != self.key
            self.html = html
            self.key = key
            view.loadHTMLString(html, baseURL: nil)
            if moved { view.scrollView.setContentOffset(.zero, animated: false) }
        }

        func webView(
            _ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
            decisionHandler: @escaping (WKNavigationActionPolicy) -> Void
        ) {
            let url: URL? = navigationAction.request.url
            let scheme: String = url?.scheme?.lowercased() ?? ""
            if navigationAction.navigationType == .other && (scheme == "about" || scheme.isEmpty) {
                decisionHandler(.allow)
                return
            }
            if navigationAction.navigationType == .linkActivated || navigationAction.targetFrame == nil, let url {
                if scheme == "http" || scheme == "https" || scheme == "mailto" { onLink(url) }
            }
            decisionHandler(.cancel)
        }

        func webView(
            _ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
            for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures
        ) -> WKWebView? {
            if let url = navigationAction.request.url {
                let scheme: String = url.scheme?.lowercased() ?? ""
                if scheme == "http" || scheme == "https" || scheme == "mailto" { onLink(url) }
            }
            return nil
        }
    }
}
