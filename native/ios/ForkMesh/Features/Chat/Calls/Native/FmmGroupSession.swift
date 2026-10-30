#if canImport(Fmm) && canImport(Opus)
import FMChat
import Fmm
import Foundation
import Network

@MainActor
protocol FmmGroupOwner: AnyObject {
    func fmmChat(_ kind: GroupCallE2eeKind, data: Data)
    func fmmTile(peer: String, feed: FmmTileFeed?)
    func fmmFace(peer: String, active: Bool, stale: Bool)
    func fmmAuth(peer: String, state: GroupCallAuthState)
    func fmmVerify()
    func fmmMoved()
    func fmmClosed(_ reason: String)
}

final class FmmListenerBridge: NSObject, FmmmobileListenerProtocol, @unchecked Sendable {
    weak var target: FmmGroupSession?

    func onEvent(_ e: FmmmobileEvent?) {
        guard let e else { return }
        target?.handle(e)
    }
}

enum FmmJoinResult {
    case joined(FmmGroupSession)
    case otherEngine
    case failed(String)
}

final class FmmPathMonitor: @unchecked Sendable {
    private let cellular = NWPathMonitor(requiredInterfaceType: .cellular)
    private let wifi = NWPathMonitor(requiredInterfaceType: .wifi)
    private let queue = DispatchQueue(label: "com.forkmesh.fmm.paths", qos: .utility)
    private weak var call: FmmmobileCall?
    private var cellularIndex = 0
    private var wifiUp = false
    private var path: Int64?

    init(call: FmmmobileCall) {
        self.call = call
    }

    func start() {
        cellular.pathUpdateHandler = { [weak self] update in
            self?.cellularIndex = update.status == .satisfied
                ? update.availableInterfaces.first { $0.type == .cellular }?.index ?? 0 : 0
            self?.decide()
        }
        wifi.pathUpdateHandler = { [weak self] update in
            self?.wifiUp = update.status == .satisfied
            self?.decide()
        }
        cellular.start(queue: queue)
        wifi.start(queue: queue)
    }

    private func decide() {
        guard let call else { return }
        if wifiUp && cellularIndex > 0 {
            guard path == nil else { return }
            var id: Int64 = 0
            if (try? call.addPathInterface(cellularIndex, backup: false, ret0_: &id)) != nil { path = id }
        } else if let current = path {
            path = nil
            try? call.abandonPath(current)
        }
    }

    func moved() {
        queue.async { [weak self] in
            self?.path = nil
            self?.decide()
        }
    }

    func stop() {
        cellular.cancel()
        wifi.cancel()
    }
}

final class FmmGroupSession: @unchecked Sendable {
    static let pollEvery = DispatchTimeInterval.milliseconds(5)
    static let faceEvery: TimeInterval = 1.0 / 24
    static let connectMs = 8000

    let call: FmmmobileCall
    let audio: FmmAudioEngine
    let peer: String
    private let bridge: FmmListenerBridge
    private let receive = DispatchQueue(label: "com.forkmesh.fmm.receive", qos: .userInitiated)
    private let lock = NSLock()
    private var poller: DispatchSourceTimer?
    private var remoteVideo: [Int: FmmRemoteVideo] = [:]
    private var feeds: [String: FmmTileFeed] = [:]
    private var faceShown: [String: (Bool, Bool)] = [:]
    private var faceAt = Date.distantPast
    private(set) var video: FmmVideoSender?
    private var paths: FmmPathMonitor?
    private var audioStream = 0
    private var closed = false
    weak var owner: FmmGroupOwner?

    private init(call: FmmmobileCall, bridge: FmmListenerBridge, peer: String) {
        self.call = call
        self.bridge = bridge
        self.peer = peer
        audio = FmmAudioEngine(call: call, voice: { NativeVoicePipeline.currentVoice })
    }

    static func join(access: GroupCallFmmAccess, identity: GroupCallIdentity, peer: String) async -> FmmJoinResult {
        await Task.detached(priority: .userInitiated) { () -> FmmJoinResult in
            let config = FmmmobileConfig()
            config.access = access.json
            config.call = identity.call
            config.peer = peer
            config.account = identity.acct
            config.identity = Data(identity.seed)
            config.startBitrate = 600_000
            config.maxBitrate = CallMediaTuning.groupVideoMaxBitrate + 200_000
            let bridge = FmmListenerBridge()
            var error: NSError?
            guard let call = FmmmobileNewCall(config, bridge, &error) else {
                return .failed(error?.localizedDescription ?? "The call engine could not start.")
            }
            let session = FmmGroupSession(call: call, bridge: bridge, peer: peer)
            bridge.target = session
            do {
                try call.connect(Self.connectMs)
            } catch {
                call.leave()
                return call.joinError() == GroupCallEngine.refusedByRoom ? .otherEngine : .failed(error.localizedDescription)
            }
            return .joined(session)
        }.value
    }

    func start(owner: FmmGroupOwner, sessionOwned: Bool) -> Bool {
        self.owner = owner
        var id = 0
        guard (try? call.publishAudio("microphone", ret0_: &id)) != nil else { return false }
        audioStream = id
        audio.publish(id)
        guard audio.start(sessionOwned: sessionOwned) else { return false }
        let made = DispatchSource.makeTimerSource(queue: receive)
        made.schedule(deadline: .now(), repeating: Self.pollEvery, leeway: .milliseconds(2))
        made.setEventHandler { [weak self] in self?.poll() }
        made.resume()
        poller = made
        let monitor = FmmPathMonitor(call: call)
        monitor.start()
        paths = monitor
        return true
    }

