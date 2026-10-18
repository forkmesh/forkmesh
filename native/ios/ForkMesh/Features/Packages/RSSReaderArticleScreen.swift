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
        if let article {
            RSSArticlePager(
                previous: neighbor(-1), current: page(article, reader: reader), next: neighbor(1), direction: pageDirection,
                onLink: { (url: URL) in openLink(url) },
                onPage: { (step: Int) in move(step) })
                .ignoresSafeArea(edges: .bottom)
                .accessibilityIdentifier("rss-article-body")
        } else {
            GeometryReader { (proxy: GeometryProxy) in
                Text("This article is no longer on this iPhone.")
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .contentShape(Rectangle())
                    .onTapGesture { (point: CGPoint) in
                        move(point.x < proxy.size.width / 2 ? -1 : 1)
                    }
                    .gesture(
                        DragGesture(minimumDistance: 12).onEnded { value in
                            guard abs(value.translation.width) > abs(value.translation.height) else { return }
                            move(value.translation.width < 0 ? 1 : -1)
                        })
            }
        }
    }

    private func page(_ article: RSSArticle, reader: Bool) -> RSSPagerPage {
        RSSPagerPage(id: article.id, key: "\(article.id)|\(reader)", html: model.page(for: article, reader: reader))
    }

    private func neighbor(_ step: Int) -> RSSPagerPage? {
        guard let index else { return nil }
        let next: Int = index + step
        guard next >= 0 && next < list.count, let article = model.article(list[next].id) else { return nil }
        return page(article, reader: false)
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
        articleId = list[next].id
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
        articleId = found.article.id
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


struct RSSPagerPage: Equatable {
    let id: String
    let key: String
    let html: String
}

final class RSSPagerView: UIView {
    var onLayout: (() -> Void)?

    override func layoutSubviews() {
        super.layoutSubviews()
        onLayout?()
    }
}

struct RSSArticlePager: UIViewRepresentable {
    let previous: RSSPagerPage?
    let current: RSSPagerPage
    let next: RSSPagerPage?
    let direction: Int
    let onLink: (URL) -> Void
    let onPage: (Int) -> Void

    private var pages: [Int: RSSPagerPage] {
        var pages: [Int: RSSPagerPage] = [0: current]
        if let previous { pages[-1] = previous }
        if let next { pages[1] = next }
        return pages
    }

    func makeCoordinator() -> Coordinator { Coordinator(onLink: onLink, onPage: onPage) }

    func makeUIView(context: Context) -> RSSPagerView {
        let view = RSSPagerView()
        view.clipsToBounds = true
        view.backgroundColor = .clear
        context.coordinator.attach(view)
        context.coordinator.show(pages, direction: direction)
        return view
    }

    func updateUIView(_ view: RSSPagerView, context: Context) {
        context.coordinator.onLink = onLink
        context.coordinator.onPage = onPage
        context.coordinator.show(pages, direction: direction)
    }

    final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate, UIGestureRecognizerDelegate, UIScrollViewDelegate {
        var onLink: (URL) -> Void
        var onPage: (Int) -> Void
        private weak var host: RSSPagerView?
        private let configuration: WKWebViewConfiguration
        private let pan = UIPanGestureRecognizer()
        private let tap = UITapGestureRecognizer()
        private var views: [String: WKWebView] = [:]
        private var slots: [String: Int] = [:]
        private var loaded: [String: RSSPagerPage] = [:]
        private var wanted: [Int: RSSPagerPage] = [:]
        private var currentId = ""
        private var heading = ""
        private var shift: CGFloat = 0
        private var animator: UIViewPropertyAnimator?
        private var finish: (() -> Void)?
        private var taps = 0
        private var linkOpened = false

        init(onLink: @escaping (URL) -> Void, onPage: @escaping (Int) -> Void) {
            self.onLink = onLink
            self.onPage = onPage
            let configuration = WKWebViewConfiguration()
            configuration.defaultWebpagePreferences.allowsContentJavaScript = false
            configuration.preferences.javaScriptCanOpenWindowsAutomatically = false
            configuration.websiteDataStore = .nonPersistent()
            configuration.allowsInlineMediaPlayback = true
            configuration.mediaTypesRequiringUserActionForPlayback = .all
            configuration.dataDetectorTypes = []
            self.configuration = configuration
            super.init()
        }

        func attach(_ view: RSSPagerView) {
            host = view
            view.onLayout = { [weak self] in self?.layout() }
            pan.addTarget(self, action: #selector(panned(_:)))
            pan.delegate = self
            view.addGestureRecognizer(pan)
            tap.addTarget(self, action: #selector(tapped(_:)))
            tap.delegate = self
            tap.cancelsTouchesInView = false
            view.addGestureRecognizer(tap)
        }

        func show(_ pages: [Int: RSSPagerPage], direction: Int) {
            wanted = pages
            guard let current = pages[0], let host else { return }
            if animator != nil {
                if current.id == heading || current.id == currentId { return }
                finishAnimation()
            }
            guard !currentId.isEmpty, current.id != currentId, host.bounds.width > 0 else {
                reconcile()
                return
            }
            let known: Int = slots[current.id] ?? 0
            let step: Int = known != 0 ? (known > 0 ? 1 : -1) : (direction < 0 ? -1 : 1)
            if known != step {
                for (id, slot) in slots where slot == step { drop(id) }
                place(current, slot: step)
                layout()
            }
            turn(step, speed: 0, notify: false)
        }

        private func reconcile() {
            guard animator == nil else { return }
            for (slot, page) in wanted { place(page, slot: slot) }
            let keep = Set(wanted.values.map(\.id))
            for id in views.keys where !keep.contains(id) { drop(id) }
            currentId = wanted[0]?.id ?? ""
            shift = 0
            layout()
        }

        private func place(_ page: RSSPagerPage, slot: Int) {
            let view: WKWebView = views[page.id] ?? make(page.id)
            slots[page.id] = slot
            let before: RSSPagerPage? = loaded[page.id]
            guard before != page else { return }
            loaded[page.id] = page
            view.loadHTMLString(page.html, baseURL: nil)
            if before?.key != page.key {
                view.scrollView.setContentOffset(CGPoint(x: 0, y: -view.scrollView.adjustedContentInset.top), animated: false)
            }
        }

        private func make(_ id: String) -> WKWebView {
            let view = WKWebView(frame: host?.bounds ?? .zero, configuration: configuration)
            view.navigationDelegate = self
            view.uiDelegate = self
            view.isOpaque = false
            view.backgroundColor = .clear
            view.scrollView.backgroundColor = .clear
            view.scrollView.contentInsetAdjustmentBehavior = .automatic
            view.scrollView.alwaysBounceHorizontal = false
            view.scrollView.showsHorizontalScrollIndicator = false
            view.scrollView.isDirectionalLockEnabled = true
            view.scrollView.delegate = self
            view.scrollView.panGestureRecognizer.require(toFail: pan)
            host?.addSubview(view)
            views[id] = view
            return view
        }

        private func drop(_ id: String) {
            views[id]?.scrollView.delegate = nil
            views[id]?.removeFromSuperview()
            views[id] = nil
            slots[id] = nil
            loaded[id] = nil
        }

        private func layout() {
            guard let host else { return }
            let size: CGSize = host.bounds.size
            for (id, view) in views {
                let frame = CGRect(x: CGFloat(slots[id] ?? 0) * size.width + shift, y: 0, width: size.width, height: size.height)
                if view.frame != frame { view.frame = frame }
            }
        }

        private func has(_ step: Int) -> Bool { slots.values.contains(step) }

        private var page: WKWebView? { views[currentId] }

        private func offset(_ moved: CGFloat) -> CGFloat {
            has(moved < 0 ? 1 : -1) ? moved : moved * 0.25
        }

        private func turn(_ step: Int, speed: CGFloat, notify: Bool) {
            guard let host, let target = slots.first(where: { $0.value == step })?.key else { return }
            heading = target
            animate(to: -CGFloat(step) * host.bounds.width, speed: speed, duration: 0.34) { [weak self] in
                guard let self else { return }
                for (id, slot) in self.slots { self.slots[id] = slot - step }
                for (id, slot) in self.slots where abs(slot) > 1 { self.drop(id) }
                self.currentId = target
                self.shift = 0
                self.layout()
                if notify {
                    self.onPage(step)
                } else {
                    self.reconcile()
                }
            }
        }

        private func settle(speed: CGFloat) {
            heading = currentId
            animate(to: 0, speed: speed, duration: 0.3) {}
        }

        private func animate(to target: CGFloat, speed: CGFloat, duration: TimeInterval, then done: @escaping () -> Void) {
            let distance: CGFloat = target - shift
            let relative: CGFloat = abs(distance) < 1 ? 0 : max(-30, min(30, speed / distance))
            let timing = UISpringTimingParameters(dampingRatio: 1, initialVelocity: CGVector(dx: relative, dy: 0))
            let animator = UIViewPropertyAnimator(duration: duration, timingParameters: timing)
            animator.addAnimations { [weak self] in
                self?.shift = target
                self?.layout()
            }
            animator.addCompletion { [weak self] (_: UIViewAnimatingPosition) in
                guard let self, self.animator === animator else { return }
                self.complete()
            }
            finish = done
            self.animator = animator
            animator.startAnimation()
        }

        private func complete() {
            animator = nil
            let done = finish
            finish = nil
            done?()
        }

        private func finishAnimation() {
            guard let running = animator else { return }
            running.stopAnimation(true)
            complete()
            layout()
        }

        @objc func panned(_ pan: UIPanGestureRecognizer) {
            guard let host else { return }
            let moved: CGFloat = pan.translation(in: host).x
            switch pan.state {
            case .began:
                finishAnimation()
                shift = offset(moved)
                layout()
            case .changed:
                shift = offset(moved)
                layout()
            case .ended:
                let speed: CGFloat = pan.velocity(in: host).x
                let step: Int = moved < 0 ? 1 : -1
                let far: Bool = abs(moved) > max(50, host.bounds.width * 0.18)
                let flung: Bool = abs(speed) > 350 && (speed < 0) == (moved < 0)
                let back: Bool = abs(speed) > 350 && (speed < 0) != (moved < 0)
                if (far || flung) && !back && has(step) {
                    turn(step, speed: speed, notify: true)
                } else {
                    settle(speed: speed)
                }
            default:
                settle(speed: 0)
            }
        }

        @objc func tapped(_ tap: UITapGestureRecognizer) {
            guard tap.state == .ended, let host, let web = page else { return }
            let step: Int = tap.location(in: host).x < host.bounds.width / 2 ? -1 : 1
            guard has(step) || animator != nil else { return }
            let scroll: UIScrollView = web.scrollView
            let point: CGPoint = tap.location(in: scroll)
            let zoom: CGFloat = max(scroll.zoomScale, 0.01)
            taps += 1
            let mine: Int = taps
            linkOpened = false
            let script = RSSPagerScript.hitsControl(x: point.x / zoom, y: point.y / zoom)
            web.evaluateJavaScript(script, in: nil, in: .defaultClient) { [weak self] (result: Result<Any, Error>) in
                switch result {
                case .success(let value):
                    if (value as? Bool) != true { self?.tapTurn(mine, step) }
                case .failure:
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { self?.tapTurn(mine, step) }
                }
            }
        }

        private func tapTurn(_ mine: Int, _ step: Int) {
            guard mine == taps, !linkOpened else { return }
            finishAnimation()
            guard has(step) else { return }
            turn(step, speed: 0, notify: true)
        }

        func gestureRecognizerShouldBegin(_ gesture: UIGestureRecognizer) -> Bool {
            guard gesture === pan, let host else { return true }
            if let scroll = page?.scrollView, scroll.zoomScale > scroll.minimumZoomScale + 0.001 { return false }
            let moved: CGPoint = pan.translation(in: host)
            if moved.x != 0 || moved.y != 0 { return abs(moved.x) > abs(moved.y) }
            let speed: CGPoint = pan.velocity(in: host)
            return abs(speed.x) > abs(speed.y)
        }

        func gestureRecognizer(_ gesture: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
            guard gesture === tap, let scroll = page?.scrollView else { return true }
            return !scroll.isDecelerating && !scroll.isDragging
        }

        func gestureRecognizer(_ gesture: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
            gesture === tap
        }

        func scrollViewDidScroll(_ scrollView: UIScrollView) {
            guard scrollView.zoomScale <= scrollView.minimumZoomScale + 0.001 else { return }
            let left: CGFloat = -scrollView.adjustedContentInset.left
            if scrollView.contentOffset.x != left { scrollView.contentOffset.x = left }
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
                linkOpened = true
                if scheme == "http" || scheme == "https" || scheme == "mailto" { onLink(url) }
            }
            decisionHandler(.cancel)
        }

        func webView(
            _ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
            for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures
        ) -> WKWebView? {
            linkOpened = true
            if let url = navigationAction.request.url {
                let scheme: String = url.scheme?.lowercased() ?? ""
                if scheme == "http" || scheme == "https" || scheme == "mailto" { onLink(url) }
            }
            return nil
        }
    }
}

enum RSSPagerScript {
    static func hitsControl(x: CGFloat, y: CGFloat) -> String {
        """
        (function(x, y) {
          var picked = window.getSelection ? String(window.getSelection()) : "";
          if (picked.length > 0) return true;
          var nodes = document.querySelectorAll("a[href], button, input, select, textarea, label, summary, video, audio, iframe");
          var slop = 12;
          for (var i = 0; i < nodes.length; i++) {
            var rects = nodes[i].getClientRects();
            for (var j = 0; j < rects.length; j++) {
              var r = rects[j];
              var left = r.left + window.scrollX - slop, right = r.right + window.scrollX + slop;
              var top = r.top + window.scrollY - slop, bottom = r.bottom + window.scrollY + slop;
              if (x >= left && x <= right && y >= top && y <= bottom) return true;
            }
          }
          return false;
        })(\(Double(x)), \(Double(y)))
        """
    }
}
