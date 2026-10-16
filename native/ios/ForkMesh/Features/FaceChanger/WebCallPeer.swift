import AVFoundation
import FMBase
import FMChat
import Foundation
import Observation
import SwiftUI
import WebKit

enum CallAudioRoute {
    static var headphonesConnected: Bool {
        let worn: Set<AVAudioSession.Port> = [.headphones, .bluetoothA2DP, .bluetoothHFP, .bluetoothLE]
        return AVAudioSession.sharedInstance().currentRoute.outputs.contains { worn.contains($0.portType) }
    }
}

struct WebCallRecording {
    let data: Data
    let mime: String
    let durationMs: Int64
}

enum WebCallError: Error {
    case missingPage
    case notReady
    case failed(String)
}

@MainActor
@Observable
final class WebCallPeerFactory: DirectCallPeerMaking {
    private(set) var current: WebCallPeer?
    var voice: @MainActor () -> String = { "" }

    func makePeer(media: DirectCallKind, iceServers: [String]) async throws -> DirectCallPeer {
        if let previewed = current, previewed.previewing, media == .video {
            previewed.previewing = false
            previewed.dropRecording()
            do {
                _ = try await previewed.run(
                    "setup", [.string(media.rawValue), .array(iceServers.map { .string($0) }), .string(""), .string(voice())])
            } catch {
                previewed.close()
                throw error
            }
            return previewed
        }
        current?.close()
        let peer = WebCallPeer { [weak self] closed in
            if self?.current === closed { self?.current = nil }
        }
        current = peer
        do {
            try await peer.load()
            _ = try await peer.run(
                "setup", [.string(media.rawValue), .array(iceServers.map { .string($0) }), .string(""), .string(voice())])
        } catch {
            peer.close()
            throw error
        }
        return peer
    }

    var busy: Bool {
        guard let current else { return false }
        return !current.previewing
    }

    func preview(face: String, background: String, image: String, faceImage: String = "") async throws -> WebCallPeer {
        if let previewed = current, previewed.previewing { return previewed }
        guard !busy else { throw WebCallError.failed("busy") }
        let peer = WebCallPeer { [weak self] closed in
            if self?.current === closed { self?.current = nil }
        }
        peer.previewing = true
        current = peer
        do {
            try await peer.load()
            try await peer.preview(
                media: .video, voice: voice(), face: face, background: background, image: image, faceImage: faceImage)
        } catch {
            peer.close()
            throw error
        }
        return peer
    }

    func endPreview() {
        guard let previewed = current, previewed.previewing else { return }
        previewed.close()
    }

    static func requestAccess(for media: DirectCallKind) async -> Bool {
        let audio: Bool = await AVCaptureDevice.requestAccess(for: .audio)
        guard audio else { return false }
        guard media == .video else { return true }
        return await AVCaptureDevice.requestAccess(for: .video)
    }
}

@MainActor
private final class WebCallMessageProxy: NSObject, WKScriptMessageHandler {
    weak var target: WebCallPeer?

    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        guard let text = message.body as? String else { return }
        target?.receive(text)
    }
}

@MainActor
final class WebCallPeer: NSObject, DirectCallPeer, WKUIDelegate {
    static let origin = URL(string: "https://call.forkmesh.com/")

    var onIceCandidate: (@MainActor (JSONObject) -> Void)?
    var onLive: (@MainActor () -> Void)?
    var onClosed: (@MainActor () -> Void)?
    let webView: WKWebView
    private let proxy = WebCallMessageProxy()
    private let onDispose: @MainActor (WebCallPeer) -> Void
    private var ready = false
    private var readyWaiters: [CheckedContinuation<Void, Error>] = []
    private var pending: [Int: CheckedContinuation<JSONValue, Error>] = [:]
    private var nextId = 1
    private var closed = false
    private var voiceObserver: (@MainActor (String) -> Void)?
    private var depthCamera: CallDepthCamera?
    var previewing = false

    init(onDispose: @escaping @MainActor (WebCallPeer) -> Void) {
        self.onDispose = onDispose
        let configuration = WKWebViewConfiguration()
        configuration.allowsInlineMediaPlayback = true
        configuration.mediaTypesRequiringUserActionForPlayback = []
        configuration.userContentController.add(proxy, name: "fmCall")
        if CallDepthCamera.available {
            configuration.userContentController.addUserScript(
                WKUserScript(
                    source: "window.FmCallDepthCamera = { available: true };", injectionTime: .atDocumentStart,
                    forMainFrameOnly: true))
        }
        webView = WKWebView(frame: .zero, configuration: configuration)
        webView.isOpaque = false
        webView.backgroundColor = .black
        webView.scrollView.isScrollEnabled = false
        super.init()
        proxy.target = self
        webView.uiDelegate = self
    }

