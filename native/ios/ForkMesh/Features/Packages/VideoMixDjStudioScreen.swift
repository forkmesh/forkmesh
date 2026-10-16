import FMApi
import FMBase
import SwiftUI
import UIKit
import WebKit

struct VideoMixDjStudioScreen: View {
    @State private var page = VideoMixDjStudioPage()

    var body: some View {
        Group {
            if page.url != nil {
                VideoMixDjStudioWebView(page: page)
                    .ignoresSafeArea()
            } else {
                ContentUnavailableView(
                    "Video Mix DJ Studio unavailable", systemImage: "music.note.list",
                    description: Text("Connect to a ForkMesh server to use it."))
            }
        }
        .overlay {
            if page.loading { ProgressView() }
        }
        .overlay(alignment: .bottom) {
            if !page.problem.isEmpty {
                Label(page.problem, systemImage: "exclamationmark.triangle")
                    .font(.footnote)
                    .padding(FmSpace.x3)
                    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
                    .padding(FmSpace.x4)
                    .fmMessageTools(page.problem)
            }
        }
        .toolbar(.hidden, for: .navigationBar)
        .toolbar(.hidden, for: .tabBar)
        .statusBarHidden()
        .onDisappear { page.pause() }
    }
}

@MainActor
@Observable
final class VideoMixDjStudioPage: NSObject, WKUIDelegate, WKNavigationDelegate {
    static let route = "/dashboard/video-mix-dj-studio"

    var loading = true
    var problem = ""

    @ObservationIgnored let url: URL?
    @ObservationIgnored private(set) var webView: WKWebView?

    override init() {
        url = AppServices.current.flatMap { service in
            var components = URLComponents(url: service.api.url(VideoMixDjStudioPage.route), resolvingAgainstBaseURL: false)
            components?.queryItems = [URLQueryItem(name: "presentation", value: "studio")]
            return components?.url
        }.flatMap { $0.scheme == "https" ? $0 : nil }
        super.init()
    }

    func makeWebView() -> WKWebView {
        if let webView { return webView }
        let configuration = WKWebViewConfiguration()
        configuration.allowsInlineMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = []
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.uiDelegate = self
        view.navigationDelegate = self
        view.allowsBackForwardNavigationGestures = false
        webView = view
        if let url { view.load(URLRequest(url: url)) }
        return view
    }

    func pause() {
        webView?.evaluateJavaScript("document.querySelector('[data-vmdj=\\\"pause-both\\\"]')?.click()", completionHandler: nil)
    }

    func webView(
        _ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction
    ) async -> WKNavigationActionPolicy {
        guard let target = navigationAction.request.url else { return .cancel }
        if target.scheme == "about" { return .allow }
        if target.scheme == "https", target.host == url?.host { return .allow }
        if target.scheme == "https", target.host == "www.youtube-nocookie.com", navigationAction.targetFrame?.isMainFrame == false { return .allow }
        if navigationAction.navigationType == .linkActivated, target.scheme == "https" || target.scheme == "http" {
            _ = await UIApplication.shared.open(target)
        }
        return .cancel
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        loading = false
    }

    func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
        loading = false
        problem = "Video Mix DJ Studio could not load. Check your connection and try again."
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        loading = false
        problem = "Video Mix DJ Studio could not load. Check your connection and try again."
    }
}

private struct VideoMixDjStudioWebView: UIViewRepresentable {
    let page: VideoMixDjStudioPage

    func makeUIView(context: Context) -> WKWebView {
        page.makeWebView()
    }

    func updateUIView(_ view: WKWebView, context: Context) {}
}
