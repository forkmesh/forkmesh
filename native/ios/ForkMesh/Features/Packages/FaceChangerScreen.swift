import AVFoundation
import FMApi
import FMBase
import FMPackages
import SwiftUI
import UIKit
import WebKit

struct FaceChangerScreen: View {
    @State private var page = FaceChangerPage()

    var body: some View {
        Group {
            if page.url != nil {
                FaceChangerWebView(page: page)
                    .ignoresSafeArea(edges: .bottom)
            } else {
                ContentUnavailableView(
                    "Face Changer unavailable", systemImage: "theatermasks",
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
        .navigationTitle("Face Changer")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Button {
                    page.reload()
                } label: {
                    Label("Reload", systemImage: "arrow.clockwise")
                }
                .disabled(page.url == nil)
            }
        }
        .sheet(item: $page.sharing) { (item: FaceChangerShareItem) in
            AgentActivityView(items: [item.url], onDone: { page.sharing = nil })
                .presentationDetents([.medium, .large])
        }
        .task { await page.askForDevices() }
        .onDisappear { page.pause() }
    }
}

struct FaceChangerShareItem: Identifiable {
    let url: URL
    var id: String { url.path }
}

@MainActor
@Observable
final class FaceChangerPage: NSObject, WKUIDelegate, WKNavigationDelegate {
    static let route = "/dashboard/face-changer"

    var loading = true
    var problem = ""
    var sharing: FaceChangerShareItem?

    @ObservationIgnored let url: URL?
    @ObservationIgnored private(set) var webView: WKWebView?
    @ObservationIgnored private let proxy = FaceChangerMessageProxy()
    @ObservationIgnored private let writer = ShareTransferWriter(
        folder: FileManager.default.temporaryDirectory.appendingPathComponent("face-changer-share", isDirectory: true))

    override init() {
        url = AppServices.current.map { $0.api.url(FaceChangerPage.route) }.flatMap { $0.scheme == "https" ? $0 : nil }
        super.init()
        proxy.target = self
    }

    func makeWebView() -> WKWebView {
        if let webView { return webView }
        let configuration = WKWebViewConfiguration()
        configuration.allowsInlineMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = []
        configuration.userContentController.add(proxy, name: "fmShare")
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.uiDelegate = self
        view.navigationDelegate = self
        view.allowsBackForwardNavigationGestures = false
        webView = view
        if let url { view.load(URLRequest(url: url)) }
        return view
    }

    func askForDevices() async {
        if AVCaptureDevice.authorizationStatus(for: .video) == .notDetermined {
            _ = await AVCaptureDevice.requestAccess(for: .video)
        }
        if AVCaptureDevice.authorizationStatus(for: .audio) == .notDetermined {
            _ = await AVCaptureDevice.requestAccess(for: .audio)
        }
        if AVCaptureDevice.authorizationStatus(for: .video) == .denied {
            problem = "Camera access is off for ForkMesh. Turn it on in Settings to use Face Changer."
        }
    }

    func reload() {
        problem = ""
        loading = true
        webView?.reload()
    }

    func pause() {
        webView?.evaluateJavaScript("document.querySelector('[data-fc=\"stop\"]')?.click()", completionHandler: nil)
    }

    func receive(_ text: String, from origin: WKSecurityOrigin) {
        guard isOwnOrigin(origin), let finished = writer.receive(text) else { return }
        sharing = FaceChangerShareItem(url: finished.url)
        FmHaptics.commit()
    }

    private func isOwnOrigin(_ origin: WKSecurityOrigin) -> Bool {
        guard let url else { return false }
        return origin.protocol == "https" && origin.host == url.host
    }

    func webView(
        _ webView: WKWebView, decideMediaCapturePermissionsFor origin: WKSecurityOrigin,
        initiatedBy frame: WKFrameInfo, type: WKMediaCaptureType
    ) async -> WKPermissionDecision {
        isOwnOrigin(origin) ? .grant : .deny
    }

    func webView(
        _ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction
    ) async -> WKNavigationActionPolicy {
        guard let target = navigationAction.request.url else { return .cancel }
        if target.scheme == "blob" || target.scheme == "about" { return .allow }
        if target.scheme == "https", target.host == url?.host { return .allow }
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
        problem = "Face Changer could not load. Check your connection and reload."
    }

    func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
        loading = false
        problem = "Face Changer could not load. Check your connection and reload."
    }
}

@MainActor
private final class FaceChangerMessageProxy: NSObject, WKScriptMessageHandler {
    weak var target: FaceChangerPage?

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let text = message.body as? String, message.frameInfo.isMainFrame else { return }
        target?.receive(text, from: message.frameInfo.securityOrigin)
    }
}

private struct FaceChangerWebView: UIViewRepresentable {
    let page: FaceChangerPage

    func makeUIView(context: Context) -> WKWebView {
        page.makeWebView()
    }

    func updateUIView(_ view: WKWebView, context: Context) {}
}
