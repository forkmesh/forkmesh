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
            .simultaneousGesture(
                DragGesture(minimumDistance: 30).onEnded { value in
                    guard abs(value.translation.width) > abs(value.translation.height) else { return }
                    move(value.translation.width < 0 ? 1 : -1)
                })
            .navigationTitle("")
            .navigationBarTitleDisplayMode(.inline)
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
            RSSArticleWebView(
                html: model.page(for: article, reader: reader), articleKey: "\(article.id)|\(reader)",
                onLink: { (url: URL) in openLink(url) })
                .ignoresSafeArea(edges: .bottom)
                .accessibilityIdentifier("rss-article-body")
        } else {
            Text("This article is no longer on this iPhone.")
                .foregroundStyle(.secondary)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
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

    private func move(_ step: Int) {
        guard let index else { return }
        let next: Int = index + step
        guard next >= 0 && next < list.count else { return }
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

    private func openLink(_ url: URL) {
        let scheme: String = url.scheme?.lowercased() ?? ""
        if (scheme == "http" || scheme == "https") && model.settings.openLinksInApp {
            browser = RSSBrowserLink(url: url)
        } else if scheme == "http" || scheme == "https" || scheme == "mailto" {
            openURL(url)
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

    func makeCoordinator() -> Coordinator { Coordinator(onLink: onLink) }

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
        context.coordinator.load(html, key: articleKey, into: view)
        return view
    }

    func updateUIView(_ view: WKWebView, context: Context) {
        context.coordinator.onLink = onLink
        context.coordinator.load(html, key: articleKey, into: view)
    }

    final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate {
        var onLink: (URL) -> Void
        private var html = ""
        private var key = ""

        init(onLink: @escaping (URL) -> Void) {
            self.onLink = onLink
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