    func load() async throws {
        guard let url = Bundle.main.url(forResource: "direct-call", withExtension: "html"),
            let html = try? String(contentsOf: url, encoding: .utf8)
        else { throw WebCallError.missingPage }
        webView.loadHTMLString(html, baseURL: WebCallPeer.origin)
        if ready { return }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            readyWaiters.append(continuation)
        }
    }

    @discardableResult
    func run(_ name: String, _ args: [JSONValue] = []) async throws -> JSONValue {
        guard !closed else { throw WebCallError.notReady }
        let id = nextId
        nextId += 1
        let script = "window.fmCall(\(id), \(JSON.encode(.string(name))), \(JSON.encode(.array(args))))"
        return try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<JSONValue, Error>) in
            pending[id] = continuation
            webView.evaluateJavaScript(script) { [weak self] _, error in
                guard let error else { return }
                let reason = "\(error)"
                Task { @MainActor in
                    self?.pending.removeValue(forKey: id)?.resume(throwing: WebCallError.failed(reason))
                }
            }
        }
    }

    func receive(_ text: String) {
        let message: JSONObject = JSON.parseObject(text)
        switch message["event"]?.text ?? "" {
        case "ready":
            ready = true
            let waiters = readyWaiters
            readyWaiters = []
            for waiter in waiters { waiter.resume() }
        case "result":
            let id = message["id"]?.intValue ?? -1
            guard let continuation = pending.removeValue(forKey: id) else { return }
            if message["ok"]?.boolValue == true {
                continuation.resume(returning: message["value"] ?? .null)
            } else {
                continuation.resume(throwing: WebCallError.failed(message["error"]?.text ?? "call page error"))
            }
        case "ice":
            if let candidate = message["candidate"]?.objectValue { onIceCandidate?(candidate) }
        case "landmarks":
            let id = message["id"]?.intValue ?? 0
            guard let image = message["image"]?.text else { return }
            let landmarks = message["landmarks"]?.boolValue ?? true
            let mask = message["mask"]?.boolValue ?? false
            let accurate = message["accurate"]?.boolValue ?? false
            Task { [weak self] in
                let reply = await CallFaceLandmarks.detect(dataURL: image, landmarks: landmarks, mask: mask, accurate: accurate)
                self?.replyLandmarks(id: id, regions: reply)
            }
        case "camera":
            controlCamera(
                id: message["id"]?.intValue ?? 0, op: message["op"]?.text ?? "",
                landmarks: message["landmarks"]?.boolValue ?? false, mask: message["mask"]?.boolValue ?? false)
        case "frame-done":
            depthCamera?.frameDone(
                landmarks: message["landmarks"]?.boolValue ?? false, mask: message["mask"]?.boolValue ?? false)
        case "voice":
            voiceObserver?(message["id"]?.text ?? "")
        case "live":
            onLive?()
        case "closed":
            onClosed?()
        default:
            return
        }
    }

    private func replyLandmarks(id: Int, regions: JSONValue) {
        guard !closed else { return }
        webView.evaluateJavaScript(
            "window.fmCallLandmarks && window.fmCallLandmarks(\(id), \(JSON.encode(regions)))",
            completionHandler: nil)
    }

    private func controlCamera(id: Int, op: String, landmarks: Bool, mask: Bool) {
        switch op {
        case "start":
            guard !closed, CallDepthCamera.trueDepth != nil else {
                replyCamera(id: id, error: "no_camera")
                return
            }
            let camera = depthCamera ?? makeDepthCamera()
            camera.start(landmarks: landmarks, mask: mask) { [weak self] failure in
                Task { @MainActor in self?.replyCamera(id: id, error: failure) }
            }
        case "stop":
            guard let camera = depthCamera else {
                replyCamera(id: id, error: nil)
                return
            }
            camera.stop { [weak self] in
                Task { @MainActor in self?.replyCamera(id: id, error: nil) }
            }
        default:
            replyCamera(id: id, error: "unknown_camera_operation")
        }
    }

    private func makeDepthCamera() -> CallDepthCamera {
        let camera = CallDepthCamera()
        camera.onFrame = { [weak self] payload in
            Task { @MainActor in self?.pushFrame(payload) }
        }
        camera.onFailure = { [weak self] reason in
            Task { @MainActor in self?.cameraFailed(reason) }
        }
        depthCamera = camera
        return camera
    }

    private func replyCamera(id: Int, error: String?) {
        guard !closed, id > 0 else { return }
        var reply = JSONObject()
        reply["ok"] = .bool(error == nil)
        if let error { reply["error"] = .string(error) }
        webView.evaluateJavaScript(
            "window.fmCallCamera && window.fmCallCamera(\(id), \(JSON.encode(.object(reply))))", completionHandler: nil)
    }

    private func pushFrame(_ payload: String) {
        guard !closed else { return }
        webView.evaluateJavaScript("window.fmCallFrame && window.fmCallFrame(\(payload))", completionHandler: nil)
    }

    private func cameraFailed(_ reason: String) {
        guard !closed else { return }
        webView.evaluateJavaScript(
            "window.fmCallCameraFailed && window.fmCallCameraFailed(\(JSON.encode(.string(reason))))", completionHandler: nil)
    }

    func offer() async throws -> JSONObject {
        try await run("offer").objectValue ?? [:]
    }

    func answer(offer: JSONObject) async throws -> JSONObject {
        try await run("answer", [.object(offer)]).objectValue ?? [:]
    }

    func acceptAnswer(_ answer: JSONObject) async throws {
        try await run("accept", [.object(answer)])
    }

    func add(candidate: JSONObject) async {
        _ = try? await run("ice", [.object(candidate)])
    }

    func setMuted(_ muted: Bool) {
        Task { _ = try? await run("mute", [.bool(muted)]) }
    }

    func setCameraOff(_ off: Bool) {
        Task { _ = try? await run("camera", [.bool(off)]) }
    }

    func setFace(_ id: String) {
        Task { _ = try? await run("face", [.string(id)]) }
    }

    func setFace(_ id: String, image: String) {
        Task { _ = try? await run("face", [.string(id), .string(image)]) }
    }

    func faceState() async -> String {
        (try? await run("faceState"))?.text ?? ""
    }

    func setVoice(_ id: String) {
        Task { _ = try? await run("voice", [.string(id)]) }
    }

    func setBackground(_ id: String, image: String) {
        Task { _ = try? await run("background", [.string(id), .string(image)]) }
    }

    func backgroundThumbnails() async -> [String: String] {
        let list = (try? await run("backgroundList"))?.arrayValue ?? []
        var out: [String: String] = [:]
        for entry in list {
            guard let item = entry.objectValue, let id = item["id"]?.text, let thumb = item["thumb"]?.text, !thumb.isEmpty
            else { continue }
            out[id] = thumb
        }
        return out
    }

    func observeVoice(_ handler: @escaping @MainActor (String) -> Void) {
        voiceObserver = handler
    }

    @discardableResult
    func preview(
        media: DirectCallKind, voice: String, face: String = "", background: String = "", image: String = "",
        faceImage: String = ""
    ) async throws -> String {
        try await run(
            "preview",
            [.string(media.rawValue), .string(voice), .string(face), .string(background), .string(image), .string(faceImage)]
        ).text
    }

    @discardableResult
    func setMonitor(_ on: Bool) async -> Bool {
        (try? await run("monitor", [.bool(on)]))?.boolValue ?? false
    }

    func startRecording(face: String, background: String, voice: String) async throws {
        try await run("record", [.string(face), .string(background), .string(voice)])
    }

    func finishRecording() async throws -> WebCallRecording {
        defer { dropRecording() }
        let summary: JSONObject = try await run("recordStop").objectValue ?? [:]
        let size: Int = summary["size"]?.intValue ?? 0
        guard size > 0 else { throw WebCallError.failed("empty_recording") }
        var data = Data(capacity: size)
        while data.count < size {
            let text: String = try await run("recordChunk", [.int(Int64(data.count))]).text
            guard let chunk = Data(base64Encoded: text), !chunk.isEmpty else {
                throw WebCallError.failed("recording_read")
            }
            data.append(chunk)
        }
        guard data.count == size else { throw WebCallError.failed("recording_read") }
        let mime: String = (summary["mime"]?.text ?? "").split(separator: ";").first
            .map { $0.trimmingCharacters(in: .whitespaces).lowercased() } ?? ""
        return WebCallRecording(
            data: data, mime: mime.hasPrefix("video/") ? mime : "video/mp4",
            durationMs: Int64(summary["durationMs"]?.intValue ?? 0))
    }

    func dropRecording() {
        guard !closed else { return }
        Task { _ = try? await run("recordDrop") }
    }

    func close() {
        guard !closed else { return }
        webView.evaluateJavaScript("window.fmCall && window.fmCall(0, \"close\", [])", completionHandler: nil)
        closed = true
        depthCamera?.stop()
        depthCamera = nil
        let waiters = readyWaiters
        readyWaiters = []
        for waiter in waiters { waiter.resume(throwing: WebCallError.notReady) }
        let calls = pending
        pending = [:]
        for (_, continuation) in calls { continuation.resume(throwing: WebCallError.notReady) }
        webView.configuration.userContentController.removeScriptMessageHandler(forName: "fmCall")
        webView.stopLoading()
        onDispose(self)
    }

    func webView(
        _ webView: WKWebView, decideMediaCapturePermissionsFor origin: WKSecurityOrigin,
        initiatedBy frame: WKFrameInfo, type: WKMediaCaptureType
    ) async -> WKPermissionDecision {
        origin.host == WebCallPeer.origin?.host ? .grant : .deny
    }
}

struct WebCallSurface: UIViewRepresentable {
    let webView: WKWebView

    func makeUIView(context: Context) -> WKWebView { webView }

    func updateUIView(_ view: WKWebView, context: Context) {}
}