    func startVideo() -> FmmVideoSender? {
        guard video == nil, let made = FmmVideoSender(call: call) else { return video }
        do {
            try made.publish()
        } catch {
            made.stop()
            return nil
        }
        video = made
        return made
    }

    var muted: Bool {
        get { audio.muted }
        set { audio.muted = newValue }
    }

    func receiveChat(_ message: GroupCallE2eeMessage) {
        receive.async { [call] in
            try? call.handleChat(message.kind.rawValue, data: message.data, senderKid: message.senderKid, senderAcct: message.senderAcct)
        }
    }

    func peerLeft(_ peer: String) {
        receive.async { [call] in call.peerLeft(peer) }
    }

    func verification() -> FmmmobileVerification? { call.verification() }

    func takeLevels() -> CallVoiceLevel { audio.takeLevels() }

    func qualitySample() async -> CallQualitySample {
        let call = self.call
        return await withCheckedContinuation { (continuation: CheckedContinuation<CallQualitySample, Never>) in
            DispatchQueue.global(qos: .utility).async {
                var sample = CallQualitySample(at: Date().timeIntervalSince1970)
                if let stats = call.stats() {
                    if stats.rttUs > 0 { sample.rttMs = Double(stats.rttUs) / 1000 }
                    if stats.loss.isFinite { sample.lossPct = max(0, stats.loss) * 100 }
                    if stats.targetBitrate > 0 { sample.sendKbps = Double(stats.targetBitrate) / 1000 }
                }
                continuation.resume(returning: sample)
            }
        }
    }

    func markVerified(_ round: String) -> Bool { (try? call.markVerified(round)) != nil }

    func unverify() { call.unverify() }

    func handle(_ e: FmmmobileEvent) {
        let kind = Int64(e.kind)
        switch kind {
        case FmmmobileEventChat:
            guard let chat = GroupCallE2eeKind(rawValue: e.name), let data = e.data else { return }
            main { $0.fmmChat(chat, data: data) }
        case FmmmobileEventTrackAdded:
            if Int64(e.track) == FmmmobileKindAudio {
                audio.addRemote(e.stream)
                try? call.subscribe(e.stream, maxLayer: 0)
            } else {
                let peer = e.peer
                let feed = lock.withLock { () -> FmmTileFeed in
                    let made = feeds[peer] ?? FmmTileFeed(peer: peer)
                    feeds[peer] = made
                    remoteVideo[e.stream] = FmmRemoteVideo(stream: e.stream, peer: peer, feed: made)
                    return made
                }
                try? call.subscribe(e.stream, maxLayer: max(e.layers - 1, 0))
                main { $0.fmmTile(peer: peer, feed: feed) }
            }
        case FmmmobileEventTrackRemoved:
            audio.removeRemote(e.stream)
            let gone = lock.withLock { remoteVideo.removeValue(forKey: e.stream) }
            gone?.close()
        case FmmmobileEventKeyframeRequest:
            video?.request(e.stream)
        case FmmmobileEventRecover:
            video?.recover(e.stream, frame: e.frameID)
        case FmmmobileEventSendRepresentation:
            video?.represent(e.stream, rep: e.rep)
        case FmmmobileEventRepresentation:
            lock.withLock { remoteVideo[e.stream] }?.setRep(e.rep)
        case FmmmobileEventTarget:
            audio.retune(force: false)
        case FmmmobileEventAuth:
            let peer = e.peer
            let state = GroupCallAuthState(rawValue: e.state) ?? .pending
            main { $0.fmmAuth(peer: peer, state: state) }
        case FmmmobileEventVerify:
            main { $0.fmmVerify() }
        case FmmmobileEventMoved:
            audio.clearRemotes()
            paths?.moved()
            main { $0.fmmMoved() }
        case FmmmobileEventClosed:
            let text = e.text
            main { $0.fmmClosed(text) }
        default:
            break
        }
    }

    private func main(_ body: @escaping @MainActor (FmmGroupOwner) -> Void) {
        DispatchQueue.main.async { [weak self] in
            MainActor.assumeIsolated {
                guard let owner = self?.owner else { return }
                body(owner)
            }
        }
    }

    private func poll() {
        let streams = lock.withLock { remoteVideo }
        for (id, remote) in streams {
            while let frame = call.pollVideo(id) { remote.handle(frame) }
        }
        guard Date().timeIntervalSince(faceAt) >= Self.faceEvery else { return }
        faceAt = Date()
        var changes: [(String, Bool, Bool)] = []
        for remote in streams.values {
            remote.renderFace()
            let now = (remote.showsFace, remote.faceStale)
            let was = lock.withLock { faceShown[remote.peer] }
            if was == nil || was! != now {
                lock.withLock { faceShown[remote.peer] = now }
                changes.append((remote.peer, now.0, now.1))
            }
        }
        for change in changes {
            main { $0.fmmFace(peer: change.0, active: change.1, stale: change.2) }
        }
    }

    func close(sessionOwned: Bool) {
        guard !lock.withLock({ () -> Bool in
            let was = closed
            closed = true
            return was
        }) else { return }
        poller?.cancel()
        poller = nil
        paths?.stop()
        video?.stop()
        audio.stop(sessionOwned: sessionOwned)
        let streams = lock.withLock { remoteVideo }
        for remote in streams.values { remote.close() }
        let call = self.call
        DispatchQueue.global(qos: .utility).async { call.leave() }
        bridge.target = nil
    }
}
#endif
