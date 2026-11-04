import FMApi
import SwiftUI
import UIKit
import WebKit

struct MeetingSaveItem: Identifiable {
    let id = UUID()
    let items: [Any]
    let file: URL?

    static func make(name: String, text: String) -> MeetingSaveItem {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("meeting-transcripts", isDirectory: true)
        let file = folder.appendingPathComponent(name, isDirectory: false)
        guard file.deletingLastPathComponent().standardizedFileURL.path == folder.standardizedFileURL.path else {
            return MeetingSaveItem(items: [text], file: nil)
        }
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try Data(text.utf8).write(to: file, options: [.atomic, .completeFileProtection])
            return MeetingSaveItem(items: [file], file: file)
        } catch {
            return MeetingSaveItem(items: [text], file: nil)
        }
    }

    func finish() {
        guard let file else { return }
        try? Data().write(to: file, options: [.atomic, .completeFileProtection])
    }
}

@MainActor
private final class MeetingMessageProxy: NSObject, WKScriptMessageHandler {
    weak var target: MeetingHost?

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        target?.receive(message)
    }
}

@MainActor
final class MeetingHost: NSObject, WKNavigationDelegate, WKUIDelegate {
    static let saveHandler = "fmMeetingSave"

    let origin: URL
    let page: URL
    let webView: WKWebView
    var onSave: (@MainActor (MeetingSaveItem) -> Void)?
    private let proxy = MeetingMessageProxy()
    private var closed = false

    init(origin: URL, room: String, key: String) {
        self.origin = origin
        page = MeetingLink.pageURL(origin: origin, room: room, key: key)
        let configuration = WKWebViewConfiguration()
        configuration.allowsInlineMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = []
        configuration.websiteDataStore = .default()
        configuration.userContentController.add(proxy, name: MeetingHost.saveHandler)
        webView = WKWebView(frame: .zero, configuration: configuration)
        webView.isOpaque = false
        webView.backgroundColor = .black
        webView.allowsLinkPreview = false
        super.init()
        proxy.target = self
        webView.navigationDelegate = self
        webView.uiDelegate = self
    }

    func load() {
        guard !closed else { return }
        webView.load(URLRequest(url: page))
    }

    func close() {
        guard !closed else { return }
        closed = true
        onSave = nil
        webView.configuration.userContentController.removeScriptMessageHandler(forName: MeetingHost.saveHandler)
        webView.stopLoading()
        webView.navigationDelegate = nil
        webView.uiDelegate = nil
        if let blank = URL(string: "about:blank") { webView.load(URLRequest(url: blank)) }
    }

    private func sameOrigin(_ source: WKSecurityOrigin) -> Bool {
        MeetingLink.sameOrigin(
            scheme: source.protocol, host: source.host, port: source.port == 0 ? nil : source.port, origin: origin)
    }

    func receive(_ message: WKScriptMessage) {
        guard !closed, message.name == MeetingHost.saveHandler, message.frameInfo.isMainFrame,
            sameOrigin(message.frameInfo.securityOrigin),
            let body = message.body as? [String: Any], let text = body["text"] as? String
        else { return }
        let name = MeetingLink.saveName(body["name"] as? String)
        onSave?(MeetingSaveItem.make(name: name, text: MeetingLink.saveText(text)))
    }

    func webView(
        _ webView: WKWebView, decideMediaCapturePermissionsFor origin: WKSecurityOrigin,
        initiatedBy frame: WKFrameInfo, type: WKMediaCaptureType
    ) async -> WKPermissionDecision {
        !closed && frame.isMainFrame && sameOrigin(origin) ? .grant : .deny
    }

    func webView(
        _ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction
    ) async -> WKNavigationActionPolicy {
        guard !closed, let url = navigationAction.request.url else { return .cancel }
        let scheme: String = url.scheme?.lowercased() ?? ""
        if scheme == "about" { return .allow }
        let main: Bool = navigationAction.targetFrame?.isMainFrame ?? false
        if main && MeetingLink.isMeetingPage(url, origin: origin) { return .allow }
        if scheme == "https" && (main || navigationAction.targetFrame == nil) {
            UIApplication.shared.open(url, options: [:], completionHandler: nil)
        }
        return .cancel
    }

    func webView(
        _ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
        for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures
    ) -> WKWebView? {
        if !closed, let url = navigationAction.request.url, url.scheme?.lowercased() == "https" {
            UIApplication.shared.open(url, options: [:], completionHandler: nil)
        }
        return nil
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        load()
    }
}

struct MeetingScreen: View {
    let room: String
    let key: String
    @Environment(\.fmPalette) private var fm
    @State private var host: MeetingHost?
    @State private var save: MeetingSaveItem?
    @State private var unavailable = false
    @State private var heldIdle: Bool?

    var body: some View {
        VStack(spacing: 0) {
            Label("End-to-end encrypted · the key stays in this link", systemImage: "lock.fill")
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(2)
                .multilineTextAlignment(.center)
                .frame(maxWidth: .infinity)
                .padding(.horizontal, 16)
                .padding(.vertical, 6)
                .accessibilityIdentifier("meeting-caption")
            if let host {
                WebCallSurface(webView: host.webView)
                    .accessibilityIdentifier("meeting-page")
            } else if unavailable {
                FmEmptyState(
                    icon: "error_outline", title: "This meeting can't open here",
                    message: "The active project has no secure web address. Open the meeting link in your browser.")
            } else {
                fm.canvas
            }
        }
        .background(host == nil ? fm.canvas : Color.black)
        .navigationTitle("Meeting")
        .navigationBarTitleDisplayMode(.inline)
        .onAppear(perform: start)
        .onDisappear(perform: stop)
        .sheet(item: $save) { (item: MeetingSaveItem) in
            AgentActivityView(items: item.items) { item.finish() }
        }
    }

    private func start() {
        if heldIdle == nil {
            heldIdle = UIApplication.shared.isIdleTimerDisabled
            UIApplication.shared.isIdleTimerDisabled = true
        }
        guard host == nil else { return }
        guard let base = AppServices.current?.api.url("/"), let origin = MeetingLink.origin(of: base) else {
            unavailable = true
            return
        }
        unavailable = false
        let made = MeetingHost(origin: origin, room: room, key: key)
        made.onSave = { (item: MeetingSaveItem) in save = item }
        host = made
        made.load()
    }

    private func stop() {
        host?.close()
        host = nil
        if let heldIdle {
            UIApplication.shared.isIdleTimerDisabled = heldIdle
            self.heldIdle = nil
        }
    }
}
